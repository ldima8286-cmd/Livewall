#include "media.h"

#include "log.h"
#include "util.h"

#include <atomic>

std::unique_ptr<MediaSource> CreateWicMediaSource();
std::unique_ptr<MediaSource> CreateVideoMediaSource();

namespace {

std::atomic<uint64_t> g_frameSerial{0};

bool IsImageExtension(const std::wstring& ext) {
    return ext == L".gif" || ext == L".png" || ext == L".jpg" || ext == L".jpeg" ||
           ext == L".bmp" || ext == L".tif" || ext == L".tiff" || ext == L".ico";
}

}  // namespace

uint64_t NextFrameSerial() {
    return g_frameSerial.fetch_add(1, std::memory_order_relaxed) + 1;
}

std::unique_ptr<MediaSource> CreateMediaSource(const std::wstring& path, std::string& error) {
    if (!FileExists(path)) {
        error = "file not found";
        return nullptr;
    }

    const std::wstring ext = ExtensionOf(path);
    std::unique_ptr<MediaSource> source;

    if (IsImageExtension(ext)) {
        source = CreateWicMediaSource();
    } else {
        source = CreateVideoMediaSource();
    }

    if (!source) {
        error = "no decoder for extension " + WideToUtf8(ext.empty() ? L"(none)" : ext);
        return nullptr;
    }

    if (!source->Open(path, error)) {
        return nullptr;
    }

    LogPrintf("media opened: %s (%s, %ux%u, animated=%d)",
              WideToUtf8(path).c_str(), source->Kind(),
              source->CurrentFrame().width, source->CurrentFrame().height,
              source->IsAnimated() ? 1 : 0);
    return source;
}

void MediaFrameworkShutdown() {
    // Implemented in media_mf.cpp; declared here to keep callers free of MF headers.
    extern void MediaFoundationShutdownImpl();
    MediaFoundationShutdownImpl();
}
