#include "renderer.h"

#include "log.h"
#include "util.h"

#include <d3dcompiler.h>

using Microsoft::WRL::ComPtr;

namespace {

const char* kShaderSource = R"(
cbuffer Params : register(b0)
{
    float2 g_scale;
    float2 g_offset;
    float4 g_background;
    float  g_clamp;
    float  g_pad0;
    float  g_pad1;
    float  g_pad2;
};

Texture2D    g_texture : register(t0);
SamplerState g_sampler : register(s0);

struct VSOut
{
    float4 position : SV_Position;
    float2 uv       : TEXCOORD0;
};

VSOut VSMain(uint vertexId : SV_VertexID)
{
    VSOut output;
    float2 p = float2(vertexId == 2 ? 3.0 : -1.0, vertexId == 1 ? 3.0 : -1.0);
    output.position = float4(p, 0.0, 1.0);
    output.uv = p * float2(0.5, -0.5) + 0.5;
    return output;
}

float4 PSMain(VSOut input) : SV_Target
{
    float2 uv = (input.uv - g_offset) / g_scale;

    // Letterbox mode shows the background outside of the image; crop mode
    // clamps to the edge instead, so no bars ever appear.
    if (g_clamp < 0.5)
    {
        if (any(uv < 0.0) || any(uv > 1.0))
            return g_background;
    }

    uv = saturate(uv);
    return float4(g_texture.Sample(g_sampler, uv).rgb, 1.0);
}
)";

constexpr float kBackground[4] = {0.05f, 0.05f, 0.06f, 1.0f};

Renderer::CompileProc LoadD3DCompile() {
    // d3dcompiler_47 ships with Windows 10/11; 43 is a legacy fallback.
    for (const wchar_t* name : {L"d3dcompiler_47.dll", L"d3dcompiler_43.dll"}) {
        HMODULE module = LoadLibraryW(name);
        if (!module) continue;
        auto proc = reinterpret_cast<Renderer::CompileProc>(
            reinterpret_cast<void*>(GetProcAddress(module, "D3DCompile")));
        if (proc) return proc;
        FreeLibrary(module);
    }
    return nullptr;
}

std::string BlobText(ID3DBlob* blob) {
    if (!blob || !blob->GetBufferPointer()) return {};
    return std::string(static_cast<const char*>(blob->GetBufferPointer()),
                       blob->GetBufferSize());
}

}  // namespace

Renderer::~Renderer() {
    Shutdown();
}

bool Renderer::Init(std::string& error) {
    UINT flags = 0;
#ifdef _DEBUG
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
                                        D3D_FEATURE_LEVEL_10_0};

    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels,
                                   static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION,
                                   m_device.GetAddressOf(), &m_featureLevel,
                                   m_context.GetAddressOf());

    if (FAILED(hr) && (flags & D3D11_CREATE_DEVICE_DEBUG)) {
        // The debug layer is not installed on most machines.
        flags &= ~D3D11_CREATE_DEVICE_DEBUG;
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels,
                               static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION,
                               m_device.GetAddressOf(), &m_featureLevel,
                               m_context.GetAddressOf());
    }

    if (FAILED(hr)) {
        // Remote sessions and some VMs have no usable hardware device.
        LogHr(hr, "D3D11CreateDevice(HARDWARE), falling back to WARP");
        flags &= ~D3D11_CREATE_DEVICE_DEBUG;
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, levels,
                               static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION,
                               m_device.GetAddressOf(), &m_featureLevel,
                               m_context.GetAddressOf());
    }

    if (FAILED(hr)) {
        error = "D3D11CreateDevice: " + HrToString(hr);
        return false;
    }

    ComPtr<IDXGIDevice> dxgiDevice;
    hr = m_device.As(&dxgiDevice);
    if (FAILED(hr)) {
        error = "QueryInterface(IDXGIDevice): " + HrToString(hr);
        return false;
    }

    ComPtr<IDXGIAdapter> adapter;
    hr = dxgiDevice->GetAdapter(adapter.GetAddressOf());
    if (FAILED(hr)) {
        error = "IDXGIDevice::GetAdapter: " + HrToString(hr);
        return false;
    }

    hr = adapter->GetParent(IID_PPV_ARGS(m_factory.GetAddressOf()));
    if (FAILED(hr)) {
        error = "IDXGIAdapter::GetParent: " + HrToString(hr);
        return false;
    }

    m_compile = LoadD3DCompile();
    if (!m_compile) {
        error = "d3dcompiler_47.dll is not available";
        return false;
    }

    if (!CompileShaders(error)) return false;
    D3D11_SAMPLER_DESC samplerDesc{};
    samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    samplerDesc.MinLOD = 0.0f;
    samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
    hr = m_device->CreateSamplerState(&samplerDesc, m_sampler.GetAddressOf());
    if (FAILED(hr)) {
        error = "CreateSamplerState: " + HrToString(hr);
        return false;
    }

    D3D11_BUFFER_DESC bufferDesc{};
    bufferDesc.Usage = D3D11_USAGE_DYNAMIC;
    bufferDesc.ByteWidth = sizeof(Params);
    bufferDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bufferDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    hr = m_device->CreateBuffer(&bufferDesc, nullptr, m_paramsBuffer.GetAddressOf());
    if (FAILED(hr)) {
        error = "CreateBuffer(constants): " + HrToString(hr);
        return false;
    }

    m_deviceLost = false;
    LogPrintf("renderer initialized (feature level 0x%X)", static_cast<unsigned>(m_featureLevel));
    return true;
}

