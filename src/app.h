#pragma once

#include "common.h"
#include "desktop.h"
#include "media.h"
#include "renderer.h"
#include "settings.h"

#include <memory>

// Commands that can be sent to an already running instance, either from the
// command line or from the installer.
enum class RemoteCommand : DWORD {
    Open = 1,  // payload: the media path (WM_COPYDATA)
    Pause,
    Resume,
    TogglePause,
    Quit,
    OpenDialog,
};

struct LaunchOptions {
    std::wstring media;
    std::wstring mode;
    int fps = 0;
    bool autostart = false;
    bool showVersion = false;
    bool showHelp = false;
    bool hasCommand = false;
    RemoteCommand command = RemoteCommand::OpenDialog;
};

class App final : public DesktopSink {
public:
    int Run(HINSTANCE instance, int argc, wchar_t** argv);

    void OnWallpaperResize(HWND hwnd, uint32_t width, uint32_t height) override;

private:
    enum Command : UINT {
        kCmdOpen = 1001,
        kCmdPause,
        kCmdFit,
        kCmdFill,
        kCmdStretch,
        kCmdRestart,
        kCmdAutostart,
        kCmdOpenLog,
        kCmdExit,
    };

    enum : UINT {
        kTrayMessage = WM_APP + 1,
        kTimerRestart = 1,
    };

    bool Init(HINSTANCE instance, const LaunchOptions& options);
    void Shutdown();

    bool CreateMessageWindow();
    void CreateTrayIcon();
    void UpdateTrayIcon();
    void ShowTrayBalloon(const std::wstring& title, const std::wstring& text);
    void RemoveTrayIcon();
    void ShowTrayMenu(POINT screenPoint);
    void OpenMediaDialog();

    void LoadMedia(const std::wstring& path);
    void ApplyMode();
    void ReloadDesktop();

    void UpdateAutostart(bool enabled);
    bool IsAutostartEnabled() const;

    bool IsPaused();
    bool IsFullscreenAppActive() const;
    bool IsOnBattery() const;

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT HandleMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

    HINSTANCE m_instance = nullptr;
    HWND m_messageWindow = nullptr;
    Renderer m_renderer;
    std::unique_ptr<DesktopHost> m_host;
    std::unique_ptr<MediaSource> m_source;
    Settings m_settings;
    std::wstring m_pendingPath;

    bool m_trayAdded = false;
    bool m_userPaused = false;
    bool m_sessionLocked = false;
    bool m_paused = false;
    bool m_wasPaused = false;
    bool m_dirty = true;
    bool m_quitting = false;
    bool m_pauseStateDirty = true;

    double m_lastFrameTime = 0.0;
    double m_lastHealthCheck = 0.0;
    double m_lastPauseCheck = 0.0;
};
