#include "app.h"

#include "log.h"
#include "util.h"

#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <wtsapi32.h>

#include <cstdlib>

namespace {

const wchar_t* kMessageClass = L"LiveWallMessageWindow";
const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
const wchar_t* kRunValue = L"LiveWall";
const wchar_t* kWindowTitle = L"LiveWall";
const UINT kTrayIconId = 1;
const UINT kIconResourceId = 101;

UINT g_taskbarCreated = 0;

std::wstring MakeAbsolute(const std::wstring& path) {
    if (path.empty()) return {};
    wchar_t buffer[MAX_PATH * 4];
    const DWORD length = GetFullPathNameW(path.c_str(),
                                           static_cast<DWORD>(std::size(buffer)), buffer, nullptr);
    if (length == 0 || length >= std::size(buffer)) return path;
    return std::wstring(buffer, length);
}

Renderer::Mode ToMode(const std::wstring& value) {
    if (value == L"fill") return Renderer::Mode::Fill;
    if (value == L"stretch") return Renderer::Mode::Stretch;
    return Renderer::Mode::Fit;
}

void ShowError(const std::wstring& text) {
    MessageBoxW(nullptr, text.c_str(), kWindowTitle, MB_ICONERROR | MB_OK);
}

}  // namespace

int App::Run(HINSTANCE instance, int argc, wchar_t** argv) {
    m_instance = instance;

    // The manifest asks for PerMonitorV2; this call is a safety net for builds
    // where the manifest is not embedded.
#ifdef DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
#endif

    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool comInitialized = SUCCEEDED(com);
    if (!comInitialized && com != RPC_E_CHANGED_MODE) {
        ShowError(L"Не удалось инициализировать COM.");
        return 1;
    }

    LogInit();
    LogPrintf("livewall starting (pid %lu)", GetCurrentProcessId());

    // Single instance: a second launch just forwards the media path.
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\LiveWall.SingleInstance");
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        if (argc > 1) {
            if (HWND running = FindWindowW(kMessageClass, nullptr)) {
                int index = 1;
                while (index < argc && argv[index][0] == L'-') ++index;
                const std::wstring path = index < argc ? MakeAbsolute(argv[index]) : std::wstring();
                if (path.empty()) {
                    if (mutex) CloseHandle(mutex);
                    LogShutdown();
                    return 0;
                }
                COPYDATASTRUCT data{};
                data.dwData = 1;
                data.cbData = static_cast<DWORD>((path.size() + 1) * sizeof(wchar_t));
                data.lpData = const_cast<wchar_t*>(path.c_str());
                SendMessageW(running, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&data));
            }
        }
        if (mutex) CloseHandle(mutex);
        LogShutdown();
        return 0;
    }

    if (!Init(instance, argc, argv)) {
        Shutdown();
        if (mutex) CloseHandle(mutex);
        if (comInitialized) CoUninitialize();
        LogShutdown();
        return 1;
    }

    LARGE_INTEGER frequency;
    LARGE_INTEGER start;
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&start);

    auto now = [&]() {
        LARGE_INTEGER counter;
        QueryPerformanceCounter(&counter);
        return static_cast<double>(counter.QuadPart - start.QuadPart) /
               static_cast<double>(frequency.QuadPart);
    };

    m_lastFrameTime = now();
    m_lastHealthCheck = m_lastFrameTime;
    m_lastPauseCheck = m_lastFrameTime;

    for (;;) {
        MSG message;
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) {
                m_quitting = true;
                break;
            }
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (m_quitting) break;

        const double time = now();
        const double delta = std::min(time - m_lastFrameTime, 0.1);

        if (time - m_lastPauseCheck >= 0.25 || m_pauseStateDirty) {
            m_lastPauseCheck = time;
            m_pauseStateDirty = false;
            m_paused = IsPaused();
            if (m_paused != m_wasPaused) {
                m_wasPaused = m_paused;
                m_dirty = true;
                UpdateTrayIcon();
                LogPrintf(m_paused ? "rendering paused" : "rendering resumed");
            }
        }

        if (time - m_lastHealthCheck >= 2.0) {
            m_lastHealthCheck = time;
            if (m_host && m_host->CheckHealth()) ReloadDesktop();
        }

        if (m_host && m_host->NeedsRebuild()) {
            m_host->ClearRebuildFlag();
            ReloadDesktop();
        }

        const double interval = m_settings.fps > 0 ? 1.0 / m_settings.fps : 0.0;

        if (!m_paused && time - m_lastFrameTime >= interval) {
            m_lastFrameTime = time;
            if (m_source && m_source->IsAnimated()) {
                if (m_source->Advance(delta)) {
                    m_renderer.UploadFrame(m_source->CurrentFrame());
                    m_dirty = true;
                }
            }
            if (m_dirty) {
                if (m_renderer.IsDeviceLost()) {
                    LogPrintf("recreating the renderer after device loss");
                    m_renderer.Shutdown();
                    std::string error;
                    if (!m_renderer.Init(error)) {
                        LogPrintf("renderer reinit failed: %s", error.c_str());
                        m_quitting = true;
                        break;
                    }
                    m_renderer.SetMode(ToMode(m_settings.mode));
                    if (m_source) m_renderer.UploadFrame(m_source->CurrentFrame());
                    ReloadDesktop();
                } else {
                    m_dirty = !m_renderer.Render();
                }
            }
        } else if (m_dirty && m_paused) {
            // A resize while paused still needs one draw, otherwise the window
            // shows garbage from the resized swap chain.
            m_dirty = !m_renderer.Render();
        }

        double remaining = interval - (time - m_lastFrameTime);
        DWORD timeout = static_cast<DWORD>(
            std::clamp(remaining * 1000.0, 1.0, 16.0));
        MsgWaitForMultipleObjectsEx(0, nullptr, timeout, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    }

    Shutdown();
    if (mutex) CloseHandle(mutex);
    if (comInitialized) CoUninitialize();
    LogPrintf("livewall stopped");
    LogShutdown();
    return 0;
}

