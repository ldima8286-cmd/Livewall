#include "settings.h"

#include "log.h"
#include "util.h"

#include <cstdlib>
#include <iterator>

namespace {

std::wstring ReadString(const wchar_t* key, const wchar_t* fallback, const std::wstring& file) {
    wchar_t buffer[1024]{};
    GetPrivateProfileStringW(L"livewall", key, fallback, buffer,
                             static_cast<DWORD>(std::size(buffer)), file.c_str());
    return buffer;
}

void WriteString(const wchar_t* key, const std::wstring& value, const std::wstring& file) {
    WritePrivateProfileStringW(L"livewall", key, value.c_str(), file.c_str());
}

}  // namespace

std::wstring Settings::Path() {
    const std::wstring& dir = GetAppDataDir();
    if (dir.empty()) return {};
    return JoinPath(dir, L"settings.ini");
}

void Settings::Load() {
    const std::wstring file = Path();
    if (file.empty() || !FileExists(file)) return;

    media = ReadString(L"media", L"", file);
    mode = ReadString(L"mode", L"fit", file);
    fps = _wtoi(ReadString(L"fps", L"60", file).c_str());
    pauseOnFullscreen = ReadString(L"pause_on_fullscreen", L"1", file) != L"0";
    pauseOnBattery = ReadString(L"pause_on_battery", L"0", file) != L"0";
    pauseOnLock = ReadString(L"pause_on_lock", L"1", file) != L"0";

    if (fps < 1 || fps > 240) fps = 60;
    if (mode != L"fit" && mode != L"fill" && mode != L"stretch") mode = L"fit";

    LogPrintf("settings loaded: mode=%s fps=%d media=%s",
              WideToUtf8(mode).c_str(), fps, WideToUtf8(media).c_str());
}

void Settings::Save() const {
    const std::wstring file = Path();
    if (file.empty()) return;

    WriteString(L"media", media, file);
    WriteString(L"mode", mode, file);
    WriteString(L"fps", std::to_wstring(fps), file);
    WriteString(L"pause_on_fullscreen", pauseOnFullscreen ? L"1" : L"0", file);
    WriteString(L"pause_on_battery", pauseOnBattery ? L"1" : L"0", file);
    WriteString(L"pause_on_lock", pauseOnLock ? L"1" : L"0", file);
}
