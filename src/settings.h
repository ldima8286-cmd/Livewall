#pragma once

#include "common.h"

struct Settings {
    std::wstring media;          // absolute path to the wallpaper file
    std::wstring mode = L"fit";  // fit | fill | stretch
    int fps = 60;                // render cap, 0 = unlimited (not recommended)
    bool pauseOnFullscreen = true;
    bool pauseOnBattery = false;
    bool pauseOnLock = true;

    void Load();
    void Save() const;

    static std::wstring Path();
};
