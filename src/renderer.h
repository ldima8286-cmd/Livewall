#pragma once

#include "common.h"
#include "media.h"

#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

// Renders frames into one swap chain per wallpaper window.
//
// Rendering is intentionally simple: a full-screen triangle with a pixel shader
// that samples the current frame and letterboxes / crops / stretches it
// according to the configured mode.
class Renderer {
public:
    enum class Mode { Fit, Fill, Stretch };

    // D3DCompile, resolved at runtime so the executable does not depend on a
    // specific d3dcompiler_XX.dll import library.
    using CompileProc = HRESULT(WINAPI*)(LPCVOID pSrcData, SIZE_T SrcDataSize,
                                         LPCSTR pSourceName, const D3D_SHADER_MACRO* pDefines,
                                         ID3DInclude* pInclude, LPCSTR pEntrypoint,
                                         LPCSTR pTarget, UINT Flags1, UINT Flags2,
                                         ID3DBlob** ppCode, ID3DBlob** ppErrorMsgs);

    Renderer() = default;
    ~Renderer();

    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    bool Init(std::string& error);
    void Shutdown();

    bool AttachWindow(HWND hwnd, uint32_t width, uint32_t height, std::string& error);
    void DetachWindow(HWND hwnd);
    void ResizeWindow(HWND hwnd, uint32_t width, uint32_t height);

    void UploadFrame(const Frame& frame);
    void DetachAllWindows() { m_targets.clear(); }
    void SetMode(Mode mode) { m_mode = mode; }
    Mode GetMode() const { return m_mode; }

    // Draws the current texture into every window. Returns false when the
    // device was lost and the whole pipeline must be recreated.
    bool Render();

    bool IsDeviceLost() const { return m_deviceLost; }
    size_t WindowCount() const { return m_targets.size(); }

private:
    struct Target {
        HWND hwnd = nullptr;
        Microsoft::WRL::ComPtr<IDXGISwapChain1> swapChain;
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv;
        uint32_t width = 0;
        uint32_t height = 0;
    };

    struct Params {
        float scale[2];
        float offset[2];
        float background[4];
        float clampToEdge;  // 1 = crop mode, 0 = letterbox
        float padding[3];    };

    Target* FindTarget(HWND hwnd);
    bool CreateTargetResources(Target& target, std::string& error);
    bool CreateFrameTexture(uint32_t width, uint32_t height);
    bool CompileShaders(std::string& error);
    bool DrawTarget(Target& target);

    Microsoft::WRL::ComPtr<ID3D11Device> m_device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_context;
    Microsoft::WRL::ComPtr<IDXGIFactory2> m_factory;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> m_vs;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_ps;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_paramsBuffer;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> m_sampler;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_frameSrv;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_frameTexture;

    std::vector<Target> m_targets;
    CompileProc m_compile = nullptr;
    Mode m_mode = Mode::Fit;
    uint32_t m_frameWidth = 0;
    uint32_t m_frameHeight = 0;
    uint64_t m_frameSerial = 0;
    bool m_deviceLost = false;
    bool m_hasFrame = false;
    D3D_FEATURE_LEVEL m_featureLevel = D3D_FEATURE_LEVEL_11_0;
};
