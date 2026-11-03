#include "log.h"

#include "util.h"

#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace {

std::mutex g_mutex;
FILE* g_file = nullptr;
std::wstring g_path;
constexpr long long kMaxLogSize = 2 * 1024 * 1024;

void OpenFile() {
    if (g_path.empty()) {
        const std::wstring& dir = GetAppDataDir();
        if (dir.empty()) return;
        g_path = JoinPath(dir, L"livewall.log");
    }

    // Rotate a single time, so the log never grows unbounded.
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (GetFileAttributesExW(g_path.c_str(), GetFileExInfoStandard, &data)) {
        LARGE_INTEGER size;
        size.LowPart = data.nFileSizeLow;
        size.HighPart = static_cast<LONG>(data.nFileSizeHigh);
        if (size.QuadPart > kMaxLogSize) {
            const std::wstring old = g_path + L".old";
            MoveFileExW(g_path.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING);
        }
    }

    g_file = _wfopen(g_path.c_str(), L"ab");
    if (!g_file) {
        g_path.clear();
    }
}

}  // namespace

void LogInit() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file) return;
    OpenFile();
}

void LogShutdown() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file) {
        fclose(g_file);
        g_file = nullptr;
    }
}

void LogPrintf(const char* fmt, ...) {
    char message[2048];
    va_list args;
    va_start(args, fmt);
    const int written = vsnprintf(message, sizeof(message), fmt, args);
    va_end(args);
    if (written <= 0) return;

    SYSTEMTIME time;
    GetLocalTime(&time);

    char line[2200];
    snprintf(line, sizeof(line), "[%02d:%02d:%02d.%03d] %s\n",
             time.wHour, time.wMinute, time.wSecond, time.wMilliseconds, message);

    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_file) OpenFile();
    if (g_file) {
        fputs(line, g_file);
        fflush(g_file);
    }
    OutputDebugStringA(line);
}

void LogHr(HRESULT hr, const char* what) {
    LogPrintf("%s failed: 0x%08lX (%s)", what, static_cast<unsigned long>(hr),
              HrToString(hr).c_str());
}

const std::wstring& LogFilePath() {
    return g_path;
}
