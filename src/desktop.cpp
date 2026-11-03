#include "desktop.h"

#include "log.h"
#include "util.h"

namespace {

const wchar_t* kWindowClass = L"LiveWallDesktopWindow";

// Undocumented message that makes Explorer (re)create the WorkerW layer behind
// the desktop icons. 0x052C is used by every live wallpapers implementation in
// existence; there is no supported alternative.
constexpr UINT kSpawnWorkerW = 0x052C;

struct WorkerSearch {
    HWND behindIcons = nullptr;
};

BOOL CALLBACK FindBehindIconsProc(HWND top, LPARAM param) {
    // The wallpaper WorkerW is the one that follows the window owning
    // SHELLDLL_DefView in Z-order.
    if (FindWindowExW(top, nullptr, L"SHELLDLL_DefView", nullptr)) {
        HWND worker = FindWindowExW(nullptr, top, L"WorkerW", nullptr);
        if (worker) {
            reinterpret_cast<WorkerSearch*>(param)->behindIcons = worker;
            return FALSE;
        }
    }
    return TRUE;
}

HWND FindWorkerWBehindIcons() {
    WorkerSearch search;
    EnumWindows(&FindBehindIconsProc, reinterpret_cast<LPARAM>(&search));
    return search.behindIcons;
}

HWND ResolveWallpaperLayer() {
    HWND progman = FindWindowW(L"Progman", nullptr);
    if (!progman) return nullptr;

    // Strategy 1: the classic nudge. On most Windows 10/11 builds this makes
    // Explorer spawn a WorkerW that covers the desktop and sits below the icons.
    SendMessageTimeoutW(progman, kSpawnWorkerW, 0, 0, SMTO_NORMAL, 1000, nullptr);
    if (HWND worker = FindWorkerWBehindIcons()) return worker;

    // Strategy 2: wParam = 0xD, lParam = 1. Some builds only create the layer
    // with these arguments, notably when desktop icons are hidden.
    SendMessageTimeoutW(progman, kSpawnWorkerW, 0x0D, 0x01, SMTO_NORMAL, 1000, nullptr);
    if (HWND worker = FindWorkerWBehindIcons()) return worker;

    // Strategy 3: WorkerW exists as a direct child of Progman on older builds.
    if (HWND worker = FindWindowExW(progman, nullptr, L"WorkerW", nullptr)) return worker;

    // Strategy 4: last resort - Progman itself. The wallpaper window is pushed
    // to the bottom of the Z-order so the icon view stays visible, but Explorer
    // may paint the static wallpaper over us.
    LogPrintf("WorkerW not found, falling back to Progman as the wallpaper parent");
    return progman;
}

}  // namespace

DesktopHost::DesktopHost(DesktopSink* sink) : m_sink(sink) {}

DesktopHost::~DesktopHost() {
    Stop();
}

bool DesktopHost::RegisterWindowClass() {
    static bool registered = false;
    if (registered) return true;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &DesktopHost::WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kWindowClass;

    if (!RegisterClassExW(&wc)) {
        LogPrintf("RegisterClassEx(desktop window) failed: %lu", GetLastError());
        return false;
    }
    registered = true;
    return true;
}

BOOL CALLBACK DesktopHost::MonitorProc(HMONITOR monitor, HDC, LPRECT, LPARAM param) {
    auto* self = reinterpret_cast<DesktopHost*>(param);
    self->m_monitors.push_back(monitor);
    return TRUE;
}

bool DesktopHost::Start(std::string& error) {
    if (!RegisterWindowClass()) {
        error = "cannot register the wallpaper window class";
        return false;
    }

    m_stopping = false;
    m_needsRebuild = false;
    m_worker = ResolveWallpaperLayer();
    if (!m_worker) {
        error = "the desktop wallpaper layer was not found (is Explorer running?)";
        return false;
    }

    m_monitors.clear();
    EnumDisplayMonitors(nullptr, nullptr, &DesktopHost::MonitorProc,
                        reinterpret_cast<LPARAM>(this));
    if (m_monitors.empty()) {
        error = "no monitors detected";
        return false;
    }

    for (HMONITOR monitor : m_monitors) {
        if (HWND window = CreateForMonitor(monitor)) {
            m_windows.push_back(window);
        }
    }

    if (m_windows.empty()) {
        error = "cannot create wallpaper windows";
        return false;
    }

    LogPrintf("desktop host started: %zu window(s), %zu monitor(s), parent=0x%p",
              m_windows.size(), m_monitors.size(), m_worker);
    m_started = true;
    return true;
}

