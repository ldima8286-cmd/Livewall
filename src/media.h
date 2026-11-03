#pragma once

#include "common.h"

// A decoded frame, always BGRA8 with tightly packed rows (stride = width * 4).
struct Frame {
    std::vector<uint8_t> pixels;
    uint32_t width = 0;
    uint32_t height = 0;
    uint64_t serial = 0;  // unique id of the content, see NextFrameSerial()

    size_t Stride() const { return static_cast<size_t>(width) * 4; }
    bool Empty() const { return pixels.empty() || width == 0 || height == 0; }
};

// Process-wide, monotonically increasing frame id. It must not restart per
// media source: the renderer skips an upload when the serial matches the last
// one, and a per-source counter made the first frame of a newly opened file
// collide with the last frame of the previous one (a static image always has
// serial 1, so switching between two images changed nothing).
uint64_t NextFrameSerial();

class MediaSource {
public:
    virtual ~MediaSource() = default;

    // Opens the file. Returns false and fills `error` on failure.
    virtual bool Open(const std::wstring& path, std::string& error) = 0;

    // Advances the playback clock by `seconds`. Returns true when CurrentFrame()
    // contains new pixels. Sources that are not animated return false forever
    // after Open().
    virtual bool Advance(double seconds) = 0;

    virtual const Frame& CurrentFrame() const = 0;
    virtual bool IsAnimated() const = 0;
    virtual const char* Kind() const = 0;
};

// Picks a decoder based on the file extension:
//   .gif, .png, .jpg, .jpeg, .bmp, .tif, .tiff, .ico  -> WIC
//   everything else                                    -> Media Foundation
std::unique_ptr<MediaSource> CreateMediaSource(const std::wstring& path, std::string& error);

// Releases Media Foundation. Call once at process exit.
void MediaFrameworkShutdown();
