#include "media.h"

#include "log.h"
#include "util.h"

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace {

bool g_mfStarted = false;

constexpr LONGLONG kTicksPerSecond = 10'000'000;

class VideoSource final : public MediaSource {
public:
    bool Open(const std::wstring& path, std::string& error) override;
    bool Advance(double seconds) override;
    const Frame& CurrentFrame() const override { return m_current; }
    bool IsAnimated() const override { return true; }
    const char* Kind() const override { return "video"; }

private:
    enum class ReadResult { Frame, Gap, EndOfStream, Error };

    ReadResult ReadSample(std::vector<uint8_t>& destination, int64_t& timestamp);
    bool SeekToStart();

    ComPtr<IMFSourceReader> m_reader;
    Frame m_current;
    Frame m_pending;
    int64_t m_pendingTime = 0;
    double m_clock = 0.0;

    uint32_t m_width = 0;
    uint32_t m_height = 0;
    LONG m_stride = 0;
    bool m_hasPending = false;
};

bool VideoSource::Open(const std::wstring& path, std::string& error) {
    if (!g_mfStarted) {
        const HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_FULL);
        if (FAILED(hr)) {
            error = "MFStartup: " + HrToString(hr);
            return false;
        }
        g_mfStarted = true;
    }

    ComPtr<IMFAttributes> attributes;
    if (FAILED(MFCreateAttributes(attributes.GetAddressOf(), 1))) {
        error = "MFCreateAttributes failed";
        return false;
    }
    attributes->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);

    HRESULT hr = MFCreateSourceReaderFromURL(path.c_str(), attributes.Get(),
                                             m_reader.GetAddressOf());
    if (FAILED(hr)) {
        error = "cannot open video (MFCreateSourceReaderFromURL: " + HrToString(hr) + ")";
        return false;
    }

    m_reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
    m_reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), TRUE);

    ComPtr<IMFMediaType> native;
    hr = m_reader->GetNativeMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0,
                                      native.GetAddressOf());
    if (FAILED(hr)) {
        error = "file has no video stream (" + HrToString(hr) + ")";
        return false;
    }

    UINT32 width = 0;
    UINT32 height = 0;
    if (FAILED(MFGetAttributeSize(native.Get(), MF_MT_FRAME_SIZE, &width, &height)) ||
        width == 0 || height == 0) {
        error = "video stream has no frame size";
        return false;
    }

    ComPtr<IMFMediaType> requested;
    if (FAILED(MFCreateMediaType(requested.GetAddressOf()))) {
        error = "MFCreateMediaType failed";
        return false;
    }
    requested->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    requested->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
    MFSetAttributeSize(requested.Get(), MF_MT_FRAME_SIZE, width, height);
    requested->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    MFSetAttributeRatio(requested.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);

    hr = m_reader->SetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM),
                                       nullptr, requested.Get());
    if (FAILED(hr)) {
        error = "cannot configure RGB32 output (" + HrToString(hr) + ")";
        return false;
    }

    // The reader may pick a different size or a negative (bottom-up) stride.
    ComPtr<IMFMediaType> actual;
    m_reader->GetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM),
                                  actual.GetAddressOf());

    UINT32 actualWidth = width;
    UINT32 actualHeight = height;
    if (actual) {
        MFGetAttributeSize(actual.Get(), MF_MT_FRAME_SIZE, &actualWidth, &actualHeight);
    }

    UINT32 stride = 0;
    if (!actual || FAILED(actual->GetUINT32(MF_MT_DEFAULT_STRIDE, &stride)) || stride == 0) {
        stride = actualWidth * 4;
    }

    m_width = actualWidth;
    m_height = actualHeight;
    m_stride = static_cast<LONG>(stride);

    m_current.width = m_width;
    m_current.height = m_height;
    m_current.pixels.resize(static_cast<size_t>(m_width) * m_height * 4);
    m_pending.width = m_width;
    m_pending.height = m_height;
    m_pending.pixels.resize(static_cast<size_t>(m_width) * m_height * 4);

    // Prime the first frame so the wallpaper is not black on startup.
    Advance(0.0);
    return true;
}