bool Renderer::CompileShaders(std::string& error) {
    UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#ifdef _DEBUG
    flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
    flags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif

    ComPtr<ID3DBlob> vsBlob;
    ComPtr<ID3DBlob> psBlob;
    ComPtr<ID3DBlob> errors;

    HRESULT hr = m_compile(kShaderSource, strlen(kShaderSource), "livewall.hlsl", nullptr,
                           nullptr, "VSMain", "vs_5_0", flags, 0, vsBlob.GetAddressOf(),
                           errors.GetAddressOf());
    if (FAILED(hr)) {
        error = "vertex shader: " + HrToString(hr) + "\n" + BlobText(errors.Get());
        return false;
    }

    errors.Reset();
    hr = m_compile(kShaderSource, strlen(kShaderSource), "livewall.hlsl", nullptr, nullptr,
                   "PSMain", "ps_5_0", flags, 0, psBlob.GetAddressOf(), errors.GetAddressOf());
    if (FAILED(hr)) {
        error = "pixel shader: " + HrToString(hr) + "\n" + BlobText(errors.Get());
        return false;
    }

    hr = m_device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(),
                                      nullptr, m_vs.GetAddressOf());
    if (FAILED(hr)) {
        error = "CreateVertexShader: " + HrToString(hr);
        return false;
    }

    hr = m_device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(),
                                     nullptr, m_ps.GetAddressOf());
    if (FAILED(hr)) {
        error = "CreatePixelShader: " + HrToString(hr);
        return false;
    }

    return true;
}

void Renderer::Shutdown() {
    m_targets.clear();
    m_frameSrv.Reset();
    m_frameTexture.Reset();
    m_paramsBuffer.Reset();
    m_sampler.Reset();
    m_ps.Reset();
    m_vs.Reset();
    m_factory.Reset();
    m_context.Reset();
    m_device.Reset();
    m_hasFrame = false;
    m_frameWidth = 0;
    m_frameHeight = 0;
    m_frameSerial = 0;
    m_deviceLost = false;
}

Renderer::Target* Renderer::FindTarget(HWND hwnd) {
    for (auto& target : m_targets) {
        if (target.hwnd == hwnd) return &target;
    }
    return nullptr;
}