bool App::Init(HINSTANCE instance, int argc, wchar_t** argv) {
    m_instance = instance;
    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    m_settings.Load();

    bool mediaFromCommandLine = false;
    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        if (arg == L"--mode" && i + 1 < argc) {
            m_settings.mode = argv[++i];
        } else if (arg == L"--fps" && i + 1 < argc) {
            m_settings.fps = _wtoi(argv[++i]);
        } else if (arg == L"--autostart") {
            // Started from the Run key: nothing to override.
        } else if (!arg.empty() && arg[0] != L'-') {
            m_settings.media = MakeAbsolute(arg);
            mediaFromCommandLine = true;
        }
    }

    if (m_settings.fps < 1 || m_settings.fps > 240) m_settings.fps = 60;
    if (m_settings.mode != L"fit" && m_settings.mode != L"fill" &&
        m_settings.mode != L"stretch") {
        m_settings.mode = L"fit";
    }

    if (!CreateMessageWindow()) {
        ShowError(L"Не удалось создать служебное окно.");
        return false;
    }

    std::string error;
    if (!m_renderer.Init(error)) {
        ShowError(L"Не удалось инициализировать Direct3D 11:\n" + Utf8ToWide(error));
        return false;
    }
    m_renderer.SetMode(ToMode(m_settings.mode));

    m_host = std::make_unique<DesktopHost>(this);

    CreateTrayIcon();

    if (!m_settings.media.empty()) {
        if (FileExists(m_settings.media)) {
            LoadMedia(m_settings.media);
        } else {
            LogPrintf("media file is missing: %s", WideToUtf8(m_settings.media).c_str());
            if (mediaFromCommandLine) {
                ShowError(L"Файл не найден:\n" + m_settings.media);
            }
            m_settings.media.clear();
            m_settings.Save();
        }
    }

    if (m_source) {
        ReloadDesktop();
    } else {
        // Do not touch the desktop until there is something to show: a black
        // wallpaper with no way to tell why would be worse than the default one.
        ShowTrayBalloon(L"LiveWall запущен",
                        L"Щёлкните по значку в трее и выберите файл обоев.");
    }
    return true;
}

void App::Shutdown() {
    RemoveTrayIcon();

    if (m_host) {
        m_host->Stop();
        m_host.reset();
    }

    m_source.reset();
    m_renderer.Shutdown();
    MediaFrameworkShutdown();

    if (m_messageWindow && IsWindow(m_messageWindow)) {
        DestroyWindow(m_messageWindow);
        m_messageWindow = nullptr;
    }
}

// ---------------------------------------------------------------------------
// windows and tray
// ---------------------------------------------------------------------------

