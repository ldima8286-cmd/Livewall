#include "media.h"

#include "log.h"
#include "util.h"

#include <wincodec.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace {

// GIF frames without a timing chunk are shown for 100 ms; the same value is
// used by browsers when a file declares 0 or 10 ms.
constexpr UINT kDefaultDelayMs = 100;

class WicSource final : public MediaSource {
public:
    bool Open(const std::wstring& path, std::string& error) override;
    bool Advance(double seconds) override;
    const Frame& CurrentFrame() const override { return m_frame; }
    bool IsAnimated() const override { return m_frameCount > 1; }
    const char* Kind() const override { return m_frameCount > 1 ? "gif" : "image"; }

private:
    bool DecodeFrame(UINT index);
    UINT DelayOf(UINT index);

    ComPtr<IWICImagingFactory> m_factory;
    ComPtr<IWICBitmapDecoder> m_decoder;
    ComPtr<IWICFormatConverter> m_converter;

    Frame m_frame;
    std::vector<UINT> m_delays;
    UINT m_frameCount = 0;
    UINT m_index = 0;
    double m_elapsedMs = 0.0;
};

bool WicSource::Open(const std::wstring& path, std::string& error) {
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(m_factory.GetAddressOf()));
    if (FAILED(hr)) {
        error = "WIC factory: " + HrToString(hr);
        return false;
    }

    hr = m_factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                              WICDecodeMetadataCacheOnDemand,
                                              m_decoder.GetAddressOf());
    if (FAILED(hr)) {
        error = "WIC decoder: " + HrToString(hr);
        return false;
    }

    hr = m_decoder->GetFrameCount(&m_frameCount);
    if (FAILED(hr) || m_frameCount == 0) {
        error = "image has no frames";
        return false;
    }

    hr = m_factory->CreateFormatConverter(m_converter.GetAddressOf());
    if (FAILED(hr)) {
        error = "WIC format converter: " + HrToString(hr);
        return false;
    }

    ComPtr<IWICBitmapFrameDecode> first;
    hr = m_decoder->GetFrame(0, first.GetAddressOf());
    if (FAILED(hr)) {
        error = "WIC first frame: " + HrToString(hr);
        return false;
    }

    UINT width = 0;
    UINT height = 0;
    if (FAILED(first->GetSize(&width, &height)) || width == 0 || height == 0) {
        error = "invalid image size";
        return false;
    }

    m_frame.width = width;
    m_frame.height = height;
    m_frame.pixels.resize(static_cast<size_t>(width) * height * 4);

    m_delays.resize(m_frameCount, kDefaultDelayMs);
    for (UINT i = 0; i < m_frameCount; ++i) {
        m_delays[i] = DelayOf(i);
    }

    if (!DecodeFrame(0)) {
        error = "failed to decode the first frame";
        return false;
    }
    m_frame.serial = NextFrameSerial();
    return true;
}

UINT WicSource::DelayOf(UINT index) {
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(m_decoder->GetFrame(index, frame.GetAddressOf()))) return kDefaultDelayMs;

    ComPtr<IWICMetadataQueryReader> reader;
    if (FAILED(frame->GetMetadataQueryReader(reader.GetAddressOf())) || !reader) {
        return kDefaultDelayMs;
    }

    PROPVARIANT value{};
    if (FAILED(reader->GetMetadataByName(L"/grctlext/Delay", &value))) return kDefaultDelayMs;

    UINT delay = kDefaultDelayMs;
    if (value.vt == VT_UI2) delay = value.uiVal * 10u;
    if (delay < 20) delay = kDefaultDelayMs;  // clamp the usual "0 ms" misconfiguration
    return delay;
}

bool WicSource::DecodeFrame(UINT index) {
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(m_decoder->GetFrame(index, frame.GetAddressOf()))) return false;

    UINT width = 0;
    UINT height = 0;
    if (FAILED(frame->GetSize(&width, &height)) || width == 0 || height == 0) return false;

    if (width != m_frame.width || height != m_frame.height) {
        m_frame.width = width;
        m_frame.height = height;
        m_frame.pixels.resize(static_cast<size_t>(width) * height * 4);
    }

    const HRESULT hr = m_converter->Initialize(
        frame.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0.0,
        WICBitmapPaletteTypeCustom);
    if (FAILED(hr)) {
        LogHr(hr, "WIC converter Initialize");
        return false;
    }

    const UINT stride = width * 4;
    const HRESULT copy = m_converter->CopyPixels(
        nullptr, stride, static_cast<UINT>(m_frame.pixels.size()), m_frame.pixels.data());
    if (FAILED(copy)) {
        LogHr(copy, "WIC CopyPixels");
        return false;
    }
    return true;
}

bool WicSource::Advance(double seconds) {
    if (m_frameCount <= 1) return false;

    m_elapsedMs += seconds * 1000.0;
    bool changed = false;

    // Skip at most a handful of frames per tick so a long stall cannot turn
    // into an unbounded loop.
    for (int guard = 0; guard < 4; ++guard) {
        const double delay = static_cast<double>(m_delays[m_index]);
        if (m_elapsedMs < delay) break;

        m_elapsedMs -= delay;
        m_index = (m_index + 1) % m_frameCount;
        if (!DecodeFrame(m_index)) break;
        changed = true;
    }

    if (changed) m_frame.serial = NextFrameSerial();
    return changed;
}

}  // namespace

std::unique_ptr<MediaSource> CreateWicMediaSource() {
    return std::make_unique<WicSource>();
}
