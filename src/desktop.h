#pragma once

#include "common.h"

class Renderer;

// Receives events from the desktop host windows. All callbacks run on the thread
// that created the windows.
class DesktopSink {
public:
    virtual ~DesktopSink() = default;
    virtual void OnWallpaperResize(HWND hwnd, uint32_t width, uint32_t height) = 0;
};

// Owns one child window per monitor, parented into Explorer's wallpaper layer
// (WorkerW). This is based on undocumented Explorer behaviour and is expected to
// break on some Windows builds - Rebuild() tries a few known strategies.
class DesktopHost {
public:
    explicit DesktopHost(DesktopSink* sink);
    ~DesktopHost();

    DesktopHost(const DesktopHost&) = delete;
    DesktopHost& operator=(const DesktopHost&) = delete;

    bool Start(std::string& error);
    void Stop();
    bool Rebuild(std::string& error);

    // Called periodically. Detects Explorer restarts, closed windows and
    // monitor changes. Returns true when a rebuild is required.
    bool CheckHealth();

    bool Healthy() const { return m_started && m_worker != nullptr && !m_windows.empty(); }
    bool NeedsRebuild() const { return m_needsRebuild; }
    void ClearRebuildFlag() { m_needsRebuild = false; }
    void RequestRebuild() { m_needsRebuild = true; }

    const std::vector<HWND>& Windows() const { return m_windows; }

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    static BOOL CALLBACK MonitorProc(HMONITOR mon, HDC dc, LPRECT rect, LPARAM data);

    HWND CreateForMonitor(HMONITOR mon);
    void DestroyWindows();
    static bool RegisterWindowClass();

    DesktopSink* m_sink = nullptr;
    HWND m_worker = nullptr;
    std::vector<HMONITOR> m_monitors;
    std::vector<HWND> m_windows;
    bool m_needsRebuild = false;
    bool m_stopping = false;
    bool m_started = false;
};