bool App::CreateMessageWindow() {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &App::WndProc;
    wc.hInstance = m_instance;
    wc.lpszClassName = kMessageClass;

    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        LogPrintf("RegisterClassEx(message window) failed: %lu", GetLastError());
        return false;
    }

    // A hidden *top-level* window is required: broadcast messages such as
    // TaskbarCreated are not delivered to message-only windows.
    m_messageWindow = CreateWindowExW(0, kMessageClass, kWindowTitle, WS_OVERLAPPED, 0, 0, 0, 0,
                                      nullptr, nullptr, m_instance, this);
    if (!m_messageWindow) {
        LogPrintf("CreateWindowEx(message window) failed: %lu", GetLastError());
        return false;
    }

    WTSRegisterSessionNotification(m_messageWindow, NOTIFY_FOR_THIS_SESSION);
    return true;
}

void App::CreateTrayIcon() {
    if (!m_messageWindow) return;

    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = m_messageWindow;
    data.uID = kTrayIconId;
    data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    data.uCallbackMessage = kTrayMessage;
    data.hIcon = static_cast<HICON>(LoadImageW(m_instance, MAKEINTRESOURCEW(kIconResourceId),
                                               IMAGE_ICON, 0, 0, LR_DEFAULTSIZE));
    if (!data.hIcon) data.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    lstrcpynW(data.szTip, L"LiveWall", std::size(data.szTip));

    if (Shell_NotifyIconW(NIM_ADD, &data)) {
        m_trayAdded = true;
        UpdateTrayIcon();
    } else {
        LogPrintf("Shell_NotifyIcon(NIM_ADD) failed");
    }
}

void App::UpdateTrayIcon() {
    if (!m_trayAdded || !m_messageWindow) return;

    std::wstring tip = L"LiveWall";
    if (m_source) {
        tip += L": ";
        tip += FileNameOf(m_settings.media);
    } else {
        tip += L": файл не выбран";
    }
    if (m_paused) tip += L" (пауза)";

    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = m_messageWindow;
    data.uID = kTrayIconId;
    data.uFlags = NIF_TIP;
    lstrcpynW(data.szTip, tip.c_str(), static_cast<int>(std::size(data.szTip)));
    Shell_NotifyIconW(NIM_MODIFY, &data);
}

void App::ShowTrayBalloon(const std::wstring& title, const std::wstring& text) {
    if (!m_trayAdded || !m_messageWindow) return;

    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = m_messageWindow;
    data.uID = kTrayIconId;
    data.uFlags = NIF_INFO;
    data.dwInfoFlags = NIIF_INFO;
    data.uTimeout = 5000;
    lstrcpynW(data.szInfo, text.c_str(), static_cast<int>(std::size(data.szInfo)));
    lstrcpynW(data.szInfoTitle, title.c_str(),
              static_cast<int>(std::size(data.szInfoTitle)));
    Shell_NotifyIconW(NIM_MODIFY, &data);
}

void App::RemoveTrayIcon() {
    if (!m_trayAdded || !m_messageWindow) return;
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = m_messageWindow;
    data.uID = kTrayIconId;
    Shell_NotifyIconW(NIM_DELETE, &data);
    m_trayAdded = false;
}

void App::ShowTrayMenu(POINT screenPoint) {
    HMENU menu = CreatePopupMenu();
    HMENU modes = CreatePopupMenu();
    if (!menu || !modes) {
        if (menu) DestroyMenu(menu);
        return;
    }

    AppendMenuW(modes, MF_STRING | (m_settings.mode == L"fit" ? MF_CHECKED : 0), kCmdFit,
                L"Вписать (fit)");
    AppendMenuW(modes, MF_STRING | (m_settings.mode == L"fill" ? MF_CHECKED : 0), kCmdFill,
                L"Заполнить (fill)");
    AppendMenuW(modes, MF_STRING | (m_settings.mode == L"stretch" ? MF_CHECKED : 0), kCmdStretch,
                L"Растянуть (stretch)");

    AppendMenuW(menu, MF_STRING, kCmdOpen, L"Открыть файл…");
    AppendMenuW(menu, MF_STRING | (m_userPaused ? MF_CHECKED : 0), kCmdPause, L"Пауза");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(modes), L"Масштабирование");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (IsAutostartEnabled() ? MF_CHECKED : 0), kCmdAutostart,
                L"Запускать при входе");
    AppendMenuW(menu, MF_STRING, kCmdRestart, L"Перезапустить обои");
    AppendMenuW(menu, MF_STRING, kCmdOpenLog, L"Открыть журнал");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kCmdExit, L"Выход");

    SetForegroundWindow(m_messageWindow);
    const UINT command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY,
                                        screenPoint.x, screenPoint.y, 0, m_messageWindow, nullptr);
    DestroyMenu(menu);
    PostMessageW(m_messageWindow, WM_NULL, 0, 0);

    if (command != 0) {
        SendMessageW(m_messageWindow, WM_COMMAND, MAKEWPARAM(command, 0), 0);
    }
}