HWND DesktopHost::CreateForMonitor(HMONITOR monitor) {
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(monitor, &info)) return nullptr;

    const RECT& rect = info.rcMonitor;
    const int width = rect.right - rect.left;
    const int height = rect.bottom - rect.top;
    if (width <= 0 || height <= 0) return nullptr;

    HWND window = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, kWindowClass, L"",
                                  WS_POPUP, 0, 0, width, height, nullptr, nullptr,
                                  GetModuleHandleW(nullptr), this);
    if (!window) {
        LogPrintf("CreateWindowEx(wallpaper) failed: %lu", GetLastError());
        return nullptr;
    }

    // SetParent on a visible top-level window is not reliable: hide first.
    ShowWindow(window, SW_HIDE);

    if (!SetParent(window, m_worker)) {
        LogPrintf("SetParent(wallpaper) failed: %lu", GetLastError());
        DestroyWindow(window);
        return nullptr;
    }

    LONG_PTR style = GetWindowLongPtrW(window, GWL_STYLE);
    style = (style & ~static_cast<LONG_PTR>(WS_POPUP)) | WS_CHILD;
    SetWindowLongPtrW(window, GWL_STYLE, style);

    POINT origin{rect.left, rect.top};
    if (!ScreenToClient(m_worker, &origin)) {
        DestroyWindow(window);
        return nullptr;
    }

    SetWindowPos(window, nullptr, origin.x, origin.y, width, height,
                 SWP_NOACTIVATE | SWP_FRAMECHANGED | SWP_SHOWWINDOW);

    if (m_worker == FindWindowW(L"Progman", nullptr)) {
        // Keep the fallback window behind SHELLDLL_DefView, otherwise it would
        // cover the desktop icons.
        SetWindowPos(window, HWND_BOTTOM, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }

    return window;
}

void DesktopHost::Stop() {
    m_stopping = true;
    m_started = false;
    for (HWND window : m_windows) {
        if (IsWindow(window)) DestroyWindow(window);
    }
    m_windows.clear();
    m_monitors.clear();
    m_worker = nullptr;
    m_stopping = false;
}

bool DesktopHost::Rebuild(std::string& error) {
    Stop();
    return Start(error);
}

bool DesktopHost::CheckHealth() {
    if (m_stopping || !m_started) return false;
    if (m_needsRebuild) return true;

    if (!m_worker || !IsWindow(m_worker)) {
        LogPrintf("wallpaper layer is gone");
        m_needsRebuild = true;
        return true;
    }

    for (HWND window : m_windows) {
        if (!IsWindow(window) || GetParent(window) != m_worker) {
            LogPrintf("wallpaper window 0x%p was lost or reparented", window);
            m_needsRebuild = true;
            return true;
        }
    }

    std::vector<HMONITOR> current;
    EnumDisplayMonitors(
        nullptr, nullptr,
        [](HMONITOR monitor, HDC, LPRECT, LPARAM param) -> BOOL {
            reinterpret_cast<std::vector<HMONITOR>*>(param)->push_back(monitor);
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&current));

    if (current.size() != m_monitors.size()) {
        LogPrintf("monitor count changed: %zu -> %zu", m_monitors.size(), current.size());
        m_needsRebuild = true;
        return true;
    }

    return false;
}

LRESULT CALLBACK DesktopHost::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    DesktopHost* self =
        reinterpret_cast<DesktopHost*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    if (msg == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lp);
        self = static_cast<DesktopHost*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }

    switch (msg) {
        case WM_NCHITTEST:
            // Never intercept mouse input: clicks belong to the icon view.
            return HTTRANSPARENT;

        case WM_ERASEBKGND:
            return 1;

        case WM_SIZE:
            if (self && !self->m_stopping && self->m_sink && wp != SIZE_MINIMIZED) {
                self->m_sink->OnWallpaperResize(hwnd, LOWORD(lp), HIWORD(lp));
            }
            return 0;

        case WM_DISPLAYCHANGE:
            if (self) self->m_needsRebuild = true;
            return 0;

        case WM_DESTROY:
            if (self && !self->m_stopping) {
                self->m_needsRebuild = true;
                self->m_worker = nullptr;
            }
            return 0;

        default:
            break;
    }

    return DefWindowProcW(hwnd, msg, wp, lp);
}