VideoSource::ReadResult VideoSource::ReadSample(std::vector<uint8_t>& destination,
                                                int64_t& timestamp) {
    DWORD flags = 0;
    LONGLONG sampleTime = 0;
    ComPtr<IMFSample> sample;

    const HRESULT hr =
        m_reader->ReadSample(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, nullptr,
                             &flags, &sampleTime, sample.GetAddressOf());
    if (FAILED(hr)) {
        LogHr(hr, "IMFSourceReader::ReadSample");
        return ReadResult::Error;
    }

    if (flags & MF_SOURCE_READERF_ENDOFSTREAM) return ReadResult::EndOfStream;
    if (!sample) return ReadResult::Gap;

    ComPtr<IMFMediaBuffer> buffer;
    if (FAILED(sample->ConvertToContiguousBuffer(buffer.GetAddressOf()))) {
        return ReadResult::Error;
    }

    BYTE* data = nullptr;
    DWORD maxLength = 0;
    if (FAILED(buffer->Lock(&data, &maxLength, nullptr))) return ReadResult::Error;

    const size_t rowBytes = static_cast<size_t>(m_width) * 4;
    const size_t absoluteStride = static_cast<size_t>(m_stride >= 0 ? m_stride : -m_stride);
    const size_t needed = rowBytes + (m_height > 0 ? (m_height - 1) * absoluteStride : 0);

    if (maxLength < needed || destination.size() < rowBytes * m_height) {
        buffer->Unlock();
        LogPrintf("video sample too small: %lu bytes, need %zu", maxLength, needed);
        return ReadResult::Error;
    }

    for (uint32_t y = 0; y < m_height; ++y) {
        const BYTE* source =
            m_stride >= 0
                ? data + static_cast<size_t>(y) * static_cast<size_t>(m_stride)
                : data + static_cast<size_t>(m_height - 1 - y) * static_cast<size_t>(-m_stride);
        memcpy(destination.data() + static_cast<size_t>(y) * rowBytes, source, rowBytes);
    }

    buffer->Unlock();
    timestamp = sampleTime;
    return ReadResult::Frame;
}

bool VideoSource::SeekToStart() {
    if (!m_reader) return false;

    PROPVARIANT position{};
    position.vt = VT_I8;
    position.hVal.QuadPart = 0;

    const HRESULT hr = m_reader->SetCurrentPosition(GUID_NULL, position);
    if (FAILED(hr)) {
        LogHr(hr, "SetCurrentPosition");
        return false;
    }

    m_reader->Flush(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM));
    m_hasPending = false;
    m_clock = 0.0;
    return true;
}

bool VideoSource::Advance(double seconds) {
    if (!m_reader) return false;

    m_clock += seconds;
    bool changed = false;

    for (int guard = 0; guard < 8; ++guard) {
        if (m_hasPending) {
            const double due = static_cast<double>(m_pendingTime) / kTicksPerSecond;
            if (due <= m_clock + 1e-4) {
                m_current.pixels.swap(m_pending.pixels);
                m_current.width = m_width;
                m_current.height = m_height;
                m_current.serial = NextFrameSerial();
                m_hasPending = false;
                changed = true;
                continue;
            }
            break;  // next frame is in the future
        }

        int64_t timestamp = 0;
        switch (ReadSample(m_pending.pixels, timestamp)) {
            case ReadResult::Frame:
                m_pendingTime = timestamp;
                m_hasPending = true;
                break;
            case ReadResult::Gap:
                continue;
            case ReadResult::EndOfStream:
                if (!SeekToStart()) return changed;
                break;
            case ReadResult::Error:
                return changed;
        }
    }

    return changed;
}

}  // namespace

std::unique_ptr<MediaSource> CreateVideoMediaSource() {
    return std::make_unique<VideoSource>();
}

void MediaFoundationShutdownImpl() {
    if (g_mfStarted) {
        MFShutdown();
        g_mfStarted = false;
    }
}