bool Renderer::CreateTargetResources(Target& target, std::string& error) {
    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = target.width;
    desc.Height = target.height;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.Stereo = FALSE;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.Scaling = DXGI_SCALING_STRETCH;
    // Bitblt model: flip-model swap chains are fragile on cross-process child
    // windows, which is exactly what a wallpaper window is.
    desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    desc.Flags = 0;

    HRESULT hr = m_factory->CreateSwapChainForHwnd(m_device.Get(), target.hwnd, &desc, nullptr,
                                                   nullptr, target.swapChain.GetAddressOf());
    if (FAILED(hr)) {
        error = "CreateSwapChainForHwnd: " + HrToString(hr);
        return false;
    }

    ComPtr<ID3D11Texture2D> backBuffer;
    hr = target.swapChain->GetBuffer(0, IID_PPV_ARGS(backBuffer.GetAddressOf()));
    if (FAILED(hr)) {
        error = "GetBuffer: " + HrToString(hr);
        return false;
    }

    hr = m_device->CreateRenderTargetView(backBuffer.Get(), nullptr, target.rtv.GetAddressOf());
    if (FAILED(hr)) {
        error = "CreateRenderTargetView: " + HrToString(hr);
        return false;
    }
    return true;
}

bool Renderer::AttachWindow(HWND hwnd, uint32_t width, uint32_t height, std::string& error) {
    if (!hwnd || width == 0 || height == 0) return false;

    if (Target* existing = FindTarget(hwnd)) {
        if (existing->width != width || existing->height != height) {
            ResizeWindow(hwnd, width, height);
        }
        return true;
    }

    Target target;
    target.hwnd = hwnd;
    target.width = width;
    target.height = height;

    if (!CreateTargetResources(target, error)) {
        LogPrintf("attach failed for hwnd 0x%p: %s", hwnd, error.c_str());
        return false;
    }

    m_targets.push_back(std::move(target));
    LogPrintf("attached wallpaper window 0x%p (%ux%u)", hwnd, width, height);
    return true;
}

void Renderer::DetachWindow(HWND hwnd) {
    for (auto it = m_targets.begin(); it != m_targets.end(); ++it) {
        if (it->hwnd == hwnd) {
            m_targets.erase(it);
            LogPrintf("detached wallpaper window 0x%p", hwnd);
            return;
        }
    }
}

void Renderer::ResizeWindow(HWND hwnd, uint32_t width, uint32_t height) {
    Target* target = FindTarget(hwnd);
    if (!target || width == 0 || height == 0) return;
    if (target->width == width && target->height == height) return;

    m_context->OMSetRenderTargets(0, nullptr, nullptr);
    target->rtv.Reset();

    const HRESULT hr =
        target->swapChain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);
    if (FAILED(hr)) {
        LogHr(hr, "ResizeBuffers");
        return;
    }

    ComPtr<ID3D11Texture2D> backBuffer;
    if (FAILED(target->swapChain->GetBuffer(0, IID_PPV_ARGS(backBuffer.GetAddressOf())))) return;
    if (FAILED(m_device->CreateRenderTargetView(backBuffer.Get(), nullptr,
                                                target->rtv.GetAddressOf())))
        return;

    target->width = width;
    target->height = height;
    LogPrintf("resized wallpaper window 0x%p to %ux%u", hwnd, width, height);
}

bool Renderer::CreateFrameTexture(uint32_t width, uint32_t height) {
    ID3D11ShaderResourceView* nullView = nullptr;
    m_context->PSSetShaderResources(0, 1, &nullView);
    m_frameSrv.Reset();
    m_frameTexture.Reset();

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DYNAMIC;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    HRESULT hr = m_device->CreateTexture2D(&desc, nullptr, m_frameTexture.GetAddressOf());
    if (FAILED(hr)) {
        LogHr(hr, "CreateTexture2D(frame)");
        return false;
    }

    hr = m_device->CreateShaderResourceView(m_frameTexture.Get(), nullptr,
                                            m_frameSrv.GetAddressOf());
    if (FAILED(hr)) {
        LogHr(hr, "CreateShaderResourceView(frame)");
        return false;
    }

    m_frameWidth = width;
    m_frameHeight = height;
    m_frameSerial = 0;
    return true;
}