void App::OpenMediaDialog() {
    wchar_t file[MAX_PATH * 4]{};

    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = m_messageWindow;
    dialog.lpstrFilter =
        L"Видео и анимация (*.mp4;*.webm;*.mkv;*.mov;*.avi;*.wmv;*.gif)\0"
        L"*.mp4;*.webm;*.mkv;*.mov;*.avi;*.wmv;*.gif\0"
        L"Изображения (*.png;*.jpg;*.jpeg;*.bmp)\0*.png;*.jpg;*.jpeg;*.bmp\0"
        L"Все файлы (*.*)\0*.*\0";
    dialog.lpstrFile = file;
    dialog.nMaxFile = static_cast<DWORD>(std::size(file));
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;
    dialog.lpstrTitle = L"Выберите файл для живых обоев";

    if (GetOpenFileNameW(&dialog)) {
        LoadMedia(file);
    }
}

// ---------------------------------------------------------------------------
// media and desktop
// ---------------------------------------------------------------------------

void App::LoadMedia(const std::wstring& path) {
    if (path.empty()) return;

    const std::wstring absolute = MakeAbsolute(path);
    std::string error;
    auto source = CreateMediaSource(absolute, error);
    if (!source) {
        LogPrintf("cannot open %s: %s", WideToUtf8(absolute).c_str(), error.c_str());
        ShowError(L"Не удалось открыть файл:\n" + absolute + L"\n\n" + Utf8ToWide(error));
        return;
    }

    m_source = std::move(source);
    m_settings.media = absolute;
    m_settings.Save();

    m_renderer.SetMode(ToMode(m_settings.mode));
    m_renderer.UploadFrame(m_source->CurrentFrame());
    m_dirty = true;
    UpdateTrayIcon();

    if (m_host && !m_host->Healthy()) ReloadDesktop();
}

void App::ApplyMode() {
    m_renderer.SetMode(ToMode(m_settings.mode));
    m_settings.Save();
    m_dirty = true;
}

void App::ReloadDesktop() {
    if (!m_host) return;

    m_renderer.DetachAllWindows();

    std::string error;
    if (!m_host->Rebuild(error)) {
        LogPrintf("desktop rebuild failed: %s", error.c_str());
        // Explorer may still be starting; try again shortly.
        SetTimer(m_messageWindow, kTimerRestart, 1500, nullptr);
        return;
    }

    for (HWND window : m_host->Windows()) {
        RECT rect{};
        GetClientRect(window, &rect);
        std::string attachError;
        m_renderer.AttachWindow(window, static_cast<uint32_t>(rect.right),
                                static_cast<uint32_t>(rect.bottom), attachError);
    }
    m_dirty = true;
}

// ---------------------------------------------------------------------------
// state
// ---------------------------------------------------------------------------

bool App::IsPaused() {
    if (m_userPaused) return true;
    if (m_settings.pauseOnLock && m_sessionLocked) return true;
    if (m_settings.pauseOnFullscreen && IsFullscreenAppActive()) return true;
    if (m_settings.pauseOnBattery && IsOnBattery()) return true;
    return false;
}

bool App::IsFullscreenAppActive() const {
    QUERY_USER_NOTIFICATION_STATE state = QUNS_NOT_PRESENT;
    if (FAILED(SHQueryUserNotificationState(&state))) return false;
    return state == QUNS_RUNNING_D3D_FULL_SCREEN || state == QUNS_BUSY ||
           state == QUNS_PRESENTATION_MODE;
}

bool App::IsOnBattery() const {
    SYSTEM_POWER_STATUS status{};
    if (!GetSystemPowerStatus(&status)) return false;
    if (status.BatteryFlag == 128) return false;  // desktop, no battery
    return status.ACLineStatus == 0;
}

void App::UpdateAutostart(bool enabled) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key,
                        nullptr) != ERROR_SUCCESS) {
        LogPrintf("cannot open the Run registry key");
        return;
    }

    if (enabled) {
        const std::wstring command = L"\"" + GetModulePath() + L"\" --autostart";
        RegSetValueExW(key, kRunValue, 0, REG_SZ,
                       reinterpret_cast<const BYTE*>(command.c_str()),
                       static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(key, kRunValue);
    }
    RegCloseKey(key);
}

bool App::IsAutostartEnabled() const {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) {
        return false;
    }
    wchar_t value[1024]{};
    DWORD size = sizeof(value);
    DWORD type = 0;
    const LONG result = RegQueryValueExW(key, kRunValue, nullptr, &type,
                                         reinterpret_cast<BYTE*>(value), &size);
    RegCloseKey(key);
    return result == ERROR_SUCCESS && type == REG_SZ;
}

// ---------------------------------------------------------------------------
// window messages
// ---------------------------------------------------------------------------

void App::OnWallpaperResize(HWND hwnd, uint32_t width, uint32_t height) {
    if (width == 0 || height == 0) return;
    std::string error;
    if (!m_renderer.AttachWindow(hwnd, width, height, error)) {
        LogPrintf("cannot attach hwnd 0x%p: %s", hwnd, error.c_str());
    }
    m_dirty = true;
}

LRESULT CALLBACK App::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    App* self = reinterpret_cast<App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lp);
        self = static_cast<App*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
    return self->HandleMessage(hwnd, msg, wp, lp);
}

LRESULT App::HandleMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg != 0 && msg == g_taskbarCreated) {
        LogPrintf("Explorer restarted");
        m_trayAdded = false;
        CreateTrayIcon();
        SetTimer(hwnd, kTimerRestart, 800, nullptr);
        return 0;
    }

    switch (msg) {
        case kTrayMessage:
            switch (LOWORD(lp)) {
                case WM_RBUTTONUP:
                case WM_CONTEXTMENU: {
                    POINT point{};
                    GetCursorPos(&point);
                    ShowTrayMenu(point);
                    return 0;
                }
                case WM_LBUTTONDBLCLK:
                    OpenMediaDialog();
                    return 0;
                default:
                    return 0;
            }

        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case kCmdOpen:
                    OpenMediaDialog();
                    break;
                case kCmdPause:
                    m_userPaused = !m_userPaused;
                    m_pauseStateDirty = true;
                    m_dirty = true;
                    break;
                case kCmdFit:
                    m_settings.mode = L"fit";
                    ApplyMode();
                    break;
                case kCmdFill:
                    m_settings.mode = L"fill";
                    ApplyMode();
                    break;
                case kCmdStretch:
                    m_settings.mode = L"stretch";
                    ApplyMode();
                    break;
                case kCmdRestart:
                    ReloadDesktop();
                    break;
                case kCmdAutostart:
                    UpdateAutostart(!IsAutostartEnabled());
                    break;
                case kCmdOpenLog: {
                    const std::wstring path = LogFilePath();
                    if (!path.empty()) {
                        ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr,
                                      SW_SHOWNORMAL);
                    }
                    break;
                }
                case kCmdExit:
                    m_quitting = true;
                    DestroyWindow(hwnd);
                    break;
                default:
                    break;
            }
            return 0;

        case WM_TIMER:
            if (wp == kTimerRestart) {
                KillTimer(hwnd, kTimerRestart);
                ReloadDesktop();
            }
            return 0;

        case WM_DISPLAYCHANGE:
            LogPrintf("display configuration changed");
            if (m_host) m_host->RequestRebuild();
            m_pauseStateDirty = true;
            return 0;

        case WM_WTSSESSION_CHANGE:
            m_sessionLocked = wp == WTS_SESSION_LOCK;
            m_pauseStateDirty = true;
            return 0;

        case WM_POWERBROADCAST:
            m_pauseStateDirty = true;
            m_dirty = true;
            return TRUE;

        case WM_COPYDATA: {
            auto* data = reinterpret_cast<COPYDATASTRUCT*>(lp);
            if (data && data->cbData >= sizeof(wchar_t) && data->lpData) {
                const std::wstring path(static_cast<const wchar_t*>(data->lpData));
                LoadMedia(path);
            }
            return TRUE;
        }

        case WM_CLOSE:
            m_quitting = true;
            DestroyWindow(hwnd);
            return 0;

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;

        default:
            break;
    }

    return DefWindowProcW(hwnd, msg, wp, lp);
}