void Renderer::UploadFrame(const Frame& frame) {
    if (frame.Empty() || !m_context || m_deviceLost) return;
    // Skip only a frame we have definitely uploaded already. The size check is
    // redundant with unique serials, but keeps a reused serial from freezing a
    // resized wallpaper.
    if (frame.serial != 0 && frame.serial == m_frameSerial && m_frameTexture &&
        m_frameWidth == frame.width && m_frameHeight == frame.height) {
        return;
    }

    if (!m_frameTexture || m_frameWidth != frame.width || m_frameHeight != frame.height) {
        if (!CreateFrameTexture(frame.width, frame.height)) return;
    }

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(m_context->Map(m_frameTexture.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        LogPrintf("Map(frame texture) failed");
        return;
    }

    const size_t rowBytes = frame.Stride();
    for (uint32_t y = 0; y < frame.height; ++y) {
        memcpy(static_cast<uint8_t*>(mapped.pData) + static_cast<size_t>(y) * mapped.RowPitch,
               frame.pixels.data() + static_cast<size_t>(y) * rowBytes, rowBytes);
    }
    m_context->Unmap(m_frameTexture.Get(), 0);

    m_frameSerial = frame.serial;
    m_hasFrame = true;
}

bool Renderer::DrawTarget(Target& target) {
    m_context->OMSetRenderTargets(1, target.rtv.GetAddressOf(), nullptr);

    const D3D11_VIEWPORT viewport{0.0f, 0.0f, static_cast<float>(target.width),
                                  static_cast<float>(target.height), 0.0f, 1.0f};
    m_context->RSSetViewports(1, &viewport);

    if (!m_hasFrame || !m_frameSrv) {
        m_context->ClearRenderTargetView(target.rtv.Get(), kBackground);
    } else {
        Params params{};
        std::copy(std::begin(kBackground), std::end(kBackground), params.background);

        const float imageAspect =
            static_cast<float>(m_frameWidth) / static_cast<float>(m_frameHeight);
        const float targetAspect =
            static_cast<float>(target.width) / static_cast<float>(target.height);

        float scaleX = 1.0f;
        float scaleY = 1.0f;
        switch (m_mode) {
            case Mode::Fit:
                if (imageAspect > targetAspect) {
                    scaleY = targetAspect / imageAspect;
                } else {
                    scaleX = imageAspect / targetAspect;
                }
                break;
            case Mode::Fill:
                if (imageAspect > targetAspect) {
                    scaleX = imageAspect / targetAspect;
                } else {
                    scaleY = targetAspect / imageAspect;
                }
                break;
            case Mode::Stretch:
                break;
        }

        params.scale[0] = scaleX;
        params.scale[1] = scaleY;
        params.offset[0] = (1.0f - scaleX) * 0.5f;
        params.offset[1] = (1.0f - scaleY) * 0.5f;
        params.clampToEdge = (m_mode == Mode::Fill) ? 1.0f : 0.0f;

        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (SUCCEEDED(m_context->Map(m_paramsBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0,
                                     &mapped))) {
            memcpy(mapped.pData, &params, sizeof(params));
            m_context->Unmap(m_paramsBuffer.Get(), 0);
        }

        m_context->IASetInputLayout(nullptr);
        m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_context->VSSetShader(m_vs.Get(), nullptr, 0);
        m_context->PSSetShader(m_ps.Get(), nullptr, 0);
        m_context->PSSetConstantBuffers(0, 1, m_paramsBuffer.GetAddressOf());
        m_context->PSSetShaderResources(0, 1, m_frameSrv.GetAddressOf());
        m_context->PSSetSamplers(0, 1, m_sampler.GetAddressOf());
        m_context->Draw(3, 0);
    }

    const HRESULT hr = target.swapChain->Present(0, 0);
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
        LogPrintf("device lost (0x%08lX), full reinit required",
                  static_cast<unsigned long>(hr));
        m_deviceLost = true;
        return false;
    }
    if (FAILED(hr)) LogHr(hr, "Present");
    return true;
}

bool Renderer::Render() {
    if (m_deviceLost || !m_context) return false;

    for (auto& target : m_targets) {
        if (!target.swapChain || !target.rtv) continue;
        if (!DrawTarget(target)) return false;
    }
    return true;
}
