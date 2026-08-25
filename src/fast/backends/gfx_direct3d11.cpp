#ifdef ENABLE_DX11

#include <cstdio>
#include <vector>
#include <cmath>

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <unordered_map>

#include <windows.h>
#include <versionhelpers.h>
#include <wrl/client.h>

#include <dxgi1_3.h>
#include <d3d11.h>
#include <d3d11sdklayers.h>
#include <d3dcompiler.h>

#ifndef _LANGUAGE_C
#define _LANGUAGE_C
#endif

#include "fast/backends/gfx_window_manager_api.h"
#include "fast/backends/gfx_direct3d_common.h"
#include "fast/backends/gfx_shader_cache.h"

#define DECLARE_GFX_DXGI_FUNCTIONS
#include "fast/backends/gfx_dxgi.h"

#include "fast/backends/gfx_screen_config.h"
#include "ship/window/gui/Gui.h"
#include "fast/Fast3dGui.h"
#include "ship/Context.h"
#include "ship/config/ConsoleVariable.h"
#include "libultraship/bridge/consolevariablebridge.h"
#include "ship/window/Window.h"

#include "fast/backends/gfx_rendering_api.h"
#include "fast/backends/gfx_post_shader_pipeline.h"
#include "fast/interpreter.h"

#include <prism/processor.h>
#include "ship/config/ConsoleVariable.h"
#include <ship/resource/factory/ShaderFactory.h>
#include <ship/resource/ResourceManager.h>
#include "spdlog/spdlog.h"
#include <spdlog/fmt/fmt.h>
#include "nlohmann/json.hpp"
#include <stb_image.h>

#define DEBUG_D3D 0

// Temporary diagnostic: set to 1 to drain the D3D11 debug info queue into the
// log after each post-pipeline chain draw. Requires DEBUG_D3D 1.
#define DEBUG_D3D_CHAIN_INFOQUEUE 0

using namespace Microsoft::WRL; // For ComPtr

namespace Fast {

static size_t GetVertexFloatCount(const CCFeatures& ccFeatures) {
    size_t count = 4;
    for (size_t texture = 0; texture < 2; ++texture) {
        if (!ccFeatures.usedTextures[texture]) {
            continue;
        }

        count += 2;
        count += ccFeatures.clamp[texture][0] ? 1 : 0;
        count += ccFeatures.clamp[texture][1] ? 1 : 0;
    }

    count += ccFeatures.opt_fog ? 4 : 0;
    count += ccFeatures.opt_grayscale ? 4 : 0;
    count += ccFeatures.numInputs * (ccFeatures.opt_alpha ? 4 : 3);
    return count;
}

// CRT post-process pass (gEnhancements.Graphics.CRTShader / CustomShader). The vertex shader is
// attribute-less: a fullscreen triangle generated from SV_VertexID so the pass cannot leave stale
// vertex-buffer state behind for the next frame's first combiner draw.
static const char* sGdxPostVsSource = R"(
struct VSOutput {
    float4 pos : SV_Position;
    float2 vUV : TEXCOORD0;
};
// aDummy exists only so the input signature has one element: CreateInputLayout with zero
// elements returns E_INVALIDARG on some drivers, while a single declared-and-unbound
// attribute reads as zero everywhere. The triangle geometry still comes from SV_VertexID.
VSOutput VSMain(uint id : SV_VertexID, float2 aDummy : POSITION) {
    VSOutput o;
    float2 p = float2(id == 1 ? 3.0 : -1.0, id == 2 ? 3.0 : -1.0);
    o.pos = float4(p, 0.0, 1.0);
    o.vUV = p * 0.5 + 0.5;
    return o;
}
)";

static const char* sGdxPostCopyPsSource = R"(
Texture2D uTex : register(t0);
SamplerState uSampler : register(s0);
struct PSInput {
    float4 pos : SV_Position;
    float2 vUV : TEXCOORD0;
};
float4 PSMain(PSInput input) : SV_Target {
    return uTex.Sample(uSampler, input.vUV);
}
)";

// uSrcSize is the native N64 resolution the downsample target holds: scanlines key on source
// rows, not output pixels, so the line count stays authentic at any window size.
static const char* sGdxPostFsScanlines = R"(
Texture2D uTex : register(t0);
SamplerState uTexSampler : register(s0);
cbuffer PostCB : register(b0) {
    float2 uSrcSize;
    float2 uOutSize;
};
struct PSInput {
    float4 pos : SV_Position;
    float2 vUV : TEXCOORD0;
};
float4 PSMain(PSInput input) : SV_Target {
    float3 color = uTex.Sample(uTexSampler, input.vUV).rgb;
    float scan = 0.80 + 0.20 * cos(6.2831853 * (frac(input.vUV.y * uSrcSize.y) - 0.5));
    return float4(color * scan, 1.0);
}
)";

// Scanlines plus an aperture-grille mask: each output column keeps one channel at full gain.
// The gamma lift recovers the brightness the mask and scanlines remove.
static const char* sGdxPostFsCrt = R"(
Texture2D uTex : register(t0);
SamplerState uTexSampler : register(s0);
cbuffer PostCB : register(b0) {
    float2 uSrcSize;
    float2 uOutSize;
};
struct PSInput {
    float4 pos : SV_Position;
    float2 vUV : TEXCOORD0;
};
float4 PSMain(PSInput input) : SV_Target {
    float3 color = uTex.Sample(uTexSampler, input.vUV).rgb;
    float scan = 0.80 + 0.20 * cos(6.2831853 * (frac(input.vUV.y * uSrcSize.y) - 0.5));
    float phase = fmod(floor(input.vUV.x * uOutSize.x), 3.0);
    float3 mask = float3(0.75, 0.75, 0.75);
    if (phase < 0.5) {
        mask.r = 1.0;
    } else if (phase < 1.5) {
        mask.g = 1.0;
    } else {
        mask.b = 1.0;
    }
    color = pow(color * scan * mask, 0.85);
    return float4(color, 1.0);
}
)";

static const char* sGdxPostCustomPrelude = R"(
Texture2D uTex : register(t0);
SamplerState uTexSampler : register(s0);
cbuffer PostCB : register(b0) {
    float2 uSrcSize;
    float2 uOutSize;
};
struct PSInput {
    float4 pos : SV_Position;
    float2 vUV : TEXCOORD0;
};
)";

// Worker-thread half of post-shader compilation: D3DCompile is free-threaded, so the expensive
// source->bytecode step runs off the render thread. Device object creation stays on the main
// thread in GdxCreatePostProgramObjectsD3D11.
static bool GdxCompilePostShaderBlobsD3D11(pD3DCompile compileFn, const char* vsSource, const char* psSource,
                                           const char* pixelShaderProfile, ID3DBlob** outVs, ID3DBlob** outPs) {
#if DEBUG_D3D
    UINT compileFlags = D3DCOMPILE_DEBUG;
#else
    UINT compileFlags = D3DCOMPILE_OPTIMIZATION_LEVEL2;
#endif
    ComPtr<ID3DBlob> errorBlob;
    HRESULT hr = compileFn(vsSource, strlen(vsSource), nullptr, nullptr, nullptr, "VSMain", "vs_4_0", compileFlags, 0,
                           outVs, errorBlob.GetAddressOf());
    if (FAILED(hr) || *outVs == nullptr) {
        const char* err = errorBlob ? (const char*)errorBlob->GetBufferPointer() : "unknown error";
        SPDLOG_ERROR("Post vertex shader compilation failed: {}", err);
        return false;
    }

    hr = compileFn(psSource, strlen(psSource), nullptr, nullptr, nullptr, "PSMain", pixelShaderProfile, compileFlags, 0,
                   outPs, errorBlob.ReleaseAndGetAddressOf());
    if (FAILED(hr) || *outPs == nullptr) {
        const char* err = errorBlob ? (const char*)errorBlob->GetBufferPointer() : "unknown error";
        SPDLOG_ERROR("Post pixel shader compilation failed: {}", err);
        return false;
    }
    return true;
}

static bool GdxCompilePostShaderD3D11(GfxRenderingAPIDX11* self, const char* vsSource, const char* psSource,
                                      GfxRenderingAPIDX11::PostShaderProgramD3D11* outProgram, bool needsConstantBuffer,
                                      D3D11_FILTER samplerFilter = D3D11_FILTER_MIN_MAG_MIP_POINT,
                                      D3D11_TEXTURE_ADDRESS_MODE wrapU = D3D11_TEXTURE_ADDRESS_CLAMP,
                                      D3D11_TEXTURE_ADDRESS_MODE wrapV = D3D11_TEXTURE_ADDRESS_CLAMP,
                                      uint32_t constantBufferSize = 16,
                                      const char* pixelShaderProfile = "ps_4_0") {
    ComPtr<ID3DBlob> vs, ps;
    if (!GdxCompilePostShaderBlobsD3D11(self->mD3dCompile, vsSource, psSource, pixelShaderProfile,
                                        vs.GetAddressOf(), ps.GetAddressOf())) {
        return false;
    }
    return GdxCreatePostProgramObjectsD3D11(self, vs.Get(), ps.Get(), outProgram, needsConstantBuffer, samplerFilter,
                                            wrapU, wrapV, constantBufferSize);
}

static bool GdxCreatePostProgramObjectsD3D11(GfxRenderingAPIDX11* self, ID3DBlob* vsBlob, ID3DBlob* psBlob,
                                             GfxRenderingAPIDX11::PostShaderProgramD3D11* outProgram,
                                             bool needsConstantBuffer, D3D11_FILTER samplerFilter,
                                             D3D11_TEXTURE_ADDRESS_MODE wrapU, D3D11_TEXTURE_ADDRESS_MODE wrapV,
                                             uint32_t constantBufferSize) {
    if (FAILED(self->mDevice->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr,
                                                 outProgram->vertex_shader.GetAddressOf()))) {
        SPDLOG_ERROR("Post vertex shader creation failed");
        return false;
    }
    if (FAILED(self->mDevice->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr,
                                                outProgram->pixel_shader.GetAddressOf()))) {
        SPDLOG_ERROR("Post pixel shader creation failed");
        return false;
    }

    // The vertex shader generates the fullscreen triangle from SV_VertexID and declares one
    // unused POSITION attribute (see sGdxPostVsSource): a zero-element layout is rejected by
    // some drivers, while this single-element layout with no vertex buffer bound reads the
    // attribute as zero on every backend. Binding a real POSITION/TEXCOORD pair would instead
    // mismatch shaders that only consume vUV.
    static const D3D11_INPUT_ELEMENT_DESC kPostDummyLayout = {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0,
                                                              D3D11_INPUT_PER_VERTEX_DATA, 0};
    if (FAILED(self->mDevice->CreateInputLayout(&kPostDummyLayout, 1, vsBlob->GetBufferPointer(),
                                                vsBlob->GetBufferSize(), outProgram->input_layout.GetAddressOf()))) {
        SPDLOG_ERROR("Post input layout creation failed");
        return false;
    }

    if (needsConstantBuffer) {
        D3D11_BUFFER_DESC cbDesc;
        ZeroMemory(&cbDesc, sizeof(cbDesc));
        cbDesc.ByteWidth = constantBufferSize;
        cbDesc.Usage = D3D11_USAGE_DYNAMIC;
        cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(self->mDevice->CreateBuffer(&cbDesc, nullptr, outProgram->constant_buffer.GetAddressOf()))) {
            SPDLOG_ERROR("Post constant buffer creation failed");
            return false;
        }
    }

    D3D11_SAMPLER_DESC samplerDesc;
    ZeroMemory(&samplerDesc, sizeof(samplerDesc));
    samplerDesc.Filter = samplerFilter;
    samplerDesc.AddressU = wrapU;
    samplerDesc.AddressV = wrapV;
    samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.MinLOD = -D3D11_FLOAT32_MAX;
    samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(self->mDevice->CreateSamplerState(&samplerDesc, outProgram->sampler_state.GetAddressOf()))) {
        SPDLOG_ERROR("Post sampler state creation failed");
        return false;
    }

    D3D11_BLEND_DESC blendDesc;
    ZeroMemory(&blendDesc, sizeof(blendDesc));
    blendDesc.RenderTarget[0].BlendEnable = FALSE;
    blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(self->mDevice->CreateBlendState(&blendDesc, outProgram->blend_state.GetAddressOf()))) {
        SPDLOG_ERROR("Post blend state creation failed");
        return false;
    }

    D3D11_RASTERIZER_DESC rasterDesc;
    ZeroMemory(&rasterDesc, sizeof(rasterDesc));
    rasterDesc.FillMode = D3D11_FILL_SOLID;
    rasterDesc.CullMode = D3D11_CULL_NONE;
    rasterDesc.ScissorEnable = FALSE;
    if (FAILED(self->mDevice->CreateRasterizerState(&rasterDesc, outProgram->rasterizer_state.GetAddressOf()))) {
        SPDLOG_ERROR("Post rasterizer state creation failed");
        return false;
    }

    return true;
}

static bool GdxCompilePostCopyShaderD3D11(GfxRenderingAPIDX11* self, GfxRenderingAPIDX11::PostShaderProgramD3D11* outProgram) {
    return GdxCompilePostShaderD3D11(self, sGdxPostVsSource, sGdxPostCopyPsSource, outProgram, false,
                                     D3D11_FILTER_MIN_MAG_MIP_LINEAR);
}

static bool GdxCompilePostModeShaderD3D11(GfxRenderingAPIDX11* self, int mode, GfxRenderingAPIDX11::PostShaderProgramD3D11* outProgram) {
    return GdxCompilePostShaderD3D11(self, sGdxPostVsSource, mode == 1 ? sGdxPostFsScanlines : sGdxPostFsCrt,
                                     outProgram, true);
}

// Rate-limited INFO logging for the post-pass hot path so the active shader is visible without
// spamming the log on every frame.
static void GdxLogPostShaderInfo(const std::string& message) {
    static auto sLastLog = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration_cast<std::chrono::seconds>(now - sLastLog).count() >= 3) {
        sLastLog = now;
        SPDLOG_INFO("[post-shader] {}", message);
    }
}

static std::string GdxReadShaderFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return "";
    }
    std::ostringstream ss;
    ss << file.rdbuf();
    std::string result = ss.str();
    // Strip UTF-8 BOM so D3DCompile does not choke on it.
    if (result.size() >= 3 && (unsigned char)result[0] == 0xEF && (unsigned char)result[1] == 0xBB &&
        (unsigned char)result[2] == 0xBF) {
        result.erase(0, 3);
    }
    return result;
}

GfxRenderingAPIDX11::~GfxRenderingAPIDX11() {
    if (mPostPipelineBuild != nullptr) {
        mPostPipelineBuild->cancel = true;
        mPostPipelineBuild.reset();
    }
}

GfxRenderingAPIDX11::GfxRenderingAPIDX11(GfxWindowBackendDXGI* backend) {
    mWindowBackend = backend;
}

void GfxRenderingAPIDX11::CreateDepthStencilObjects(uint32_t width, uint32_t height, uint32_t msaa_count,
                                                    ID3D11DepthStencilView** view, ID3D11ShaderResourceView** srv) {
    D3D11_TEXTURE2D_DESC texture_desc;
    texture_desc.Width = width;
    texture_desc.Height = height;
    texture_desc.MipLevels = 1;
    texture_desc.ArraySize = 1;
    texture_desc.Format =
        mFeatureLevel >= D3D_FEATURE_LEVEL_10_0 ? DXGI_FORMAT_R32_TYPELESS : DXGI_FORMAT_R24G8_TYPELESS;
    texture_desc.SampleDesc.Count = msaa_count;
    texture_desc.SampleDesc.Quality = 0;
    texture_desc.Usage = D3D11_USAGE_DEFAULT;
    texture_desc.BindFlags = D3D11_BIND_DEPTH_STENCIL | (srv != nullptr ? D3D11_BIND_SHADER_RESOURCE : 0);
    texture_desc.CPUAccessFlags = 0;
    texture_desc.MiscFlags = 0;

    ComPtr<ID3D11Texture2D> texture;
    ThrowIfFailed(mDevice->CreateTexture2D(&texture_desc, nullptr, texture.GetAddressOf()));

    D3D11_DEPTH_STENCIL_VIEW_DESC view_desc;
    view_desc.Format = mFeatureLevel >= D3D_FEATURE_LEVEL_10_0 ? DXGI_FORMAT_D32_FLOAT : DXGI_FORMAT_D24_UNORM_S8_UINT;
    view_desc.Flags = 0;
    if (msaa_count > 1) {
        view_desc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DMS;
        view_desc.Texture2DMS.UnusedField_NothingToDefine = 0;
    } else {
        view_desc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
        view_desc.Texture2D.MipSlice = 0;
    }

    ThrowIfFailed(mDevice->CreateDepthStencilView(texture.Get(), &view_desc, view));

    if (srv != nullptr) {
        D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc;
        srv_desc.Format =
            mFeatureLevel >= D3D_FEATURE_LEVEL_10_0 ? DXGI_FORMAT_R32_FLOAT : DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        srv_desc.ViewDimension = msaa_count > 1 ? D3D11_SRV_DIMENSION_TEXTURE2DMS : D3D11_SRV_DIMENSION_TEXTURE2D;
        srv_desc.Texture2D.MostDetailedMip = 0;
        srv_desc.Texture2D.MipLevels = -1;

        ThrowIfFailed(mDevice->CreateShaderResourceView(texture.Get(), &srv_desc, srv));
    }
}
static bool CreateDeviceFunc(class GfxRenderingAPIDX11* self, bool SoftwareRenderer) {
#if DEBUG_D3D
    UINT device_creation_flags = D3D11_CREATE_DEVICE_DEBUG;
#else
    UINT device_creation_flags = 0;
#endif
    bool CreationFailed = false;
    const char HardwareText[320] = "\nUsing software renderer. Performance issues are to be expected.\n\n"
                                   "Please check your preferred GPU in Windows graphics or GPU driver settings and "
                                   "make sure you have the correct GPU drivers installed.\n\n"
                                   "You can also try to change the graphic backend of the port in its config file.\n"
                                   "Window->Backend->Id\n"
                                   "0 = DX11, 1 = OpenGL";
    const char SoftwareText[33] = "\nUsing software renderer failed.";

    if (SoftwareRenderer) {
        SPDLOG_INFO("Using software renderer.");
    }

    HRESULT res = self->mDX11CreateDevice(
        NULL, SoftwareRenderer ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE, nullptr, device_creation_flags, NULL,
        NULL, D3D11_SDK_VERSION, self->mDevice.GetAddressOf(), &self->mFeatureLevel, self->mContext.GetAddressOf());

    // Get and log name of adapter
    IDXGIDevice* DXGIDevice = nullptr;
    IDXGIAdapter* Adapter = nullptr;
    DXGI_ADAPTER_DESC adapterDesc;
    std::wstring adapterName;
    char adapterNameCStr[128] = "";
    char error_message[512];
    HRESULT res2;

    res2 = self->mDevice->QueryInterface(__uuidof(IDXGIDevice), (void**)&DXGIDevice);
    if (SUCCEEDED(res2)) {
        res2 = DXGIDevice->GetAdapter(&Adapter);
        if (SUCCEEDED(res2)) {
            res2 = Adapter->GetDesc(&adapterDesc);
            if (SUCCEEDED(res2)) {
                adapterName = adapterDesc.Description;
                wcstombs(adapterNameCStr, adapterName.c_str(), 128);
            }
            Adapter->Release();
        }
        DXGIDevice->Release();
    }
    SPDLOG_INFO("Using D3D adapter: {0}", adapterNameCStr);

    if (FAILED(res)) {
        CreationFailed = true;
        SPDLOG_WARN("Failed to create a D3D device. HRESULT: 0x{0:08x}", res);
        snprintf(error_message, sizeof(error_message), "Failed to create a D3D device on %s\nHRESULT: 0x%08X%s",
                 adapterNameCStr, res, SoftwareRenderer ? SoftwareText : HardwareText);
    }

    else if (self->mFeatureLevel < D3D_FEATURE_LEVEL_10_0) {
        CreationFailed = true;
        SPDLOG_WARN("D3D adapter doesn't support D3D feature level 10_0 or greater.");
        snprintf(error_message, sizeof(error_message), "%s doesn't support D3D feature level 10_0 or greater.%s",
                 adapterNameCStr, SoftwareRenderer ? SoftwareText : HardwareText);
    }

    else if (self->mFeatureLevel < D3D_FEATURE_LEVEL_10_1) {
        SPDLOG_WARN("D3D adapter doesn't support D3D feature level 10_1 or greater. MSAA setting will be ignored.");

    } else {
        // Check for Compute Shader support
        if (self->mDevice != NULL) {
            D3D11_FEATURE_DATA_D3D10_X_HARDWARE_OPTIONS features;
            self->mDevice->CheckFeatureSupport(D3D11_FEATURE_D3D10_X_HARDWARE_OPTIONS, &features,
                                               sizeof(D3D11_FEATURE_DATA_D3D10_X_HARDWARE_OPTIONS));
            if (features.ComputeShaders_Plus_RawAndStructuredBuffers_Via_Shader_4_x == false) {
                CreationFailed = true;
                SPDLOG_WARN("D3D adapter doesn't support compute shaders.");
                snprintf(error_message, sizeof(error_message), "%s doesn't support compute shaders.%s", adapterNameCStr,
                         SoftwareRenderer ? SoftwareText : HardwareText);
            }
        }
    }

    if (CreationFailed) {
        if (self->mContext) {
            self->mContext->Release();
        }
        if (self->mDevice) {
            self->mDevice->Release();
        }
        MessageBoxA(self->mWindowBackend->GetWindowHandle(), error_message, "Warning", MB_OK | MB_ICONWARNING);
        return false;
    }
    return true;
};

void GfxRenderingAPIDX11::Init() {
    // Cached state objects belong to the device that created them, and this is the only
    // device-creation path, so a re-Init must not inherit objects owned by the dead device.
    mDepthStencilCache.clear();
    mRasterizerCache.clear();

    // Load d3d11.dll
    mDX11Module = LoadLibraryW(L"d3d11.dll");
    if (mDX11Module == nullptr) {
        ThrowIfFailed(HRESULT_FROM_WIN32(GetLastError()), mWindowBackend->GetWindowHandle(), "d3d11.dll not found");
    }
    mDX11CreateDevice = (PFN_D3D11_CREATE_DEVICE)GetProcAddress(mDX11Module, "D3D11CreateDevice");

    // Load D3DCompiler_47.dll
    mCompilerModule = LoadLibraryW(L"D3DCompiler_47.dll");
    if (mCompilerModule == nullptr) {
        ThrowIfFailed(HRESULT_FROM_WIN32(GetLastError()), mWindowBackend->GetWindowHandle(),
                      "D3DCompiler_47.dll not found");
    }
    mD3dCompile = (pD3DCompile)GetProcAddress(mCompilerModule, "D3DCompile");
    mD3dCreateBlob = reinterpret_cast<decltype(mD3dCreateBlob)>(GetProcAddress(mCompilerModule, "D3DCreateBlob"));
    if (mD3dCreateBlob == nullptr) {
        ThrowIfFailed(HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND), mWindowBackend->GetWindowHandle(),
                      "D3DCreateBlob not found in D3DCompiler_47.dll");
    }

    // Create D3D11 mDevice

    mWindowBackend->CreateFactoryAndDevice(DEBUG_D3D, 11, this, CreateDeviceFunc);

    // Create the swap chain
    mWindowBackend->CreateSwapChain(mDevice.Get(), [this]() {
        mFrameBuffers[0].render_target_view.Reset();
        mTextures[mFrameBuffers[0].texture_id].texture.Reset();
        mContext->ClearState();
        mContext->Flush();

        mLastShaderProgram = nullptr;
        mLastVertexBufferStride = 0;
        mLastBlendState.Reset();
        for (int i = 0; i < SHADER_MAX_TEXTURES; i++) {
            mLastResourceViews[i].Reset();
            mLastSamplerStates[i].Reset();
        }
        mLastDepthTest = -1;
        mLastDepthMask = -1;
        mLastZmodeDecal = -1;
        mLastPrimitaveTopology = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
    });

    // Create D3D Debug mDevice if in debug mode

#if DEBUG_D3D
    ComPtr<ID3D11Debug> debug;
    ThrowIfFailed(mDevice->QueryInterface(__uuidof(ID3D11Debug), (void**)debug.GetAddressOf()),
                  mWindowBackend->GetWindowHandle(), "Failed to get ID3D11Debug device.");
#endif

    // Create the default framebuffer which represents the window
    FramebufferDX11& fb = mFrameBuffers[CreateFramebuffer()];

    // Check the size of the window
    DXGI_SWAP_CHAIN_DESC1 swap_chain_desc;
    ThrowIfFailed(mWindowBackend->GetSwapChain()->GetDesc1(&swap_chain_desc));
    mTextures[fb.texture_id].width = swap_chain_desc.Width;
    mTextures[fb.texture_id].height = swap_chain_desc.Height;
    fb.msaa_level = 1;

    for (uint32_t sample_count = 1; sample_count <= D3D11_MAX_MULTISAMPLE_SAMPLE_COUNT; sample_count++) {
        ThrowIfFailed(mDevice->CheckMultisampleQualityLevels(DXGI_FORMAT_R8G8B8A8_UNORM, sample_count,
                                                             &mMsaaNumQualityLevels[sample_count - 1]));
    }

    // Create main vertex buffer

    D3D11_BUFFER_DESC vertex_buffer_desc;
    ZeroMemory(&vertex_buffer_desc, sizeof(D3D11_BUFFER_DESC));

    vertex_buffer_desc.Usage = D3D11_USAGE_DYNAMIC;
    vertex_buffer_desc.ByteWidth = 256 * 32 * 3 * sizeof(float); // Same as buf_vbo size in gfx_pc
    vertex_buffer_desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    vertex_buffer_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    vertex_buffer_desc.MiscFlags = 0;

    ThrowIfFailed(mDevice->CreateBuffer(&vertex_buffer_desc, nullptr, mVertexBuffer.GetAddressOf()),
                  mWindowBackend->GetWindowHandle(), "Failed to create vertex buffer.");

    // Create per-frame constant buffer

    D3D11_BUFFER_DESC constant_buffer_desc;
    ZeroMemory(&constant_buffer_desc, sizeof(D3D11_BUFFER_DESC));

    constant_buffer_desc.Usage = D3D11_USAGE_DYNAMIC;
    constant_buffer_desc.ByteWidth = sizeof(PerFrameCB);
    constant_buffer_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    constant_buffer_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    constant_buffer_desc.MiscFlags = 0;

    ThrowIfFailed(mDevice->CreateBuffer(&constant_buffer_desc, nullptr, mPerFrameCb.GetAddressOf()),
                  mWindowBackend->GetWindowHandle(), "Failed to create per-frame constant buffer.");

    // Create per-draw constant buffer

    constant_buffer_desc.Usage = D3D11_USAGE_DYNAMIC;
    constant_buffer_desc.ByteWidth = sizeof(PerDrawCB);
    constant_buffer_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    constant_buffer_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    constant_buffer_desc.MiscFlags = 0;

    ThrowIfFailed(mDevice->CreateBuffer(&constant_buffer_desc, nullptr, mPerDrawCb.GetAddressOf()),
                  mWindowBackend->GetWindowHandle(), "Failed to create per-draw constant buffer.");

    // Create per-prim-depth constant buffer (G_ZS_PRIM), uploaded only when mPrimDepthDirty

    constant_buffer_desc.ByteWidth = sizeof(PerPrimDepthCB);
    ThrowIfFailed(mDevice->CreateBuffer(&constant_buffer_desc, nullptr, mPerPrimDepthCb.GetAddressOf()),
                  mWindowBackend->GetWindowHandle(), "Failed to create per-prim-depth constant buffer.");

    // G_AC_THRESHOLD: the RDP compares texel alpha against the SETBLENDCOLOR alpha register,
    // not a fixed constant, so the shader needs it as a uniform.
    constant_buffer_desc.ByteWidth = sizeof(PerAlphaThresholdCB);
    ThrowIfFailed(mDevice->CreateBuffer(&constant_buffer_desc, nullptr, mPerAlphaThresholdCb.GetAddressOf()),
                  mWindowBackend->GetWindowHandle(), "Failed to create per-alpha-threshold constant buffer.");

    // Create compute shader that can be used to retrieve depth buffer values

    const char* shader_source = R"(
sampler my_sampler : register(s0);
Texture2D<float> tex : register(t0);
StructuredBuffer<int2> coord : register(t1);
RWStructuredBuffer<float> output : register(u0);

[numthreads(1, 1, 1)]
void CSMain(uint3 DTid : SV_DispatchThreadID) {
    output[DTid.x] = tex.Load(int3(coord[DTid.x], 0));
}
)";

    const char* shader_source_msaa = R"(
sampler my_sampler : register(s0);
Texture2DMS<float, 2> tex : register(t0);
StructuredBuffer<int2> coord : register(t1);
RWStructuredBuffer<float> output : register(u0);

[numthreads(1, 1, 1)]
void CSMain(uint3 DTid : SV_DispatchThreadID) {
    output[DTid.x] = tex.Load(coord[DTid.x], 0);
}
)";

#if DEBUG_D3D
    UINT compile_flags = D3DCOMPILE_DEBUG;
#else
    UINT compile_flags = D3DCOMPILE_OPTIMIZATION_LEVEL2;
#endif

    ComPtr<ID3DBlob> cs, error_blob;
    HRESULT hr;

    hr = mD3dCompile(shader_source, strlen(shader_source), nullptr, nullptr, nullptr, "CSMain", "cs_4_0", compile_flags,
                     0, cs.GetAddressOf(), error_blob.GetAddressOf());

    if (FAILED(hr)) {
        char* err = (char*)error_blob->GetBufferPointer();
        MessageBoxA(mWindowBackend->GetWindowHandle(), err, "Error", MB_OK | MB_ICONERROR);
        throw Ship::HResultException(hr, "Compute shader compilation failed");
    }

    ThrowIfFailed(mDevice->CreateComputeShader(cs->GetBufferPointer(), cs->GetBufferSize(), nullptr,
                                               mComputeShader.GetAddressOf()));

    hr = mD3dCompile(shader_source_msaa, strlen(shader_source_msaa), nullptr, nullptr, nullptr, "CSMain", "cs_4_1",
                     compile_flags, 0, mComputeShaderMsaaBlob.GetAddressOf(), error_blob.ReleaseAndGetAddressOf());

    if (FAILED(hr)) {
        char* err = (char*)error_blob->GetBufferPointer();
        MessageBoxA(mWindowBackend->GetWindowHandle(), err, "Error", MB_OK | MB_ICONERROR);
        throw Ship::HResultException(hr, "MSAA compute shader compilation failed");
    }

    // Must run after InitResourceManager and InitConsoleVariables (see port/main.cpp) so the
    // archive seed is reachable and a rejected seed is reported before the first frame. DXBC is
    // driver-independent, so no backend fingerprint bits are needed here and a seed recorded on
    // one machine is valid everywhere. The compute shaders above stay uncached: fixed-source and
    // boot-time, so they never land inside a frame.
    mShaderCache.Init(SHADER_CACHE_TAG_D3D11, 0ull, "shadercache/d3d11.gdxshc",
                      "gdiffuser-shadercache-d3d11.bin");

    // Create ImGui

    Fast::GuiWindowInitData window_impl;
    window_impl.Dx11 = { mWindowBackend->GetWindowHandle(), mContext.Get(), mDevice.Get() };
    std::dynamic_pointer_cast<Fast::Fast3dGui>(Ship::Context::GetInstance()->GetWindow()->GetGui())->Init(window_impl);
}

int GfxRenderingAPIDX11::GetMaxTextureSize() {
    return mFeatureLevel <= D3D_FEATURE_LEVEL_10_1 ? 8192 : D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION;
}

const char* GfxRenderingAPIDX11::GetName() {
    return "DirectX 11";
}

struct GfxClipParameters GfxRenderingAPIDX11::GetClipParameters() {
    return { true, false };
}

void GfxRenderingAPIDX11::UnloadShader(struct ShaderProgram* old_prg) {
}

void GfxRenderingAPIDX11::LoadShader(struct ShaderProgram* new_prg) {
    mShaderProgram = (struct ShaderProgramD3D11*)new_prg;
}

void GfxRenderingAPIDX11::ClearShaderCache() {
    mShaderProgramPool.clear();
}

struct ShaderProgram* GfxRenderingAPIDX11::CreateAndLoadNewShader(uint64_t shader_id0, uint64_t shader_id1) {
    CCFeatures cc_features;
    gfx_cc_get_features(shader_id0, shader_id1, &cc_features);

    // Both change the generated HLSL without being part of the shader id, so they must be in
    // the cache key or a filter-mode switch resurrects the wrong bytecode.
    const uint32_t cacheFlags =
        (mCurrentFilterMode == FILTER_THREE_POINT ? (uint32_t)SHADER_CACHE_FLAG_THREE_POINT : 0u) |
        (mSrgbMode ? (uint32_t)SHADER_CACHE_FLAG_SRGB : 0u);

    // Keep the stored float count for cache compatibility, but derive the runtime stride from the
    // shader features so damaged metadata cannot reinterpret the vertex stream.
    const size_t numFloats = GetVertexFloatCount(cc_features);
    const uint8_t* vsBytes = nullptr;
    const uint8_t* psBytes = nullptr;
    size_t vsSize = 0;
    size_t psSize = 0;
    ComPtr<ID3DBlob> vs, ps;

    static constexpr size_t kPayloadHeader = 12;
    const std::vector<uint8_t>* cached = mShaderCache.Lookup(shader_id0, shader_id1, cacheFlags);
    if (cached != nullptr && cached->size() > kPayloadHeader) {
        uint32_t storedFloats = 0, storedVs = 0, storedPs = 0;
        memcpy(&storedFloats, cached->data(), sizeof(storedFloats));
        memcpy(&storedVs, cached->data() + 4, sizeof(storedVs));
        memcpy(&storedPs, cached->data() + 8, sizeof(storedPs));
        if ((size_t)storedVs + storedPs + kPayloadHeader == cached->size() && storedVs >= 4 && storedPs >= 4 &&
            memcmp(cached->data() + kPayloadHeader, "DXBC", 4) == 0 &&
            memcmp(cached->data() + kPayloadHeader + storedVs, "DXBC", 4) == 0) {
            const uint8_t* storedVsBytes = cached->data() + kPayloadHeader;
            const uint8_t* storedPsBytes = storedVsBytes + storedVs;
            if (storedFloats != numFloats) {
                SPDLOG_WARN("[shader-cache] d3d11 ignored invalid vertex stride: stored={} expected={} id0={:016X} "
                            "id1={:016X}",
                            storedFloats, numFloats, shader_id0, shader_id1);
            }

            // Cache hits start at an offset inside the packed payload. Recreate the ID3DBlob
            // representation returned by D3DCompile to avoid driver-dependent bytecode handling.
            ThrowIfFailed(mD3dCreateBlob(storedVs, vs.GetAddressOf()), mWindowBackend->GetWindowHandle(),
                          "Failed to allocate cached vertex shader blob");
            ThrowIfFailed(mD3dCreateBlob(storedPs, ps.GetAddressOf()), mWindowBackend->GetWindowHandle(),
                          "Failed to allocate cached pixel shader blob");
            memcpy(vs->GetBufferPointer(), storedVsBytes, storedVs);
            memcpy(ps->GetBufferPointer(), storedPsBytes, storedPs);
            vsBytes = static_cast<const uint8_t*>(vs->GetBufferPointer());
            vsSize = vs->GetBufferSize();
            psBytes = static_cast<const uint8_t*>(ps->GetBufferPointer());
            psSize = ps->GetBufferSize();
        }
    }

    if (vsBytes == nullptr) {
        size_t generatedNumFloats = 0;
        auto shader = gfx_direct3d_common_build_shader(generatedNumFloats, cc_features, false,
                                                       mCurrentFilterMode == FILTER_THREE_POINT, mSrgbMode);
        if (generatedNumFloats != numFloats) {
            SPDLOG_ERROR("D3D11 vertex layout mismatch: generated={} expected={} id0={:016X} id1={:016X}",
                         generatedNumFloats, numFloats, shader_id0, shader_id1);
            throw std::runtime_error("D3D11 vertex layout does not match the generated shader");
        }

        char* buf = shader.data();
        size_t len = shader.size();

        ComPtr<ID3DBlob> error_blob;

#if DEBUG_D3D
        UINT compile_flags = D3DCOMPILE_DEBUG;
#else
        UINT compile_flags = D3DCOMPILE_OPTIMIZATION_LEVEL2;
#endif

        // A runtime HLSL compile inside the draw call, 9-15ms each and arriving in bursts. It
        // was uninstrumented, which is why 100-180ms frame spikes had no attributable cause.
        // With the cache in place this only runs on a genuine miss, so it doubles as a miss log.
        const auto gdxShaderCompileStart = std::chrono::steady_clock::now();
        static int sGdxShaderCompileCount = 0;
        static double sGdxShaderCompileTotalMs = 0.0;

        HRESULT hr = mD3dCompile(buf, len, nullptr, nullptr, nullptr, "VSMain", "vs_4_0", compile_flags, 0,
                                 vs.GetAddressOf(), error_blob.GetAddressOf());

        if (FAILED(hr)) {
            char* err = (char*)error_blob->GetBufferPointer();
            MessageBoxA(mWindowBackend->GetWindowHandle(), err, "Error", MB_OK | MB_ICONERROR);
            throw Ship::HResultException(hr, "Vertex shader compilation failed");
        }

        hr = mD3dCompile(buf, len, nullptr, nullptr, nullptr, "PSMain", "ps_4_0", compile_flags, 0, ps.GetAddressOf(),
                         error_blob.GetAddressOf());

        if (FAILED(hr)) {
            char* err = (char*)error_blob->GetBufferPointer();
            MessageBoxA(mWindowBackend->GetWindowHandle(), err, "Error", MB_OK | MB_ICONERROR);
            throw Ship::HResultException(hr, "Pixel shader compilation failed");
        }

        vsBytes = (const uint8_t*)vs->GetBufferPointer();
        vsSize = vs->GetBufferSize();
        psBytes = (const uint8_t*)ps->GetBufferPointer();
        psSize = ps->GetBufferSize();

        {
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                                        gdxShaderCompileStart)
                                  .count();
            ++sGdxShaderCompileCount;
            sGdxShaderCompileTotalMs += ms;
            SPDLOG_ERROR("[shader-compile] d3d11 #{} id0={:016X} id1={:016X} {:.2f}ms (total {:.1f}ms)",
                         sGdxShaderCompileCount, shader_id0, shader_id1, ms, sGdxShaderCompileTotalMs);
        }

        std::vector<uint8_t> payload(kPayloadHeader + vsSize + psSize);
        const uint32_t storedFloats = (uint32_t)numFloats;
        const uint32_t storedVs = (uint32_t)vsSize;
        const uint32_t storedPs = (uint32_t)psSize;
        memcpy(payload.data(), &storedFloats, sizeof(storedFloats));
        memcpy(payload.data() + 4, &storedVs, sizeof(storedVs));
        memcpy(payload.data() + 8, &storedPs, sizeof(storedPs));
        memcpy(payload.data() + kPayloadHeader, vsBytes, vsSize);
        memcpy(payload.data() + kPayloadHeader + vsSize, psBytes, psSize);
        mShaderCache.Store(shader_id0, shader_id1, cacheFlags, payload.data(), payload.size());
    }

    struct ShaderProgramD3D11* prg = &mShaderProgramPool[std::make_pair(shader_id0, shader_id1)];

    ThrowIfFailed(mDevice->CreateVertexShader(vsBytes, vsSize, nullptr, prg->vertex_shader.GetAddressOf()));
    ThrowIfFailed(mDevice->CreatePixelShader(psBytes, psSize, nullptr, prg->pixel_shader.GetAddressOf()));

    // Input Layout

    D3D11_INPUT_ELEMENT_DESC ied[16];
    uint8_t ied_index = 0;
    ied[ied_index++] = {
        "POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0
    };
    for (UINT i = 0; i < 2; i++) {
        if (cc_features.usedTextures[i]) {
            ied[ied_index++] = {
                "TEXCOORD", i, DXGI_FORMAT_R32G32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0
            };
            if (cc_features.clamp[i][0]) {
                ied[ied_index++] = { "TEXCLAMPS",
                                     i,
                                     DXGI_FORMAT_R32_FLOAT,
                                     0,
                                     D3D11_APPEND_ALIGNED_ELEMENT,
                                     D3D11_INPUT_PER_VERTEX_DATA,
                                     0 };
            }
            if (cc_features.clamp[i][1]) {
                ied[ied_index++] = { "TEXCLAMPT",
                                     i,
                                     DXGI_FORMAT_R32_FLOAT,
                                     0,
                                     D3D11_APPEND_ALIGNED_ELEMENT,
                                     D3D11_INPUT_PER_VERTEX_DATA,
                                     0 };
            }
        }
    }
    if (cc_features.opt_fog) {
        ied[ied_index++] = {
            "FOG", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0
        };
    }
    if (cc_features.opt_grayscale) {
        ied[ied_index++] = { "GRAYSCALE",
                             0,
                             DXGI_FORMAT_R32G32B32A32_FLOAT,
                             0,
                             D3D11_APPEND_ALIGNED_ELEMENT,
                             D3D11_INPUT_PER_VERTEX_DATA,
                             0 };
    }
    for (unsigned int i = 0; i < cc_features.numInputs; i++) {
        DXGI_FORMAT format = cc_features.opt_alpha ? DXGI_FORMAT_R32G32B32A32_FLOAT : DXGI_FORMAT_R32G32B32_FLOAT;
        ied[ied_index++] = { "INPUT", i, format, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 };
    }

    ThrowIfFailed(mDevice->CreateInputLayout(ied, ied_index, vsBytes, vsSize, prg->input_layout.GetAddressOf()));

    // Blend state

    D3D11_BLEND_DESC blend_desc;
    ZeroMemory(&blend_desc, sizeof(D3D11_BLEND_DESC));

    if (cc_features.opt_alpha) {
        blend_desc.RenderTarget[0].BlendEnable = true;
        blend_desc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
        blend_desc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        blend_desc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        blend_desc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
        blend_desc.RenderTarget[0].DestBlendAlpha =
            D3D11_BLEND_ONE; // We initially clear alpha to 1.0f and want to keep it at 1.0f
        blend_desc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        blend_desc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    } else {
        blend_desc.RenderTarget[0].BlendEnable = false;
        blend_desc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    }

    ThrowIfFailed(mDevice->CreateBlendState(&blend_desc, prg->blend_state.GetAddressOf()));

    // Save some values

    prg->shader_id0 = shader_id0;
    prg->shader_id1 = shader_id1;
    prg->numInputs = cc_features.numInputs;
    prg->numFloats = numFloats;
    prg->usedTextures[0] = cc_features.usedTextures[0];
    prg->usedTextures[1] = cc_features.usedTextures[1];
    prg->usedTextures[2] = cc_features.used_masks[0];
    prg->usedTextures[3] = cc_features.used_masks[1];
    prg->usedTextures[4] = cc_features.used_blend[0];
    prg->usedTextures[5] = cc_features.used_blend[1];

    return (struct ShaderProgram*)(mShaderProgram = prg);
}

struct ShaderProgram* GfxRenderingAPIDX11::LookupShader(uint64_t shader_id0, uint64_t shader_id1) {
    auto it = mShaderProgramPool.find(std::make_pair(shader_id0, shader_id1));
    return it == mShaderProgramPool.end() ? nullptr : (struct ShaderProgram*)&it->second;
}

void GfxRenderingAPIDX11::ShaderGetInfo(struct ShaderProgram* prg, uint8_t* numInputs, bool usedTextures[2]) {
    struct ShaderProgramD3D11* p = (struct ShaderProgramD3D11*)prg;

    *numInputs = p->numInputs;
    usedTextures[0] = p->usedTextures[0];
    usedTextures[1] = p->usedTextures[1];
}

uint32_t GfxRenderingAPIDX11::NewTexture() {
    mTextures.resize(mTextures.size() + 1);
    return (uint32_t)(mTextures.size() - 1);
}

void GfxRenderingAPIDX11::DeleteTexture(uint32_t texID) {
    // glDeleteTextures(1, &texID);
}

void GfxRenderingAPIDX11::SelectTexture(int tile, uint32_t texture_id) {
    mCurrentTile = tile;
    mCurrentTextureIds[tile] = texture_id;
}

static D3D11_TEXTURE_ADDRESS_MODE gfx_cm_to_d3d11(uint32_t val) {
    if (val & G_TX_CLAMP) {
        // N64 MIRROR|CLAMP mirrors once and then clamps, which is MIRROR_ONCE, not MIRROR:
        // out-of-range coordinates must land on the far edge row, not row zero. Track guardrail
        // strips depend on it for their edge color.
        return (val & G_TX_MIRROR) ? D3D11_TEXTURE_ADDRESS_MIRROR_ONCE : D3D11_TEXTURE_ADDRESS_CLAMP;
    }
    return (val & G_TX_MIRROR) ? D3D11_TEXTURE_ADDRESS_MIRROR : D3D11_TEXTURE_ADDRESS_WRAP;
}

void GfxRenderingAPIDX11::UploadTexture(const uint8_t* rgba32_buf, uint32_t width, uint32_t height) {
    if (width == 0 || height == 0) {
        return;
    }

    // Create texture

    TextureData* texture_data = &mTextures[mCurrentTextureIds[mCurrentTile]];
    texture_data->width = width;
    texture_data->height = height;

    D3D11_TEXTURE2D_DESC texture_desc;
    ZeroMemory(&texture_desc, sizeof(D3D11_TEXTURE2D_DESC));

    texture_desc.Width = width;
    texture_desc.Height = height;
    texture_desc.Usage = D3D11_USAGE_IMMUTABLE;
    texture_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    texture_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texture_desc.CPUAccessFlags = 0;
    texture_desc.MiscFlags = 0;
    texture_desc.ArraySize = 1;
    texture_desc.MipLevels = 1;
    texture_desc.SampleDesc.Count = 1;
    texture_desc.SampleDesc.Quality = 0;

    D3D11_SUBRESOURCE_DATA resource_data;
    resource_data.pSysMem = rgba32_buf;
    resource_data.SysMemPitch = width * 4;
    resource_data.SysMemSlicePitch = resource_data.SysMemPitch * height;

    ThrowIfFailed(
        mDevice->CreateTexture2D(&texture_desc, &resource_data, texture_data->texture.ReleaseAndGetAddressOf()));

    // Create shader resource view from texture

    ThrowIfFailed(mDevice->CreateShaderResourceView(texture_data->texture.Get(), nullptr,
                                                    texture_data->resource_view.ReleaseAndGetAddressOf()));
}

void GfxRenderingAPIDX11::SetSamplerParameters(int tile, bool linear_filter, uint32_t cms, uint32_t cmt) {
    D3D11_SAMPLER_DESC sampler_desc;
    ZeroMemory(&sampler_desc, sizeof(D3D11_SAMPLER_DESC));

    sampler_desc.Filter = linear_filter && mCurrentFilterMode == FILTER_LINEAR ? D3D11_FILTER_MIN_MAG_MIP_LINEAR
                                                                               : D3D11_FILTER_MIN_MAG_MIP_POINT;

    sampler_desc.AddressU = gfx_cm_to_d3d11(cms);
    sampler_desc.AddressV = gfx_cm_to_d3d11(cmt);
    sampler_desc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    sampler_desc.MinLOD = 0;
    sampler_desc.MaxLOD = D3D11_FLOAT32_MAX;

    TextureData* texture_data = &mTextures[mCurrentTextureIds[tile]];
    texture_data->linear_filtering = linear_filter;

    // This function is called twice per texture, the first one only to set default values.
    // Maybe that could be skipped? Anyway, make sure to release the first default sampler
    // state before setting the actual one.
    texture_data->sampler_state.Reset();

    ThrowIfFailed(mDevice->CreateSamplerState(&sampler_desc, texture_data->sampler_state.GetAddressOf()));
}

void GfxRenderingAPIDX11::SetDepthTestAndMask(bool depth_test, bool depth_mask) {
    mCurrentDepthTest = depth_test;
    mCurrentDepthMask = depth_mask;
}

void GfxRenderingAPIDX11::SetCurrentPrimDepth(float depth) {
    if (depth != mCurrentPrimDepth) {
        mCurrentPrimDepth = depth;
        mPrimDepthDirty = true;
    }
}

void GfxRenderingAPIDX11::SetCurrentAlphaCompareThreshold(float threshold) {
    if (threshold != mCurrentAlphaCompareThreshold) {
        mCurrentAlphaCompareThreshold = threshold;
        mAlphaCompareThresholdDirty = true;
    }
}

void GfxRenderingAPIDX11::SetZmodeDecal(bool zmode_decal) {
    mCurrentZmodeDecal = zmode_decal;
}

void GfxRenderingAPIDX11::SetViewport(int x, int y, int width, int height) {
    D3D11_VIEWPORT viewport;
    viewport.TopLeftX = x;
    viewport.TopLeftY = mRenderTargetHeight - y - height;
    viewport.Width = width;
    viewport.Height = height;
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;

    mContext->RSSetViewports(1, &viewport);
}

void GfxRenderingAPIDX11::SetScissor(int x, int y, int width, int height) {
    D3D11_RECT rect;
    rect.left = x;
    rect.top = mRenderTargetHeight - y - height;
    rect.right = x + width;
    rect.bottom = mRenderTargetHeight - y;

    mContext->RSSetScissorRects(1, &rect);
}

void GfxRenderingAPIDX11::SetUseAlpha(bool use_alpha) {
    // Already part of the pipeline state from shader info
}

void GfxRenderingAPIDX11::DrawTriangles(float buf_vbo[], size_t buf_vbo_len, size_t buf_vbo_num_tris) {

    // mCurrentZmodeDecal has to be in this key because DepthFunc below derives from it. Without
    // it a draw that flips only the decal bit keeps a stale DepthFunc while the rasterizer's
    // SlopeScaledDepthBias does update, leaving depth state internally inconsistent. Upstream
    // added the decal term to DepthFunc in #612 without extending the key.
    if (mLastDepthTest != mCurrentDepthTest || mLastDepthMask != mCurrentDepthMask ||
        mLastZmodeDecal != mCurrentZmodeDecal) {
        mLastDepthTest = mCurrentDepthTest;
        mLastDepthMask = mCurrentDepthMask;

        // Only eight descriptors are reachable, so this is warm within the first few frames.
        const uint8_t depthKey = (uint8_t)((mCurrentDepthTest ? 1u : 0u) | (mCurrentDepthMask ? 2u : 0u) |
                                           (mCurrentZmodeDecal ? 4u : 0u));
        auto depthIt = mDepthStencilCache.find(depthKey);
        if (depthIt == mDepthStencilCache.end()) {
            D3D11_DEPTH_STENCIL_DESC depth_stencil_desc;
            ZeroMemory(&depth_stencil_desc, sizeof(D3D11_DEPTH_STENCIL_DESC));

            depth_stencil_desc.DepthEnable = mCurrentDepthTest || mCurrentDepthMask;
            depth_stencil_desc.DepthWriteMask =
                mCurrentDepthMask ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
            depth_stencil_desc.DepthFunc =
                mCurrentDepthTest ? (mCurrentZmodeDecal ? D3D11_COMPARISON_LESS_EQUAL : D3D11_COMPARISON_LESS)
                                  : D3D11_COMPARISON_ALWAYS;
            depth_stencil_desc.StencilEnable = false;

            Microsoft::WRL::ComPtr<ID3D11DepthStencilState> created;
            ThrowIfFailed(mDevice->CreateDepthStencilState(&depth_stencil_desc, created.GetAddressOf()));
            depthIt = mDepthStencilCache.emplace(depthKey, std::move(created)).first;
        }
        mDepthStencilState = depthIt->second;
        mContext->OMSetDepthStencilState(mDepthStencilState.Get(), 0);
    }

    if (mLastZmodeDecal != mCurrentZmodeDecal) {
        mLastZmodeDecal = mCurrentZmodeDecal;

        // Read outside the lookup because it is part of the key: a mid-run z-fighting-mode
        // change must miss rather than return the state built for the old mode.
        const int zFightingMode =
            Ship::Context::GetInstance()->GetConsoleVariables()->GetInteger(CVAR_Z_FIGHTING_MODE, 0);
        const uint64_t rasterKey = (uint64_t)(mCurrentZmodeDecal ? 1u : 0u) |
                                   ((uint64_t)(uint32_t)zFightingMode << 1) |
                                   ((uint64_t)(uint32_t)mRenderTargetHeight << 33);
        auto rasterIt = mRasterizerCache.find(rasterKey);
        if (rasterIt == mRasterizerCache.end()) {
            D3D11_RASTERIZER_DESC rasterizer_desc;
            ZeroMemory(&rasterizer_desc, sizeof(D3D11_RASTERIZER_DESC));

            rasterizer_desc.FillMode = D3D11_FILL_SOLID;
            rasterizer_desc.CullMode = D3D11_CULL_NONE;
            rasterizer_desc.FrontCounterClockwise = true;
            rasterizer_desc.DepthBias = 0;
            // SSDB = SlopeScaledDepthBias 120 leads to -2 at 240p which is the same as N64 mode which has very little
            // fighting
            const int n64modeFactor = 120;
            const int noVanishFactor = 100;
            float SSDB = -2;

            switch (zFightingMode) {
                case 1: // scaled z-fighting (N64 mode like)
                    SSDB = -1.0f * (float)mRenderTargetHeight / n64modeFactor;
                    break;
                case 2: // no vanishing paths
                    SSDB = -1.0f * (float)mRenderTargetHeight / noVanishFactor;
                    break;
                case 0: // disabled
                default:
                    SSDB = -2;
            }
            rasterizer_desc.SlopeScaledDepthBias = mCurrentZmodeDecal ? SSDB : 0.0f;
            rasterizer_desc.DepthBiasClamp = 0.0f;
            rasterizer_desc.DepthClipEnable = false;
            rasterizer_desc.ScissorEnable = true;
            rasterizer_desc.MultisampleEnable = false;
            rasterizer_desc.AntialiasedLineEnable = false;

            Microsoft::WRL::ComPtr<ID3D11RasterizerState> created;
            ThrowIfFailed(mDevice->CreateRasterizerState(&rasterizer_desc, created.GetAddressOf()));
            rasterIt = mRasterizerCache.emplace(rasterKey, std::move(created)).first;
        }
        mRasterizerState = rasterIt->second;
        mContext->RSSetState(mRasterizerState.Get());
    }

    bool textures_changed = false;

    for (int i = 0; i < SHADER_MAX_TEXTURES; i++) {
        if (mShaderProgram->usedTextures[i]) {
            // mTextures is append-only (NewTexture just resizes +1, DeleteTexture is a no-op),
            // so this is really just catching stale IDs left over from before we zero-initialized
            // mCurrentTextureIds. No entries are ever removed, so gaps aren't a concern.
            if (mCurrentTextureIds[i] >= mTextures.size()) {
                continue;
            }
            if (mLastResourceViews[i].Get() != mTextures[mCurrentTextureIds[i]].resource_view.Get()) {
                mLastResourceViews[i] = mTextures[mCurrentTextureIds[i]].resource_view.Get();
                mContext->PSSetShaderResources(i, 1, mTextures[mCurrentTextureIds[i]].resource_view.GetAddressOf());

                if (mCurrentFilterMode == FILTER_THREE_POINT) {
                    mPerDrawCbData.mTextures[i].width = mTextures[mCurrentTextureIds[i]].width;
                    mPerDrawCbData.mTextures[i].height = mTextures[mCurrentTextureIds[i]].height;
                    mPerDrawCbData.mTextures[i].linear_filtering = mTextures[mCurrentTextureIds[i]].linear_filtering;
                    textures_changed = true;
                }

                if (mLastSamplerStates[i].Get() != mTextures[mCurrentTextureIds[i]].sampler_state.Get()) {
                    mLastSamplerStates[i] = mTextures[mCurrentTextureIds[i]].sampler_state.Get();
                }
            }
            mContext->PSSetSamplers(i, 1, mTextures[mCurrentTextureIds[i]].sampler_state.GetAddressOf());
        }
    }

    // Set per-draw constant buffer
    if (textures_changed) {
        D3D11_MAPPED_SUBRESOURCE ms;
        ZeroMemory(&ms, sizeof(D3D11_MAPPED_SUBRESOURCE));
        mContext->Map(mPerDrawCb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &ms);
        memcpy(ms.pData, &mPerDrawCbData, sizeof(PerDrawCB));
        mContext->Unmap(mPerDrawCb.Get(), 0);
    }

    // G_ZS_PRIM: upload prim_depth cbuffer when it changed
    if (mPrimDepthDirty) {
        mPerPrimDepthCbData.prim_depth = mCurrentPrimDepth;
        D3D11_MAPPED_SUBRESOURCE ms;
        ZeroMemory(&ms, sizeof(D3D11_MAPPED_SUBRESOURCE));
        mContext->Map(mPerPrimDepthCb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &ms);
        memcpy(ms.pData, &mPerPrimDepthCbData, sizeof(PerPrimDepthCB));
        mContext->Unmap(mPerPrimDepthCb.Get(), 0);
        mPrimDepthDirty = false;
    }

    // G_AC_THRESHOLD: upload alpha-compare-threshold cbuffer when it changed
    if (mAlphaCompareThresholdDirty) {
        mPerAlphaThresholdCbData.alpha_compare_threshold = mCurrentAlphaCompareThreshold;
        D3D11_MAPPED_SUBRESOURCE ms2;
        ZeroMemory(&ms2, sizeof(D3D11_MAPPED_SUBRESOURCE));
        mContext->Map(mPerAlphaThresholdCb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &ms2);
        memcpy(ms2.pData, &mPerAlphaThresholdCbData, sizeof(PerAlphaThresholdCB));
        mContext->Unmap(mPerAlphaThresholdCb.Get(), 0);
        mAlphaCompareThresholdDirty = false;
    }

    // Set vertex buffer data

    D3D11_MAPPED_SUBRESOURCE ms;
    ZeroMemory(&ms, sizeof(D3D11_MAPPED_SUBRESOURCE));
    mContext->Map(mVertexBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &ms);
    memcpy(ms.pData, buf_vbo, buf_vbo_len * sizeof(float));
    mContext->Unmap(mVertexBuffer.Get(), 0);

    uint32_t stride = mShaderProgram->numFloats * sizeof(float);
    uint32_t offset = 0;

    if (mLastVertexBufferStride != stride) {
        mLastVertexBufferStride = stride;
        mContext->IASetVertexBuffers(0, 1, mVertexBuffer.GetAddressOf(), &stride, &offset);
    }

    if (mLastShaderProgram != mShaderProgram) {
        mLastShaderProgram = mShaderProgram;
        mContext->IASetInputLayout(mShaderProgram->input_layout.Get());
        mContext->VSSetShader(mShaderProgram->vertex_shader.Get(), 0, 0);
        mContext->PSSetShader(mShaderProgram->pixel_shader.Get(), 0, 0);

        if (mLastBlendState.Get() != mShaderProgram->blend_state.Get()) {
            mLastBlendState = mShaderProgram->blend_state.Get();
            mContext->OMSetBlendState(mShaderProgram->blend_state.Get(), 0, 0xFFFFFFFF);
        }
    }

    if (mLastPrimitaveTopology != D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST) {
        mLastPrimitaveTopology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        mContext->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    }

    mContext->Draw(buf_vbo_num_tris * 3, 0);
}

void GfxRenderingAPIDX11::OnResize() {
    // create_render_target_views(true);
}

void GfxRenderingAPIDX11::StartFrame() {
    // Set per-frame constant buffer
    ID3D11Buffer* buffers[4] = { mPerFrameCb.Get(), mPerDrawCb.Get(), mPerPrimDepthCb.Get(),
                                 mPerAlphaThresholdCb.Get() };
    mContext->PSSetConstantBuffers(0, 4, buffers);

    mFrameCount++;
    mPerFrameCbData.noise_frame++;
    if (mPerFrameCbData.noise_frame > 150) {
        // No high values, as noise starts to look ugly
        mPerFrameCbData.noise_frame = 0;
    }
}

void GfxRenderingAPIDX11::EndFrame() {
    mContext->Flush();
}

void GfxRenderingAPIDX11::FinishRender() {
}

int GfxRenderingAPIDX11::CreateFramebuffer() {
    uint32_t texture_id = NewTexture();
    TextureData& t = mTextures[texture_id];

    size_t index = mFrameBuffers.size();
    mFrameBuffers.resize(mFrameBuffers.size() + 1);
    FramebufferDX11& data = mFrameBuffers.back();
    data.texture_id = texture_id;

    uint32_t tile = 0;
    uint32_t saved = mCurrentTextureIds[tile];
    mCurrentTextureIds[tile] = texture_id;
    SetSamplerParameters(0, true, G_TX_WRAP, G_TX_WRAP);
    mCurrentTextureIds[tile] = saved;

    return (int)index;
}

void GfxRenderingAPIDX11::UpdateFramebufferParameters(int fb_id, uint32_t width, uint32_t height, uint32_t msaa_level,
                                                      bool opengl_invertY, bool render_target, bool has_depth_buffer,
                                                      bool can_extract_depth, GdxFramebufferFormat format) {
    FramebufferDX11& fb = mFrameBuffers[fb_id];
    TextureData& tex = mTextures[fb.texture_id];

    width = ((width) > (1U) ? (width) : (1U));
    height = ((height) > (1U) ? (height) : (1U));
    // We can't use MSAA the way we are using it on feature level 10_0 hardware, so disable it altogether.
    msaa_level = mFeatureLevel < D3D_FEATURE_LEVEL_10_1 ? 1 : msaa_level;
    while (msaa_level > 1 && mMsaaNumQualityLevels[msaa_level - 1] == 0) {
        --msaa_level;
    }

    DXGI_FORMAT dxgiFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    switch (format) {
        case GdxFramebufferFormat::R16G16B16A16_FLOAT:
            dxgiFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
            break;
        case GdxFramebufferFormat::R8G8B8A8_UNORM_SRGB:
            dxgiFormat = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
            break;
        default:
            break;
    }

    bool diff = tex.width != width || tex.height != height || fb.msaa_level != msaa_level || tex.lastFormat != dxgiFormat;
    const bool update_depth_buffer =
        has_depth_buffer &&
        (diff || !fb.has_depth_buffer || (fb.depth_stencil_srv.Get() != nullptr) != can_extract_depth);
    bool depth_updated = false;

    if (diff || (fb.render_target_view.Get() != nullptr) != render_target) {
        if (fb_id != 0) {
            D3D11_TEXTURE2D_DESC texture_desc;
            texture_desc.Width = width;
            texture_desc.Height = height;
            texture_desc.Usage = D3D11_USAGE_DEFAULT;
            texture_desc.BindFlags =
                (msaa_level <= 1 ? D3D11_BIND_SHADER_RESOURCE : 0) | (render_target ? D3D11_BIND_RENDER_TARGET : 0);
            texture_desc.Format = dxgiFormat;
            texture_desc.CPUAccessFlags = 0;
            texture_desc.MiscFlags = 0;
            texture_desc.ArraySize = 1;
            texture_desc.MipLevels = 1;
            texture_desc.SampleDesc.Count = msaa_level;
            texture_desc.SampleDesc.Quality = 0;

            ComPtr<ID3D11Texture2D> new_texture;
            ComPtr<ID3D11ShaderResourceView> new_resource_view;
            ComPtr<ID3D11RenderTargetView> new_render_target_view;
            ThrowIfFailed(mDevice->CreateTexture2D(&texture_desc, nullptr, new_texture.GetAddressOf()));

            if (msaa_level <= 1) {
                ThrowIfFailed(mDevice->CreateShaderResourceView(new_texture.Get(), nullptr,
                                                                new_resource_view.GetAddressOf()));
            }
            if (render_target) {
                ThrowIfFailed(
                    mDevice->CreateRenderTargetView(new_texture.Get(), nullptr, new_render_target_view.GetAddressOf()));
            }
            tex.lastFormat = dxgiFormat;

            ComPtr<ID3D11DepthStencilView> new_depth_stencil_view;
            ComPtr<ID3D11ShaderResourceView> new_depth_stencil_srv;
            if (update_depth_buffer) {
                CreateDepthStencilObjects(width, height, msaa_level, new_depth_stencil_view.GetAddressOf(),
                                          can_extract_depth ? new_depth_stencil_srv.GetAddressOf() : nullptr);
            }

            tex.texture = new_texture;
            tex.resource_view = new_resource_view;
            fb.render_target_view = new_render_target_view;
            if (update_depth_buffer) {
                fb.depth_stencil_view = new_depth_stencil_view;
                fb.depth_stencil_srv = new_depth_stencil_srv;
                depth_updated = true;
            }
        } else if (diff || (render_target && tex.texture.Get() == nullptr)) {
            DXGI_SWAP_CHAIN_DESC1 desc1;
            IDXGISwapChain1* swap_chain = mWindowBackend->GetSwapChain();
            ThrowIfFailed(swap_chain->GetDesc1(&desc1));
            if (desc1.Width != width || desc1.Height != height) {
                fb.render_target_view.Reset();
                tex.texture.Reset();
                ThrowIfFailed(swap_chain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, desc1.Flags));
            }
            ThrowIfFailed(
                swap_chain->GetBuffer(0, __uuidof(ID3D11Texture2D), (LPVOID*)tex.texture.ReleaseAndGetAddressOf()));
            if (render_target) {
                ComPtr<ID3D11RenderTargetView> new_render_target_view;
                ThrowIfFailed(
                    mDevice->CreateRenderTargetView(tex.texture.Get(), nullptr, new_render_target_view.GetAddressOf()));
                fb.render_target_view = new_render_target_view;
            }
        }

        tex.width = width;
        tex.height = height;
    }

    if (update_depth_buffer && !depth_updated) {
        ComPtr<ID3D11DepthStencilView> new_depth_stencil_view;
        ComPtr<ID3D11ShaderResourceView> new_depth_stencil_srv;
        CreateDepthStencilObjects(width, height, msaa_level, new_depth_stencil_view.GetAddressOf(),
                                  can_extract_depth ? new_depth_stencil_srv.GetAddressOf() : nullptr);
        fb.depth_stencil_view = new_depth_stencil_view;
        fb.depth_stencil_srv = new_depth_stencil_srv;
    }
    if (!has_depth_buffer) {
        fb.depth_stencil_view.Reset();
        fb.depth_stencil_srv.Reset();
    }

    fb.has_depth_buffer = has_depth_buffer;
    fb.msaa_level = msaa_level;
}

void GfxRenderingAPIDX11::StartDrawToFramebuffer(int fb_id, float noise_scale) {
    FramebufferDX11& fb = mFrameBuffers[fb_id];
    mRenderTargetHeight = mTextures[fb.texture_id].height;

    mContext->OMSetRenderTargets(1, fb.render_target_view.GetAddressOf(),
                                 fb.has_depth_buffer ? fb.depth_stencil_view.Get() : nullptr);

    mCurrentFramebuffer = fb_id;

    if (noise_scale != 0.0f) {
        mPerFrameCbData.noise_scale = 1.0f / noise_scale;
    }

    D3D11_MAPPED_SUBRESOURCE ms;
    ZeroMemory(&ms, sizeof(D3D11_MAPPED_SUBRESOURCE));
    mContext->Map(mPerFrameCb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &ms);
    memcpy(ms.pData, &mPerFrameCbData, sizeof(PerFrameCB));
    mContext->Unmap(mPerFrameCb.Get(), 0);
}

void GfxRenderingAPIDX11::ClearFramebuffer(bool color, bool depth) {
    FramebufferDX11& fb = mFrameBuffers[mCurrentFramebuffer];
    if (color) {
        const float clearColor[] = { 0.0f, 0.0f, 0.0f, 1.0f };
        mContext->ClearRenderTargetView(fb.render_target_view.Get(), clearColor);
    }
    if (depth && fb.has_depth_buffer) {
        mContext->ClearDepthStencilView(fb.depth_stencil_view.Get(), D3D11_CLEAR_DEPTH, 1.0f, 0);
    }
}

void GfxRenderingAPIDX11::ResolveMSAAColorBuffer(int fb_id_target, int fb_id_source) {
    FramebufferDX11& fb_dst = mFrameBuffers[fb_id_target];
    FramebufferDX11& fb_src = mFrameBuffers[fb_id_source];

    mContext->ResolveSubresource(mTextures[fb_dst.texture_id].texture.Get(), 0,
                                 mTextures[fb_src.texture_id].texture.Get(), 0, DXGI_FORMAT_R8G8B8A8_UNORM);
}

void* GfxRenderingAPIDX11::GetFramebufferTextureId(int fb_id) {
    return (void*)mTextures[mFrameBuffers[fb_id].texture_id].resource_view.Get();
}

uintptr_t GfxRenderingAPIDX11::ApplyPostShader(int srcFbId, int mode, uint32_t nativeW, uint32_t nativeH, uint32_t outW,
                                               uint32_t outH) {
    if (srcFbId <= 0 || srcFbId >= (int)mFrameBuffers.size() || nativeW == 0 || nativeH == 0 || outW == 0 ||
        outH == 0) {
        return 0;
    }

    const char* customStem = CVarGetString("gEnhancements.Graphics.CustomShader", "");
    PostShaderProgramD3D11* prg = nullptr;

    if (customStem != nullptr && customStem[0] != '\0') {
        // Reject any path-separator characters so the CVar cannot escape the shaders/ directory.
        if (strchr(customStem, '\\') != nullptr || strchr(customStem, '/') != nullptr ||
            strchr(customStem, '.') != nullptr) {
            SPDLOG_ERROR("Invalid custom shader stem '{}': must be a plain file name", customStem);
            return 0;
        }
        std::filesystem::path shaderPath = std::filesystem::path(Ship::Context::GetAppDirectoryPath()) / "shaders" /
                                           (std::string(customStem) + ".hlsl");
        GdxLogPostShaderInfo(fmt::format("scan/select: custom stem '{}' -> {}", customStem, shaderPath.string()));
        if (!std::filesystem::exists(shaderPath)) {
            SPDLOG_ERROR("Custom shader file not found: {}", shaderPath.string());
            return 0;
        }
        uint64_t mtimeTicks =
            static_cast<uint64_t>(std::filesystem::last_write_time(shaderPath).time_since_epoch().count());
        auto& entry = mPostShaderCustomCache[customStem];
        if (!entry.program.attempted || entry.mtime != mtimeTicks) {
            entry.mtime = mtimeTicks;
            entry.program = {}; // drop any stale device objects before recompiling
            entry.program.attempted = true;
            std::string customSource = GdxReadShaderFile(shaderPath);
            if (customSource.empty()) {
                SPDLOG_ERROR("Custom shader file is empty or could not be read: {}", shaderPath.string());
                return 0;
            }
            std::string fullSource = std::string(sGdxPostCustomPrelude) + customSource;
            if (!GdxCompilePostShaderD3D11(this, sGdxPostVsSource, fullSource.c_str(), &entry.program, true)) {
                SPDLOG_ERROR("Custom shader compile failed: {}", shaderPath.string());
                return 0;
            }
            GdxLogPostShaderInfo(fmt::format("compile: custom shader '{}' compiled successfully",
                                             shaderPath.string()));
        }
        prg = &entry.program;
    } else {
        if (mode < 1 || mode > 2) {
            return 0;
        }
        PostShaderProgramD3D11& modePrg = mPostShaderPrograms[mode - 1];
        if (!modePrg.attempted) {
            modePrg.attempted = true;
            if (!GdxCompilePostModeShaderD3D11(this, mode, &modePrg)) {
                SPDLOG_ERROR("Built-in post shader mode {} compile failed", mode);
                return 0;
            }
        }
        prg = &modePrg;
    }

    if (prg == nullptr || prg->vertex_shader.Get() == nullptr) {
        return 0;
    }

    if (!mPostCopyProgram.attempted) {
        mPostCopyProgram.attempted = true;
        if (!GdxCompilePostCopyShaderD3D11(this, &mPostCopyProgram)) {
            SPDLOG_ERROR("Post copy shader compile failed");
            return 0;
        }
    }
    if (mPostCopyProgram.vertex_shader.Get() == nullptr) {
        return 0;
    }

    if (mPostDownsampleFb < 0) {
        mPostDownsampleFb = CreateFramebuffer();
    }
    if (mPostOutputFb < 0) {
        mPostOutputFb = CreateFramebuffer();
    }

    UpdateFramebufferParameters(mPostDownsampleFb, nativeW, nativeH, 1, true, true, false, false);
    UpdateFramebufferParameters(mPostOutputFb, outW, outH, 1, true, true, false, false);

    // The post pass samples the downsample texture with NEAREST for crisp texels; the default
    // framebuffer sampler is LINEAR from CreateFramebuffer.
    {
        uint32_t saved = mCurrentTextureIds[0];
        mCurrentTextureIds[0] = mFrameBuffers[mPostDownsampleFb].texture_id;
        SetSamplerParameters(0, false, G_TX_CLAMP, G_TX_CLAMP);
        mCurrentTextureIds[0] = saved;
    }

    // Save device state so the interpreter's cached D3D11 state stays consistent with the context.
    ComPtr<ID3D11RenderTargetView> prevRTV;
    ComPtr<ID3D11DepthStencilView> prevDSV;
    mContext->OMGetRenderTargets(1, prevRTV.GetAddressOf(), prevDSV.GetAddressOf());

    D3D11_VIEWPORT prevViewport;
    UINT numViewports = 1;
    mContext->RSGetViewports(&numViewports, &prevViewport);

    ComPtr<ID3D11VertexShader> prevVS;
    ComPtr<ID3D11PixelShader> prevPS;
    ComPtr<ID3D11GeometryShader> prevGS;
    mContext->VSGetShader(prevVS.GetAddressOf(), nullptr, nullptr);
    mContext->PSGetShader(prevPS.GetAddressOf(), nullptr, nullptr);
    mContext->GSGetShader(prevGS.GetAddressOf(), nullptr, nullptr);

    ComPtr<ID3D11InputLayout> prevIL;
    mContext->IAGetInputLayout(prevIL.GetAddressOf());

    ComPtr<ID3D11Buffer> prevVB;
    UINT prevStride = 0, prevOffset = 0;
    mContext->IAGetVertexBuffers(0, 1, prevVB.GetAddressOf(), &prevStride, &prevOffset);

    D3D_PRIMITIVE_TOPOLOGY prevTopology;
    mContext->IAGetPrimitiveTopology(&prevTopology);

    ComPtr<ID3D11BlendState> prevBlend;
    float prevBlendFactor[4] = {};
    UINT prevSampleMask = 0xFFFFFFFF;
    mContext->OMGetBlendState(prevBlend.GetAddressOf(), prevBlendFactor, &prevSampleMask);

    ComPtr<ID3D11DepthStencilState> prevDepthStencil;
    UINT prevStencilRef = 0;
    mContext->OMGetDepthStencilState(prevDepthStencil.GetAddressOf(), &prevStencilRef);

    ComPtr<ID3D11RasterizerState> prevRasterizer;
    mContext->RSGetState(prevRasterizer.GetAddressOf());

    ComPtr<ID3D11ShaderResourceView> prevSRV;
    mContext->PSGetShaderResources(0, 1, prevSRV.GetAddressOf());

    ComPtr<ID3D11SamplerState> prevSampler;
    mContext->PSGetSamplers(0, 1, prevSampler.GetAddressOf());

    ComPtr<ID3D11Buffer> prevCB;
    mContext->PSGetConstantBuffers(0, 1, prevCB.GetAddressOf());

    // Step 1: linear downsample of the rendered frame to native resolution.
    {
        FramebufferDX11& dstFb = mFrameBuffers[mPostDownsampleFb];
        TextureData& srcTex = mTextures[mFrameBuffers[srcFbId].texture_id];
        mContext->OMSetRenderTargets(1, dstFb.render_target_view.GetAddressOf(), nullptr);

        D3D11_VIEWPORT viewport;
        viewport.TopLeftX = 0.0f;
        viewport.TopLeftY = 0.0f;
        viewport.Width = static_cast<float>(nativeW);
        viewport.Height = static_cast<float>(nativeH);
        viewport.MinDepth = 0.0f;
        viewport.MaxDepth = 1.0f;
        mContext->RSSetViewports(1, &viewport);

        mContext->IASetInputLayout(mPostCopyProgram.input_layout.Get());
        mContext->VSSetShader(mPostCopyProgram.vertex_shader.Get(), nullptr, 0);
        mContext->PSSetShader(mPostCopyProgram.pixel_shader.Get(), nullptr, 0);
        mContext->PSSetShaderResources(0, 1, srcTex.resource_view.GetAddressOf());
        mContext->PSSetSamplers(0, 1, mPostCopyProgram.sampler_state.GetAddressOf());
        mContext->PSSetConstantBuffers(0, 0, nullptr);
        mContext->OMSetBlendState(mPostCopyProgram.blend_state.Get(), nullptr, 0xFFFFFFFF);
        mContext->OMSetDepthStencilState(nullptr, 0);
        mContext->RSSetState(mPostCopyProgram.rasterizer_state.Get());
        mContext->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        mContext->Draw(3, 0);
    }

    // Step 2: fullscreen triangle through the post shader into the output target.
    {
        FramebufferDX11& srcFb = mFrameBuffers[mPostDownsampleFb];
        FramebufferDX11& dstFb = mFrameBuffers[mPostOutputFb];
        TextureData& srcTex = mTextures[srcFb.texture_id];

        GdxLogPostShaderInfo(fmt::format("apply: custom='{}' mode={} {}x{} -> {}x{}",
                                          customStem != nullptr ? customStem : "", mode, nativeW, nativeH, outW, outH));

        if (prg->constant_buffer.Get() != nullptr) {
            D3D11_MAPPED_SUBRESOURCE ms;
            float cbData[4] = { static_cast<float>(nativeW), static_cast<float>(nativeH),
                                static_cast<float>(outW), static_cast<float>(outH) };
            if (SUCCEEDED(mContext->Map(prg->constant_buffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) {
                memcpy(ms.pData, cbData, sizeof(cbData));
                mContext->Unmap(prg->constant_buffer.Get(), 0);
            }
            ID3D11Buffer* cb = prg->constant_buffer.Get();
            mContext->PSSetConstantBuffers(0, 1, &cb);
        }

        mContext->OMSetRenderTargets(1, dstFb.render_target_view.GetAddressOf(), nullptr);

        D3D11_VIEWPORT viewport;
        viewport.TopLeftX = 0.0f;
        viewport.TopLeftY = 0.0f;
        viewport.Width = static_cast<float>(outW);
        viewport.Height = static_cast<float>(outH);
        viewport.MinDepth = 0.0f;
        viewport.MaxDepth = 1.0f;
        mContext->RSSetViewports(1, &viewport);

        mContext->IASetInputLayout(prg->input_layout.Get());
        mContext->VSSetShader(prg->vertex_shader.Get(), nullptr, 0);
        mContext->PSSetShader(prg->pixel_shader.Get(), nullptr, 0);
        mContext->PSSetShaderResources(0, 1, srcTex.resource_view.GetAddressOf());
        mContext->PSSetSamplers(0, 1, prg->sampler_state.GetAddressOf());
        mContext->OMSetBlendState(prg->blend_state.Get(), nullptr, 0xFFFFFFFF);
        mContext->OMSetDepthStencilState(nullptr, 0);
        mContext->RSSetState(prg->rasterizer_state.Get());
        mContext->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        mContext->Draw(3, 0);
    }

    // Restore device state and invalidate backend caches so the next draw re-binds anything changed.
    mContext->OMSetRenderTargets(1, prevRTV.GetAddressOf(), prevDSV.Get());
    if (numViewports > 0) {
        mContext->RSSetViewports(1, &prevViewport);
    }
    mContext->VSSetShader(prevVS.Get(), nullptr, 0);
    mContext->PSSetShader(prevPS.Get(), nullptr, 0);
    mContext->GSSetShader(prevGS.Get(), nullptr, 0);
    mContext->IASetInputLayout(prevIL.Get());
    mContext->IASetVertexBuffers(0, 1, prevVB.GetAddressOf(), &prevStride, &prevOffset);
    mContext->IASetPrimitiveTopology(prevTopology);
    mContext->OMSetBlendState(prevBlend.Get(), prevBlendFactor, prevSampleMask);
    mContext->OMSetDepthStencilState(prevDepthStencil.Get(), prevStencilRef);
    mContext->RSSetState(prevRasterizer.Get());
    mContext->PSSetShaderResources(0, 1, prevSRV.GetAddressOf());
    mContext->PSSetSamplers(0, 1, prevSampler.GetAddressOf());
    mContext->PSSetConstantBuffers(0, 1, prevCB.GetAddressOf());

    mLastShaderProgram = nullptr;
    mLastVertexBufferStride = 0;
    mLastBlendState.Reset();
    for (int i = 0; i < SHADER_MAX_TEXTURES; i++) {
        mLastResourceViews[i].Reset();
        mLastSamplerStates[i].Reset();
    }
    mLastPrimitaveTopology = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;

    return reinterpret_cast<uintptr_t>(GetFramebufferTextureId(mPostOutputFb));
}

static D3D11_TEXTURE_ADDRESS_MODE GdxPipelineWrapModeD3D11(GdxPostPassDesc::WrapMode mode) {
    switch (mode) {
        case GdxPostPassDesc::WrapMode::ClampToBorder:
            return D3D11_TEXTURE_ADDRESS_BORDER;
        case GdxPostPassDesc::WrapMode::Repeat:
            return D3D11_TEXTURE_ADDRESS_WRAP;
        case GdxPostPassDesc::WrapMode::MirroredRepeat:
            return D3D11_TEXTURE_ADDRESS_MIRROR;
        case GdxPostPassDesc::WrapMode::ClampToEdge:
        default:
            return D3D11_TEXTURE_ADDRESS_CLAMP;
    }
}

static D3D11_FILTER GdxPipelineFilterD3D11(bool linear) {
    return linear ? D3D11_FILTER_MIN_MAG_MIP_LINEAR : D3D11_FILTER_MIN_MAG_MIP_POINT;
}

static std::filesystem::path GdxResolvePipelineShaderPathD3D11(const std::filesystem::path& presetDir,
                                                               const std::string& stem) {
    // lexically_normal: pack presets carry ".." chains that can push the joined
    // path past MAX_PATH even when the real target is short.
    std::filesystem::path slangPath = (presetDir / (stem + ".slang")).lexically_normal();
    if (std::filesystem::exists(slangPath)) {
        return slangPath;
    }
    return (presetDir / (stem + ".hlsl")).lexically_normal();
}

bool GdxCompilePipelinePassD3D11(GfxRenderingAPIDX11* self, const std::filesystem::path& path,
                                        GfxRenderingAPIDX11::PostShaderProgramD3D11* outProgram, bool filterLinear,
                                        GdxPostPassDesc::WrapMode wrapS, GdxPostPassDesc::WrapMode wrapT) {
    outProgram->isSlang = false;
    outProgram->slangParameters.clear();
    outProgram->slangCbSize = 0;
    outProgram->slangUsedBuiltins = {};

    const bool isSlang = path.extension() == ".slang";
    if (isSlang) {
        // glslang/SPIRV-Cross throw on pathological inputs (Mega_Bezel-scale chains);
        // a failed pass must fall back to the unfiltered image, never crash the game.
        GdxSlangTranslation trans;
        try {
            trans = GdxTranslateSlangFile(path, GdxSlangTarget::HlslSm50);
        } catch (const std::exception& e) {
            SPDLOG_ERROR("Slang translation threw for {}: {}", path.string(), e.what());
            return false;
        } catch (...) {
            SPDLOG_ERROR("Slang translation threw for {}", path.string());
            return false;
        }
        GdxDumpSlangTranslation(path, GdxSlangTarget::HlslSm50, trans.source);
        if (trans.hasVertexStage) {
            GdxDumpSlangTranslation(path.parent_path() / (path.stem().string() + "-vs.slang"),
                                    GdxSlangTarget::HlslSm50, trans.vertexSource);
        }
        if (!trans.ok) {
            SPDLOG_ERROR("Slang translation failed for {}: {}", path.string(), trans.error);
            return false;
        }
        outProgram->isSlang = true;
        outProgram->slangParameters = trans.parameters;
        outProgram->slangCbSize =
            GdxSlangHlslCBufferLayout::TotalSize(trans.parameters, trans.usedBuiltins.extraSizeNames.size());
        outProgram->slangUsedBuiltins = trans.usedBuiltins;
        // A translated vertex stage keeps the fullscreen-triangle draw (its
        // Position/TexCoord inputs are synthesized from SV_VertexID).
        const char* vsSource = trans.hasVertexStage ? trans.vertexSource.c_str() : sGdxPostVsSource;
        return GdxCompilePostShaderD3D11(self, vsSource, trans.source.c_str(), outProgram, true,
                                         GdxPipelineFilterD3D11(filterLinear), GdxPipelineWrapModeD3D11(wrapS),
                                         GdxPipelineWrapModeD3D11(wrapT), outProgram->slangCbSize, "ps_5_0");
    }

    std::string customSource = GdxReadShaderFile(path);
    if (customSource.empty()) {
        SPDLOG_ERROR("Pipeline pass shader is empty or could not be read: {}", path.string());
        return false;
    }
    std::string fullSource = std::string(sGdxPostCustomPrelude) + customSource;
    return GdxCompilePostShaderD3D11(self, sGdxPostVsSource, fullSource.c_str(), outProgram, true,
                                     GdxPipelineFilterD3D11(filterLinear), GdxPipelineWrapModeD3D11(wrapS),
                                     GdxPipelineWrapModeD3D11(wrapT));
}


// Builds every pass of a post pipeline off the render thread: file reads, slang translation and
// D3DCompile are all free-threaded. Only device object creation (install) needs the main thread.
static void GdxBuildPostPipelineWorkerD3D11(pD3DCompile compileFn, std::shared_ptr<GdxPostPipelineBuildD3D11> build,
                                            GdxPostShaderPipeline pipeline) {
    const std::filesystem::path presetDir = pipeline.presetPath.parent_path();
    build->passes.resize(pipeline.passes.size());
    for (size_t i = 0; i < pipeline.passes.size(); ++i) {
        if (build->cancel.load()) {
            return;
        }
        GdxPostPassBuildD3D11& pass = build->passes[i];
        const std::filesystem::path shaderPath = GdxResolvePipelineShaderPathD3D11(presetDir, pipeline.passes[i].shader);
        pass.cacheKey = shaderPath.string();
        try {
            pass.mtime = static_cast<uint64_t>(std::filesystem::last_write_time(shaderPath).time_since_epoch().count());
        } catch (...) {
            SPDLOG_ERROR("Pipeline pass shader not found: {}", shaderPath.string());
            build->state = 2;
            return;
        }
        pass.samplerFilter = GdxPipelineFilterD3D11(pipeline.passes[i].filterLinear);
        pass.wrapU = GdxPipelineWrapModeD3D11(pipeline.passes[i].wrapS);
        pass.wrapV = GdxPipelineWrapModeD3D11(pipeline.passes[i].wrapT);

        const char* vsSource = sGdxPostVsSource;
        std::string vsStorage;
        std::string psStorage;
        const char* pixelShaderProfile = "ps_4_0";
        if (shaderPath.extension() == ".slang") {
            GdxSlangTranslation trans;
            try {
                trans = GdxTranslateSlangFile(shaderPath, GdxSlangTarget::HlslSm50);
            } catch (const std::exception& e) {
                SPDLOG_ERROR("Slang translation threw for {}: {}", shaderPath.string(), e.what());
                build->state = 2;
                return;
            } catch (...) {
                SPDLOG_ERROR("Slang translation threw for {}", shaderPath.string());
                build->state = 2;
                return;
            }
            GdxDumpSlangTranslation(shaderPath, GdxSlangTarget::HlslSm50, trans.source);
            if (trans.hasVertexStage) {
                GdxDumpSlangTranslation(shaderPath.parent_path() / (shaderPath.stem().string() + "-vs.slang"),
                                        GdxSlangTarget::HlslSm50, trans.vertexSource);
            }
            if (!trans.ok) {
                SPDLOG_ERROR("Slang translation failed for {}: {}", shaderPath.string(), trans.error);
                build->state = 2;
                return;
            }
            pass.isSlang = true;
            pass.slangParameters = trans.parameters;
            pass.slangCbSize =
                GdxSlangHlslCBufferLayout::TotalSize(trans.parameters, trans.usedBuiltins.extraSizeNames.size());
            pass.slangUsedBuiltins = trans.usedBuiltins;
            if (trans.hasVertexStage) {
                vsStorage = std::move(trans.vertexSource);
                vsSource = vsStorage.c_str();
            }
            psStorage = std::move(trans.source);
            pixelShaderProfile = "ps_5_0";
        } else {
            std::string customSource = GdxReadShaderFile(shaderPath);
            if (customSource.empty()) {
                SPDLOG_ERROR("Pipeline pass shader is empty or could not be read: {}", shaderPath.string());
                build->state = 2;
                return;
            }
            psStorage = std::string(sGdxPostCustomPrelude) + customSource;
        }
        if (build->cancel.load()) {
            return;
        }
        if (!GdxCompilePostShaderBlobsD3D11(compileFn, vsSource, psStorage.c_str(), pixelShaderProfile,
                                            pass.vsBlob.GetAddressOf(), pass.psBlob.GetAddressOf())) {
            build->state = 2;
            return;
        }
        pass.ok = true;
    }
    build->state = 1;
}

// Main-thread half of the async pipeline build: turns compiled bytecode into D3D11 device objects.
static bool GdxInstallPostPipelineBuildD3D11(GfxRenderingAPIDX11* self, GdxPostPipelineBuildD3D11* build) {
    for (GdxPostPassBuildD3D11& pass : build->passes) {
        GfxRenderingAPIDX11::PostShaderProgramD3D11 prg = {};
        prg.attempted = true;
        prg.mtime = pass.mtime;
        prg.isSlang = pass.isSlang;
        prg.slangParameters = std::move(pass.slangParameters);
        prg.slangCbSize = pass.slangCbSize;
        prg.slangUsedBuiltins = std::move(pass.slangUsedBuiltins);
        if (!GdxCreatePostProgramObjectsD3D11(self, pass.vsBlob.Get(), pass.psBlob.Get(), &prg, true,
                                              pass.samplerFilter, pass.wrapU, pass.wrapV,
                                              pass.isSlang ? pass.slangCbSize : 16)) {
            self->mPostPipelineProgramCache[pass.cacheKey] = std::move(prg);
            return false;
        }
        self->mPostPipelineProgramCache[pass.cacheKey] = std::move(prg);
    }
    return true;
}

GdxSlangUsedBuiltins GdxUnionUsedBuiltins(const std::vector<GfxRenderingAPIDX11::PostShaderProgramD3D11*>& programs) {
    GdxSlangUsedBuiltins result;
    for (const GfxRenderingAPIDX11::PostShaderProgramD3D11* prg : programs) {
        result.maxOriginalHistory = std::max(result.maxOriginalHistory, prg->slangUsedBuiltins.maxOriginalHistory);
        result.passOutputIndices.insert(prg->slangUsedBuiltins.passOutputIndices.begin(),
                                        prg->slangUsedBuiltins.passOutputIndices.end());
        result.passFeedbackIndices.insert(prg->slangUsedBuiltins.passFeedbackIndices.begin(),
                                          prg->slangUsedBuiltins.passFeedbackIndices.end());
        result.userTextureIndices.insert(prg->slangUsedBuiltins.userTextureIndices.begin(),
                                         prg->slangUsedBuiltins.userTextureIndices.end());
    }
    return result;
}

static DXGI_FORMAT GdxPipelineFramebufferFormatDxgi(const GdxPostPassDesc& pass) {
    if (pass.floatFramebuffer) {
        return DXGI_FORMAT_R16G16B16A16_FLOAT;
    }
    if (pass.srgbFramebuffer) {
        return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    }
    return DXGI_FORMAT_R8G8B8A8_UNORM;
}

static GdxFramebufferFormat GdxPipelineFramebufferFormat(const GdxPostPassDesc& pass) {
    if (pass.floatFramebuffer) {
        return GdxFramebufferFormat::R16G16B16A16_FLOAT;
    }
    if (pass.srgbFramebuffer) {
        return GdxFramebufferFormat::R8G8B8A8_UNORM_SRGB;
    }
    return GdxFramebufferFormat::R8G8B8A8_UNORM;
}

bool GdxLoadPipelineLutsD3D11(GfxRenderingAPIDX11* self, const GdxPostShaderPipeline& pipeline,
                                     std::vector<GfxRenderingAPIDX11::PostPipelineLutD3D11>* outLuts,
                                     std::vector<std::filesystem::path>* outPaths) {
    const std::filesystem::path presetDir = pipeline.presetPath.parent_path();

    outLuts->clear();
    outPaths->clear();
    outLuts->reserve(pipeline.textureOrder.size());
    outPaths->reserve(pipeline.textureOrder.size());

    for (const std::string& name : pipeline.textureOrder) {
        GfxRenderingAPIDX11::PostPipelineLutD3D11 lut;
        auto it = pipeline.textures.find(name);
        if (it == pipeline.textures.end()) {
            outLuts->push_back(std::move(lut));
            outPaths->push_back(std::filesystem::path());
            continue;
        }

        std::filesystem::path imagePath = (presetDir / it->second.path).lexically_normal();
        outPaths->push_back(imagePath);

        int w = 0, h = 0, channels = 0;
        stbi_uc* pixels = stbi_load(imagePath.string().c_str(), &w, &h, &channels, 4);
        if (pixels == nullptr) {
            SPDLOG_WARN("Could not load LUT texture '{}': {}", imagePath.string(), stbi_failure_reason());
            outLuts->push_back(std::move(lut));
            continue;
        }
        lut.width = static_cast<uint32_t>(w);
        lut.height = static_cast<uint32_t>(h);

        D3D11_TEXTURE2D_DESC textureDesc;
        ZeroMemory(&textureDesc, sizeof(textureDesc));
        textureDesc.Width = static_cast<UINT>(w);
        textureDesc.Height = static_cast<UINT>(h);
        textureDesc.MipLevels = 1;
        textureDesc.ArraySize = 1;
        textureDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        textureDesc.SampleDesc.Count = 1;
        textureDesc.SampleDesc.Quality = 0;
        textureDesc.Usage = D3D11_USAGE_IMMUTABLE;
        textureDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        textureDesc.CPUAccessFlags = 0;
        textureDesc.MiscFlags = 0;

        D3D11_SUBRESOURCE_DATA resourceData;
        resourceData.pSysMem = pixels;
        resourceData.SysMemPitch = static_cast<UINT>(w * 4);
        resourceData.SysMemSlicePitch = resourceData.SysMemPitch * static_cast<UINT>(h);

        if (FAILED(self->mDevice->CreateTexture2D(&textureDesc, &resourceData, lut.texture.GetAddressOf()))) {
            SPDLOG_WARN("Failed to create LUT texture '{}'", imagePath.string());
            stbi_image_free(pixels);
            outLuts->push_back(std::move(lut));
            continue;
        }

        if (FAILED(self->mDevice->CreateShaderResourceView(lut.texture.Get(), nullptr, lut.srv.GetAddressOf()))) {
            SPDLOG_WARN("Failed to create LUT SRV '{}'", imagePath.string());
            stbi_image_free(pixels);
            outLuts->push_back(std::move(lut));
            continue;
        }

        D3D11_SAMPLER_DESC samplerDesc;
        ZeroMemory(&samplerDesc, sizeof(samplerDesc));
        samplerDesc.Filter = it->second.linear ? D3D11_FILTER_MIN_MAG_MIP_LINEAR : D3D11_FILTER_MIN_MAG_MIP_POINT;
        samplerDesc.AddressU = GdxPipelineWrapModeD3D11(it->second.wrap);
        samplerDesc.AddressV = GdxPipelineWrapModeD3D11(it->second.wrap);
        samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        samplerDesc.MinLOD = -D3D11_FLOAT32_MAX;
        samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
        if (FAILED(self->mDevice->CreateSamplerState(&samplerDesc, lut.sampler.GetAddressOf()))) {
            SPDLOG_WARN("Failed to create LUT sampler '{}'", imagePath.string());
        }

        stbi_image_free(pixels);
        outLuts->push_back(std::move(lut));
    }
    return true;
}

// `<Alias>` / `<Alias>Feedback` named samplers and `<Alias>Size` uniforms refer
// to passes by their `aliasN =` key; resolve the name to a pass index.
static int GdxFindPipelinePassByAlias(const GdxPostShaderPipeline& pipeline, const std::string& alias) {
    for (size_t i = 0; i < pipeline.passes.size(); ++i) {
        if (!pipeline.passes[i].alias.empty() && pipeline.passes[i].alias == alias) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

uintptr_t GfxRenderingAPIDX11::ApplyPostShaderChain(int srcFbId, const GdxPostShaderPipeline& pipeline,
                                                    uint32_t nativeW, uint32_t nativeH, uint32_t outW, uint32_t outH) {
    if (srcFbId <= 0 || srcFbId >= (int)mFrameBuffers.size() || nativeW == 0 || nativeH == 0 || outW == 0 ||
        outH == 0 || pipeline.passes.empty()) {
        return 0;
    }

    const std::filesystem::path presetDir = pipeline.presetPath.parent_path();

    // Async pipeline build: the preset key includes the folded pass mtimes, so any disk edit
    // retriggers a rebuild. While a build runs we publish the unfiltered frame; a failed build is
    // latched so it is not retried every frame.
    const std::string chainKey = pipeline.presetPath.generic_string() + "#" + std::to_string(pipeline.mtime);
    if (chainKey != mPostPipelineReadyKey) {
        if (chainKey == mPostPipelineFailedKey) {
            return 0;
        }
        if (mPostPipelineBuild != nullptr && mPostPipelineBuild->key == chainKey) {
            const int state = mPostPipelineBuild->state.load();
            if (state == 0) {
                return 0;
            }
            if (state == 2 || !GdxInstallPostPipelineBuildD3D11(this, mPostPipelineBuild.get())) {
                mPostPipelineFailedKey = chainKey;
                mPostPipelineBuild.reset();
                return 0;
            }
            mPostPipelineBuild.reset();
            mPostPipelineReadyKey = chainKey;
        } else {
            bool allCached = true;
            for (size_t i = 0; i < pipeline.passes.size(); ++i) {
                const std::filesystem::path sp = GdxResolvePipelineShaderPathD3D11(presetDir, pipeline.passes[i].shader);
                uint64_t mt = 0;
                try {
                    mt = static_cast<uint64_t>(std::filesystem::last_write_time(sp).time_since_epoch().count());
                } catch (...) {
                    allCached = false;
                    break;
                }
                auto it = mPostPipelineProgramCache.find(sp.string());
                if (it == mPostPipelineProgramCache.end() || it->second.mtime != mt ||
                    it->second.vertex_shader.Get() == nullptr) {
                    allCached = false;
                    break;
                }
            }
            if (allCached) {
                mPostPipelineReadyKey = chainKey;
                mPostPipelineFailedKey.clear();
            } else {
                if (mPostPipelineBuild != nullptr) {
                    mPostPipelineBuild->cancel = true;
                }
                auto build = std::make_shared<GdxPostPipelineBuildD3D11>();
                build->key = chainKey;
                std::thread(GdxBuildPostPipelineWorkerD3D11, mD3dCompile, build, pipeline).detach();
                mPostPipelineBuild = std::move(build);
                return 0;
            }
        }
    }

    // Compile (or retrieve cached) programs for every pass. The cache key is the full shader path
    // so the same pass file reused across presets still reloads correctly when its mtime changes.
    std::vector<PostShaderProgramD3D11*> programs;
    programs.reserve(pipeline.passes.size());
    for (size_t i = 0; i < pipeline.passes.size(); ++i) {
        const std::string& shaderStem = pipeline.passes[i].shader;
        const std::filesystem::path shaderPath = GdxResolvePipelineShaderPathD3D11(presetDir, shaderStem);
        const std::string cacheKey = shaderPath.string();

        uint64_t mtimeTicks = 0;
        try {
            mtimeTicks = static_cast<uint64_t>(std::filesystem::last_write_time(shaderPath).time_since_epoch().count());
        } catch (...) {
            SPDLOG_ERROR("Pipeline pass shader not found: {}", shaderPath.string());
            return 0;
        }

        auto it = mPostPipelineProgramCache.find(cacheKey);
        if (it == mPostPipelineProgramCache.end() || it->second.mtime != mtimeTicks) {
            PostShaderProgramD3D11 prg = {};
            prg.attempted = true;
            prg.mtime = mtimeTicks;
            if (!GdxCompilePipelinePassD3D11(this, shaderPath, &prg, pipeline.passes[i].filterLinear,
                                             pipeline.passes[i].wrapS, pipeline.passes[i].wrapT)) {
                SPDLOG_ERROR("Pipeline pass shader compile failed: {}", shaderPath.string());
                mPostPipelineProgramCache[cacheKey] = std::move(prg);
                return 0;
            }
            mPostPipelineProgramCache[cacheKey] = std::move(prg);
            it = mPostPipelineProgramCache.find(cacheKey);
        }
        if (it->second.vertex_shader.Get() == nullptr) {
            return 0;
        }
        programs.push_back(&it->second);
    }

    const GdxSlangUsedBuiltins used = GdxUnionUsedBuiltins(programs);
    const size_t historySize = used.maxOriginalHistory >= 0 ? static_cast<size_t>(used.maxOriginalHistory + 1) : 0;

    // Allocate N+1 framebuffers: [0] is the first-pass input (native size), [1..N-1] are
    // intermediate pass outputs, and [N] is the final pass output.
    const size_t neededFbs = pipeline.passes.size() + 1;
    while (mPostPipelineFbs.size() < neededFbs) {
        mPostPipelineFbs.push_back(CreateFramebuffer());
    }

    // Allocate history ring buffers if any pass references OriginalHistoryN.
    while (mPostPipelineHistoryFbs.size() < historySize) {
        mPostPipelineHistoryFbs.push_back(CreateFramebuffer());
    }

    // Allocate one feedback framebuffer per pass that feeds back. The preset's
    // feedback_pass flag is one source; a pass is also fed back when any shader
    // samples PassFeedbackN or an `<Alias>Feedback` named sampler (Mega_Bezel
    // uses the latter without ever setting the flag).
    if (mPostPipelineFeedbackFbs.size() < pipeline.passes.size()) {
        mPostPipelineFeedbackFbs.resize(pipeline.passes.size(), -1);
    }
    std::set<int> feedbackNeeded;
    for (size_t i = 0; i < pipeline.passes.size(); ++i) {
        if (pipeline.passes[i].feedbackPass) {
            feedbackNeeded.insert(static_cast<int>(i));
        }
    }
    for (const PostShaderProgramD3D11* prg : programs) {
        feedbackNeeded.insert(prg->slangUsedBuiltins.passFeedbackIndices.begin(),
                              prg->slangUsedBuiltins.passFeedbackIndices.end());
        for (const std::string& samplerName : prg->slangUsedBuiltins.namedSamplers) {
            if (samplerName.size() > 8 && samplerName.compare(samplerName.size() - 8, 8, "Feedback") == 0) {
                const int passIdx =
                    GdxFindPipelinePassByAlias(pipeline, samplerName.substr(0, samplerName.size() - 8));
                if (passIdx >= 0) {
                    feedbackNeeded.insert(passIdx);
                }
            }
        }
    }
    for (int i : feedbackNeeded) {
        if (i >= 0 && i < (int)pipeline.passes.size() && mPostPipelineFeedbackFbs[i] < 0) {
            mPostPipelineFeedbackFbs[i] = CreateFramebuffer();
        }
    }

    // Load LUT textures if the pipeline's LUT set changed. A change in order or
    // path triggers a reload; everything else reuses the existing GPU textures.
    bool lutsChanged = mPostPipelineLuts.size() != pipeline.textureOrder.size();
    if (!lutsChanged) {
        for (size_t i = 0; i < pipeline.textureOrder.size(); ++i) {
            auto it = pipeline.textures.find(pipeline.textureOrder[i]);
            std::filesystem::path expectedPath;
            if (it != pipeline.textures.end()) {
                expectedPath = presetDir / it->second.path;
            }
            if (mPostPipelineLutPaths[i] != expectedPath) {
                lutsChanged = true;
                break;
            }
        }
    }
    if (lutsChanged) {
        mPostPipelineLuts.clear();
        GdxLoadPipelineLutsD3D11(this, pipeline, &mPostPipelineLuts, &mPostPipelineLutPaths);
    }

    // Shared clamp sampler for history/feedback/pass-output lookups.
    if (mPostPipelineClampSampler.Get() == nullptr) {
        D3D11_SAMPLER_DESC clampDesc;
        ZeroMemory(&clampDesc, sizeof(clampDesc));
        clampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        clampDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        clampDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        clampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        clampDesc.MinLOD = -D3D11_FLOAT32_MAX;
        clampDesc.MaxLOD = D3D11_FLOAT32_MAX;
        if (FAILED(mDevice->CreateSamplerState(&clampDesc, mPostPipelineClampSampler.GetAddressOf()))) {
            SPDLOG_ERROR("Failed to create pipeline clamp sampler");
            return 0;
        }
    }

    // Compile the copy shader used to downsample the source framebuffer to native resolution.
    if (!mPostCopyProgram.attempted) {
        mPostCopyProgram.attempted = true;
        if (!GdxCompilePostCopyShaderD3D11(this, &mPostCopyProgram)) {
            SPDLOG_ERROR("Post copy shader compile failed");
            return 0;
        }
    }
    if (mPostCopyProgram.vertex_shader.Get() == nullptr) {
        return 0;
    }

    // Save device state so the interpreter's cached D3D11 state stays consistent with the context.
    ComPtr<ID3D11RenderTargetView> prevRTV;
    ComPtr<ID3D11DepthStencilView> prevDSV;
    mContext->OMGetRenderTargets(1, prevRTV.GetAddressOf(), prevDSV.GetAddressOf());

    D3D11_VIEWPORT prevViewport;
    UINT numViewports = 1;
    mContext->RSGetViewports(&numViewports, &prevViewport);

    ComPtr<ID3D11VertexShader> prevVS;
    ComPtr<ID3D11PixelShader> prevPS;
    ComPtr<ID3D11GeometryShader> prevGS;
    mContext->VSGetShader(prevVS.GetAddressOf(), nullptr, nullptr);
    mContext->PSGetShader(prevPS.GetAddressOf(), nullptr, nullptr);
    mContext->GSGetShader(prevGS.GetAddressOf(), nullptr, nullptr);

    ComPtr<ID3D11InputLayout> prevIL;
    mContext->IAGetInputLayout(prevIL.GetAddressOf());

    ComPtr<ID3D11Buffer> prevVB;
    UINT prevStride = 0, prevOffset = 0;
    mContext->IAGetVertexBuffers(0, 1, prevVB.GetAddressOf(), &prevStride, &prevOffset);

    D3D_PRIMITIVE_TOPOLOGY prevTopology;
    mContext->IAGetPrimitiveTopology(&prevTopology);

    ComPtr<ID3D11BlendState> prevBlend;
    float prevBlendFactor[4] = {};
    UINT prevSampleMask = 0xFFFFFFFF;
    mContext->OMGetBlendState(prevBlend.GetAddressOf(), prevBlendFactor, &prevSampleMask);

    ComPtr<ID3D11DepthStencilState> prevDepthStencil;
    UINT prevStencilRef = 0;
    mContext->OMGetDepthStencilState(prevDepthStencil.GetAddressOf(), &prevStencilRef);

    ComPtr<ID3D11RasterizerState> prevRasterizer;
    mContext->RSGetState(prevRasterizer.GetAddressOf());

    ComPtr<ID3D11ShaderResourceView> prevSRV;
    mContext->PSGetShaderResources(0, 1, prevSRV.GetAddressOf());

    ComPtr<ID3D11SamplerState> prevSampler;
    mContext->PSGetSamplers(0, 1, prevSampler.GetAddressOf());

    ComPtr<ID3D11Buffer> prevCB;
    mContext->PSGetConstantBuffers(0, 1, prevCB.GetAddressOf());

    // Step 1: linear downsample of the rendered frame to native resolution into the first input.
    UpdateFramebufferParameters(mPostPipelineFbs[0], nativeW, nativeH, 1, true, true, false, false);
    {
        FramebufferDX11& dstFb = mFrameBuffers[mPostPipelineFbs[0]];
        TextureData& srcTex = mTextures[mFrameBuffers[srcFbId].texture_id];
        mContext->OMSetRenderTargets(1, dstFb.render_target_view.GetAddressOf(), nullptr);

        D3D11_VIEWPORT viewport;
        viewport.TopLeftX = 0.0f;
        viewport.TopLeftY = 0.0f;
        viewport.Width = static_cast<float>(nativeW);
        viewport.Height = static_cast<float>(nativeH);
        viewport.MinDepth = 0.0f;
        viewport.MaxDepth = 1.0f;
        mContext->RSSetViewports(1, &viewport);

        mContext->IASetInputLayout(mPostCopyProgram.input_layout.Get());
        mContext->VSSetShader(mPostCopyProgram.vertex_shader.Get(), nullptr, 0);
        mContext->PSSetShader(mPostCopyProgram.pixel_shader.Get(), nullptr, 0);
        mContext->PSSetShaderResources(0, 1, srcTex.resource_view.GetAddressOf());
        mContext->PSSetSamplers(0, 1, mPostCopyProgram.sampler_state.GetAddressOf());
        mContext->PSSetConstantBuffers(0, 0, nullptr);
        mContext->OMSetBlendState(mPostCopyProgram.blend_state.Get(), nullptr, 0xFFFFFFFF);
        mContext->OMSetDepthStencilState(nullptr, 0);
        mContext->RSSetState(mPostCopyProgram.rasterizer_state.Get());
        mContext->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        mContext->Draw(3, 0);
    }

    // Snapshot the current source frame into the history ring before the passes run.
    if (historySize > 0) {
        const int historyFb = mPostPipelineHistoryFbs[mPostPipelineHistoryIndex % historySize];
        UpdateFramebufferParameters(historyFb, nativeW, nativeH, 1, true, true, false, false);
        TextureData& srcTex = mTextures[mFrameBuffers[mPostPipelineFbs[0]].texture_id];
        TextureData& dstTex = mTextures[mFrameBuffers[historyFb].texture_id];
        mContext->CopyResource(dstTex.texture.Get(), srcTex.texture.Get());
        ++mPostPipelineHistoryIndex;
    }

    // Resolve effective parameter values (CVar overrides .slangp override overrides default).
    // The menu sliders write Float CVars, so this must read Float — CVarGetString would
    // return "" for them and the override would never apply.
    const std::string presetStem = GdxPostShaderCvarStem(
        std::filesystem::path(Ship::Context::GetAppDirectoryPath()) / "shaders", pipeline.presetPath);
    std::unordered_map<std::string, float> parameterOverrides = pipeline.parameterOverrides;
    for (const PostShaderProgramD3D11* prg : programs) {
        for (const GdxSlangParameter& p : prg->slangParameters) {
            const std::string cvarName = "gEnhancements.Graphics.PipelineParam." + presetStem + "." + p.name;
            if (CVarGet(cvarName.c_str()) != nullptr) {
                parameterOverrides[p.name] = CVarGetFloat(cvarName.c_str(), p.defaultValue);
            }
        }
    }

    // Step 2: run each pass through the chain. Every pass reads from the previous output and writes
    // to its own output framebuffer; the last pass targets the final output size.
    // Output sizes are deterministic from the scale chain, so precompute them all:
    // PassOutputNSize / `<Alias>Size` can reference a pass that runs later.
    std::vector<std::pair<uint32_t, uint32_t>> passOutSizes(pipeline.passes.size());
    {
        uint32_t chainW = nativeW, chainH = nativeH;
        for (size_t i = 0; i < pipeline.passes.size(); ++i) {
            const GdxPostPassDesc& pass = pipeline.passes[i];
            const bool isLast = (i + 1 == pipeline.passes.size());
            const uint32_t dw = isLast ? outW : GdxResolvePostPassScale(pass.scaleTypeX, pass.scaleX, chainW, outW);
            const uint32_t dh = isLast ? outH : GdxResolvePostPassScale(pass.scaleTypeY, pass.scaleY, chainH, outH);
            passOutSizes[i] = { dw, dh };
            chainW = dw;
            chainH = dh;
        }
    }

    uint32_t srcW = nativeW;
    uint32_t srcH = nativeH;
    int srcFb = mPostPipelineFbs[0];

    for (size_t i = 0; i < pipeline.passes.size(); ++i) {
        const GdxPostPassDesc& pass = pipeline.passes[i];
        const uint32_t dstW = passOutSizes[i].first;
        const uint32_t dstH = passOutSizes[i].second;
        const int dstFb = (int)mPostPipelineFbs[i + 1];

        const bool shouldSkip = pass.frameCountMod != 0 && (mFrameCount % pass.frameCountMod) != 0;

        if (!shouldSkip) {
            UpdateFramebufferParameters(dstFb, dstW, dstH, 1, true, true, false, false,
                                        GdxPipelineFramebufferFormat(pass));
        }

        if (!shouldSkip) {
            FramebufferDX11& srcFbData = mFrameBuffers[srcFb];
            FramebufferDX11& dstFbData = mFrameBuffers[dstFb];
            TextureData& srcTex = mTextures[srcFbData.texture_id];
            PostShaderProgramD3D11* prg = programs[i];

            // Retarget output before binding SRVs: the source framebuffer's RTV is still bound
            // from the previous pass, and D3D11 forces a conflicting SRV to NULL.
            mContext->OMSetRenderTargets(1, dstFbData.render_target_view.GetAddressOf(), nullptr);

            // Bind primary source at slot 0.
            mContext->PSSetShaderResources(0, 1, srcTex.resource_view.GetAddressOf());
            mContext->PSSetSamplers(0, 1, prg->sampler_state.GetAddressOf());

            ID3D11ShaderResourceView* nullSrv = nullptr;
            ID3D11SamplerState* nullSampler = nullptr;

            // OriginalHistoryN: previous N source frames, oldest first.
            for (int n = 0; n <= prg->slangUsedBuiltins.maxOriginalHistory; ++n) {
                const size_t idx = (mPostPipelineHistoryIndex - 1 - n + historySize) % historySize;
                const int histFb = mPostPipelineHistoryFbs[idx];
                TextureData& histTex = mTextures[mFrameBuffers[histFb].texture_id];
                const int slot = GdxSlangTextureBindings::OriginalHistory(n);
                mContext->PSSetShaderResources(slot, 1, histTex.resource_view.GetAddressOf());
                mContext->PSSetSamplers(slot, 1, mPostPipelineClampSampler.GetAddressOf());
            }

            // PassOutputN: same-frame output of pass N.
            for (int n : prg->slangUsedBuiltins.passOutputIndices) {
                if (n >= 0 && n < (int)mPostPipelineFbs.size() - 1) {
                    const int outFb = mPostPipelineFbs[n + 1];
                    TextureData& outTex = mTextures[mFrameBuffers[outFb].texture_id];
                    const int slot = GdxSlangTextureBindings::PassOutput(n);
                    mContext->PSSetShaderResources(slot, 1, outTex.resource_view.GetAddressOf());
                    mContext->PSSetSamplers(slot, 1, mPostPipelineClampSampler.GetAddressOf());
                }
            }

            // PassFeedbackN: previous frame's output of pass N.
            for (int n : prg->slangUsedBuiltins.passFeedbackIndices) {
                if (n >= 0 && n < (int)mPostPipelineFeedbackFbs.size() && mPostPipelineFeedbackFbs[n] >= 0) {
                    const int fbFb = mPostPipelineFeedbackFbs[n];
                    TextureData& fbTex = mTextures[mFrameBuffers[fbFb].texture_id];
                    const int slot = GdxSlangTextureBindings::PassFeedback(n);
                    mContext->PSSetShaderResources(slot, 1, fbTex.resource_view.GetAddressOf());
                    mContext->PSSetSamplers(slot, 1, mPostPipelineClampSampler.GetAddressOf());
                }
            }

            // UserN: LUT textures declared in the preset.
            for (int n : prg->slangUsedBuiltins.userTextureIndices) {
                if (n >= 0 && n < (int)mPostPipelineLuts.size()) {
                    const PostPipelineLutD3D11& lut = mPostPipelineLuts[n];
                    const int slot = GdxSlangTextureBindings::User(n);
                    if (lut.srv.Get() != nullptr) {
                        mContext->PSSetShaderResources(slot, 1, lut.srv.GetAddressOf());
                    } else {
                        mContext->PSSetShaderResources(slot, 1, &nullSrv);
                    }
                    if (lut.sampler.Get() != nullptr) {
                        mContext->PSSetSamplers(slot, 1, lut.sampler.GetAddressOf());
                    } else {
                        mContext->PSSetSamplers(slot, 1, &nullSampler);
                    }
                }
            }

            // Named samplers: a preset LUT when the name is a `textures` entry,
            // `<Alias>` for that pass's current-frame output, `<Alias>Feedback`
            // for its previous frame.
            for (size_t k = 0; k < prg->slangUsedBuiltins.namedSamplers.size(); ++k) {
                const std::string& samplerName = prg->slangUsedBuiltins.namedSamplers[k];
                const int slot = GdxSlangTextureBindings::Named(static_cast<int>(k));
                ID3D11ShaderResourceView* namedSrv = nullptr;
                ID3D11SamplerState* namedSampler = mPostPipelineClampSampler.Get();
                if (pipeline.textures.find(samplerName) != pipeline.textures.end()) {
                    const auto orderIt =
                        std::find(pipeline.textureOrder.begin(), pipeline.textureOrder.end(), samplerName);
                    const size_t lutIdx = static_cast<size_t>(orderIt - pipeline.textureOrder.begin());
                    if (orderIt != pipeline.textureOrder.end() && lutIdx < mPostPipelineLuts.size()) {
                        namedSrv = mPostPipelineLuts[lutIdx].srv.Get();
                        if (mPostPipelineLuts[lutIdx].sampler.Get() != nullptr) {
                            namedSampler = mPostPipelineLuts[lutIdx].sampler.Get();
                        }
                    }
                } else {
                    std::string alias = samplerName;
                    bool wantsFeedback = false;
                    if (alias.size() > 8 && alias.compare(alias.size() - 8, 8, "Feedback") == 0) {
                        wantsFeedback = true;
                        alias = alias.substr(0, alias.size() - 8);
                    }
                    const int passIdx = GdxFindPipelinePassByAlias(pipeline, alias);
                    if (passIdx >= 0) {
                        int fb = -1;
                        if (wantsFeedback) {
                            if (passIdx < (int)mPostPipelineFeedbackFbs.size()) {
                                fb = mPostPipelineFeedbackFbs[passIdx];
                            }
                        } else if (passIdx + 1 < (int)mPostPipelineFbs.size()) {
                            fb = mPostPipelineFbs[passIdx + 1];
                        }
                        if (fb >= 0) {
                            namedSrv = mTextures[mFrameBuffers[fb].texture_id].resource_view.Get();
                        }
                    }
                }
                mContext->PSSetShaderResources(slot, 1, namedSrv != nullptr ? &namedSrv : &nullSrv);
                mContext->PSSetSamplers(slot, 1, namedSampler != nullptr ? &namedSampler : &nullSampler);
            }

            if (prg->constant_buffer.Get() != nullptr) {
                D3D11_MAPPED_SUBRESOURCE ms;
                if (prg->isSlang) {
                    std::vector<uint8_t> cbData(prg->slangCbSize);
                    GdxSlangHlslCBufferLayout::Pack(prg->slangParameters, parameterOverrides, mFrameCount,
                                                    cbData.data(), prg->slangUsedBuiltins.extraSizeNames.size());
                    // uSrcSize / uOutSize live in the same cbuffer slots as the non-slang path,
                    // as vec4 with the slang convention: xy = size, zw = 1/size.
                    const float srcSize[4] = { static_cast<float>(srcW), static_cast<float>(srcH),
                                               1.0f / static_cast<float>(srcW), 1.0f / static_cast<float>(srcH) };
                    const float dstSize[4] = { static_cast<float>(dstW), static_cast<float>(dstH),
                                               1.0f / static_cast<float>(dstW), 1.0f / static_cast<float>(dstH) };
                    std::memcpy(cbData.data(), srcSize, sizeof(srcSize));
                    std::memcpy(cbData.data() + 16, dstSize, sizeof(dstSize));
                    // `<Name>Size` vec4s in sorted order, matching the cbuffer the
                    // translator emitted. History and unrecognized names fall back
                    // to the native source size.
                    static const std::regex passOutSizeRe(R"(^Pass(?:Output|Feedback)(\d+)Size$)");
                    static const std::regex userSizeRe(R"(^User(\d+)Size$)");
                    size_t sizeIdx = 0;
                    for (const std::string& sizeName : prg->slangUsedBuiltins.extraSizeNames) {
                        uint32_t sizeW = nativeW, sizeH = nativeH;
                        std::smatch sizeMatch;
                        if (std::regex_match(sizeName, sizeMatch, passOutSizeRe)) {
                            const int n = std::atoi(sizeMatch[1].str().c_str());
                            if (n >= 0 && n < (int)passOutSizes.size()) {
                                sizeW = passOutSizes[n].first;
                                sizeH = passOutSizes[n].second;
                            }
                        } else if (std::regex_match(sizeName, sizeMatch, userSizeRe)) {
                            const int n = std::atoi(sizeMatch[1].str().c_str());
                            if (n >= 0 && n < (int)mPostPipelineLuts.size() && mPostPipelineLuts[n].width > 0) {
                                sizeW = mPostPipelineLuts[n].width;
                                sizeH = mPostPipelineLuts[n].height;
                            }
                        } else if (sizeName.size() > 4) {
                            const int passIdx =
                                GdxFindPipelinePassByAlias(pipeline, sizeName.substr(0, sizeName.size() - 4));
                            if (passIdx >= 0) {
                                sizeW = passOutSizes[passIdx].first;
                                sizeH = passOutSizes[passIdx].second;
                            }
                        }
                        const float sizeVec[4] = { static_cast<float>(sizeW), static_cast<float>(sizeH),
                                                   1.0f / static_cast<float>(sizeW),
                                                   1.0f / static_cast<float>(sizeH) };
                        std::memcpy(cbData.data() + GdxSlangHlslCBufferLayout::ExtraSizeOffset(sizeIdx), sizeVec,
                                    sizeof(sizeVec));
                        ++sizeIdx;
                    }
                    if (SUCCEEDED(mContext->Map(prg->constant_buffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) {
                        std::memcpy(ms.pData, cbData.data(), prg->slangCbSize);
                        mContext->Unmap(prg->constant_buffer.Get(), 0);
                    }
                } else {
                    float cbData[4] = { static_cast<float>(srcW), static_cast<float>(srcH),
                                        static_cast<float>(dstW), static_cast<float>(dstH) };
                    if (SUCCEEDED(mContext->Map(prg->constant_buffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) {
                        memcpy(ms.pData, cbData, sizeof(cbData));
                        mContext->Unmap(prg->constant_buffer.Get(), 0);
                    }
                }
                ID3D11Buffer* cb = prg->constant_buffer.Get();
                mContext->PSSetConstantBuffers(0, 1, &cb);
                // Translated slang vertex stages read the same cbuffer (MVP, sizes,
                // parameters); the main renderer never uses VS slot 0, so leaving it
                // bound after the chain is harmless.
                mContext->VSSetConstantBuffers(0, 1, &cb);
            }

            D3D11_VIEWPORT viewport;
            viewport.TopLeftX = 0.0f;
            viewport.TopLeftY = 0.0f;
            viewport.Width = static_cast<float>(dstW);
            viewport.Height = static_cast<float>(dstH);
            viewport.MinDepth = 0.0f;
            viewport.MaxDepth = 1.0f;
            mContext->RSSetViewports(1, &viewport);

            mContext->IASetInputLayout(prg->input_layout.Get());
            mContext->VSSetShader(prg->vertex_shader.Get(), nullptr, 0);
            mContext->PSSetShader(prg->pixel_shader.Get(), nullptr, 0);
            mContext->OMSetBlendState(prg->blend_state.Get(), nullptr, 0xFFFFFFFF);
            mContext->OMSetDepthStencilState(nullptr, 0);
            mContext->RSSetState(prg->rasterizer_state.Get());
            mContext->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            mContext->Draw(3, 0);
        }

        srcFb = dstFb;
        srcW = dstW;
        srcH = dstH;

        // If this pass feeds back, copy its output into the feedback framebuffer
        // so PassFeedbackN / `<Alias>Feedback` samples last frame's result next
        // frame. The framebuffer's existence is the trigger: it is allocated for
        // every pass anything samples feedback from, flag or not.
        if (i < mPostPipelineFeedbackFbs.size() && mPostPipelineFeedbackFbs[i] >= 0) {
            const int fbFb = mPostPipelineFeedbackFbs[i];
            UpdateFramebufferParameters(fbFb, dstW, dstH, 1, true, true, false, false,
                                        GdxPipelineFramebufferFormat(pass));
            TextureData& dstTex = mTextures[mFrameBuffers[dstFb].texture_id];
            TextureData& fbTex = mTextures[mFrameBuffers[fbFb].texture_id];
            mContext->CopyResource(fbTex.texture.Get(), dstTex.texture.Get());
        }
    }

#if DEBUG_D3D_CHAIN_INFOQUEUE
    {
        ComPtr<ID3D11InfoQueue> infoQueue;
        if (SUCCEEDED(mDevice->QueryInterface(IID_PPV_ARGS(infoQueue.GetAddressOf())))) {
            const UINT64 messageCount = infoQueue->GetNumStoredMessages();
            for (UINT64 m = 0; m < messageCount; ++m) {
                SIZE_T length = 0;
                infoQueue->GetMessage(m, nullptr, &length);
                std::vector<uint8_t> buffer(length);
                D3D11_MESSAGE* message = reinterpret_cast<D3D11_MESSAGE*>(buffer.data());
                if (SUCCEEDED(infoQueue->GetMessage(m, message, &length))) {
                    SPDLOG_ERROR("[d3d11-debug] sev={} id={} {}", (int)message->Severity, (int)message->ID,
                                 message->pDescription ? message->pDescription : "");
                }
            }
            infoQueue->ClearStoredMessages();
        }
    }
#endif

    // Restore device state and invalidate backend caches so the next draw re-binds anything changed.
    mContext->OMSetRenderTargets(1, prevRTV.GetAddressOf(), prevDSV.Get());
    if (numViewports > 0) {
        mContext->RSSetViewports(1, &prevViewport);
    }
    mContext->VSSetShader(prevVS.Get(), nullptr, 0);
    mContext->PSSetShader(prevPS.Get(), nullptr, 0);
    mContext->GSSetShader(prevGS.Get(), nullptr, 0);
    mContext->IASetInputLayout(prevIL.Get());
    mContext->IASetVertexBuffers(0, 1, prevVB.GetAddressOf(), &prevStride, &prevOffset);
    mContext->IASetPrimitiveTopology(prevTopology);
    mContext->OMSetBlendState(prevBlend.Get(), prevBlendFactor, prevSampleMask);
    mContext->OMSetDepthStencilState(prevDepthStencil.Get(), prevStencilRef);
    mContext->RSSetState(prevRasterizer.Get());
    mContext->PSSetShaderResources(0, 1, prevSRV.GetAddressOf());
    mContext->PSSetSamplers(0, 1, prevSampler.GetAddressOf());
    mContext->PSSetConstantBuffers(0, 1, prevCB.GetAddressOf());

    mLastShaderProgram = nullptr;
    mLastVertexBufferStride = 0;
    mLastBlendState.Reset();
    for (int i = 0; i < SHADER_MAX_TEXTURES; i++) {
        mLastResourceViews[i].Reset();
        mLastSamplerStates[i].Reset();
    }
    mLastPrimitaveTopology = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;

    GdxLogPostShaderInfo(fmt::format("pipeline '{}': {} pass(es), {}x{} -> {}x{}", pipeline.presetPath.filename().string(),
                                     pipeline.passes.size(), nativeW, nativeH, outW, outH));

    return reinterpret_cast<uintptr_t>(GetFramebufferTextureId(mPostPipelineFbs[pipeline.passes.size()]));
}

void GfxRenderingAPIDX11::SelectTextureFb(int fbID) {
    int tile = 0;
    SelectTexture(tile, mFrameBuffers[fbID].texture_id);
}

void GfxRenderingAPIDX11::CopyFramebuffer(int fb_dst_id, int fb_src_id, int srcX0, int srcY0, int srcX1, int srcY1,
                                          int dstX0, int dstY0, int dstX1, int dstY1) {
    if (fb_src_id >= (int)mFrameBuffers.size() || fb_dst_id >= (int)mFrameBuffers.size()) {
        return;
    }

    FramebufferDX11& fb_dst = mFrameBuffers[fb_dst_id];
    FramebufferDX11& fb_src = mFrameBuffers[fb_src_id];

    TextureData& td_dst = mTextures[fb_dst.texture_id];
    TextureData& td_src = mTextures[fb_src.texture_id];

    // Textures are the same size so we can do a direct copy or resolve
    if (td_src.height == td_dst.height && td_src.width == td_dst.width) {
        if (fb_src.msaa_level <= 1) {
            mContext->CopyResource(td_dst.texture.Get(), td_src.texture.Get());
        } else {
            mContext->ResolveSubresource(td_dst.texture.Get(), 0, td_src.texture.Get(), 0, DXGI_FORMAT_R8G8B8A8_UNORM);
        }
        return;
    }

    if (srcY1 > (int)td_src.height || srcX1 > (int)td_src.width || srcX0 < 0 || srcY0 < 0 ||
        dstY1 > (int)td_dst.height || dstX1 > (int)td_dst.width || dstX0 < 0 || dstY0 < 0) {
        // Using a source region larger than the source resource or copy outside of the destination resource is
        // considered undefined behavior and could lead to removal of the rendering mDevice
        return;
    }

    D3D11_BOX region;
    region.left = srcX0;
    region.right = srcX1;
    region.top = srcY0;
    region.bottom = srcY1;
    region.front = 0;
    region.back = 1;

    // We can't region copy a multi-sample texture to a single sample texture
    if (fb_src.msaa_level <= 1) {
        mContext->CopySubresourceRegion(td_dst.texture.Get(), dstX0, dstY0, 0, 0, td_src.texture.Get(), 0, &region);
    } else {
        // Setup a temporary texture
        TextureData td_resolved;
        td_resolved.width = td_src.width;
        td_resolved.height = td_src.height;

        D3D11_TEXTURE2D_DESC texture_desc;
        texture_desc.Width = td_src.width;
        texture_desc.Height = td_src.height;
        texture_desc.Usage = D3D11_USAGE_DEFAULT;
        texture_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        texture_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        texture_desc.CPUAccessFlags = 0;
        texture_desc.MiscFlags = 0;
        texture_desc.ArraySize = 1;
        texture_desc.MipLevels = 1;
        texture_desc.SampleDesc.Count = 1;
        texture_desc.SampleDesc.Quality = 0;

        ThrowIfFailed(mDevice->CreateTexture2D(&texture_desc, nullptr, td_resolved.texture.GetAddressOf()));

        // Resolve multi-sample to temporary
        mContext->ResolveSubresource(td_resolved.texture.Get(), 0, td_src.texture.Get(), 0, DXGI_FORMAT_R8G8B8A8_UNORM);
        // Then copy the region to the destination
        mContext->CopySubresourceRegion(td_dst.texture.Get(), dstX0, dstY0, 0, 0, td_resolved.texture.Get(), 0,
                                        &region);
    }
}

void GfxRenderingAPIDX11::ReadFramebufferToCPU(int fb_id, uint32_t width, uint32_t height, uint16_t* rgba16_buf) {
    if (fb_id >= (int)mFrameBuffers.size()) {
        return;
    }

    FramebufferDX11& fb = mFrameBuffers[fb_id];
    TextureData& td = mTextures[fb.texture_id];

    // Query actual texture dimensions — CopyResource requires matching sizes
    D3D11_TEXTURE2D_DESC srcDesc;
    td.texture->GetDesc(&srcDesc);

    // Reuse cached staging texture when dimensions match — avoids per-frame CreateTexture2D
    if (!mReadbackStaging || mReadbackStagingW != srcDesc.Width || mReadbackStagingH != srcDesc.Height) {
        mReadbackStaging.Reset();

        D3D11_TEXTURE2D_DESC texture_desc;
        texture_desc.Width = srcDesc.Width;
        texture_desc.Height = srcDesc.Height;
        texture_desc.Usage = D3D11_USAGE_STAGING;
        texture_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        texture_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        texture_desc.BindFlags = 0;
        texture_desc.MiscFlags = 0;
        texture_desc.ArraySize = 1;
        texture_desc.MipLevels = 1;
        texture_desc.SampleDesc.Count = 1;
        texture_desc.SampleDesc.Quality = 0;

        ThrowIfFailed(mDevice->CreateTexture2D(&texture_desc, nullptr, mReadbackStaging.GetAddressOf()));
        mReadbackStagingW = srcDesc.Width;
        mReadbackStagingH = srcDesc.Height;
    }

    // Copy the framebuffer texture to the staging texture
    mContext->CopyResource(mReadbackStaging.Get(), td.texture.Get());

    // Map the staging texture to a resource that we can read
    D3D11_MAPPED_SUBRESOURCE resource = {};
    ThrowIfFailed(mContext->Map(mReadbackStaging.Get(), 0, D3D11_MAP_READ, 0, &resource));

    if (!resource.pData) {
        mContext->Unmap(mReadbackStaging.Get(), 0);
        return;
    }

    // Box-filter average, not nearest-neighbor. Downscaling 1920x1080 to a 320x240 capture is
    // 6:1 horizontally, and keeping only every 6th column shredded high-frequency title-screen
    // art into the dash band seen during fade transitions. No aspect crop is needed: the
    // transition redraws the capture stretched back across the full viewport (decomp
    // ovl_i2/transition.c, G_EX_WIDESCREEN_STRETCH), so only filter quality matters. Runs once
    // per screen transition, so the extra source reads are free.
    const uint32_t srcW = srcDesc.Width;
    const uint32_t srcH = srcDesc.Height;
    for (uint32_t j = 0; j < height; j++) {
        uint32_t sy0 = j * srcH / height;
        uint32_t sy1 = (j + 1) * srcH / height;
        if (sy1 <= sy0) sy1 = sy0 + 1;
        if (sy1 > srcH) sy1 = srcH;
        for (uint32_t i = 0; i < width; i++) {
            uint32_t sx0 = i * srcW / width;
            uint32_t sx1 = (i + 1) * srcW / width;
            if (sx1 <= sx0) sx1 = sx0 + 1;
            if (sx1 > srcW) sx1 = srcW;

            uint32_t accR = 0, accG = 0, accB = 0, count = 0;
            for (uint32_t sy = sy0; sy < sy1; sy++) {
                const uint32_t* srcRow =
                    (const uint32_t*)((const uint8_t*)resource.pData + (size_t)sy * resource.RowPitch);
                for (uint32_t sx = sx0; sx < sx1; sx++) {
                    const uint32_t pixel = srcRow[sx];
                    accR += pixel & 0xFF;
                    accG += (pixel >> 8) & 0xFF;
                    accB += (pixel >> 16) & 0xFF;
                    ++count;
                }
            }
            const uint32_t avgR = count ? accR / count : 0;
            const uint32_t avgG = count ? accG / count : 0;
            const uint32_t avgB = count ? accB / count : 0;
            // Same 8->5-bit rounding (+4 bias) as the per-pixel path, on the averaged channel.
            uint8_t r = ((avgR + 4) * 0x1F) / 0xFF;
            uint8_t g = ((avgG + 4) * 0x1F) / 0xFF;
            uint8_t b = ((avgB + 4) * 0x1F) / 0xFF;
            // Coverage bit, not host alpha. On N64 the low bit is coverage and a captured full
            // frame is fully covered; the host render target's alpha is whatever the combiner
            // last wrote, usually 0 for opaque geometry, which made every alpha-dependent redraw
            // of the capture discard its texels and draw nothing.
            uint8_t a = 1;

            rgba16_buf[i + (j * width)] = (r << 11) | (g << 6) | (b << 1) | a;
        }
    }

    mContext->Unmap(mReadbackStaging.Get(), 0);
}

void GfxRenderingAPIDX11::SetTextureFilter(FilteringMode mode) {
    mCurrentFilterMode = mode;
    gfx_texture_cache_clear();
}

FilteringMode GfxRenderingAPIDX11::GetTextureFilter() {
    return mCurrentFilterMode;
}

std::unordered_map<std::pair<float, float>, uint16_t, hash_pair_ff>
GfxRenderingAPIDX11::GetPixelDepth(int fb_id, const std::set<std::pair<float, float>>& coordinates) {
    FramebufferDX11& fb = mFrameBuffers[fb_id];
    TextureData& td = mTextures[fb.texture_id];

    if (coordinates.size() > mCoordBufferSize) {
        mCoordBuffer.Reset();
        mCoordBufferSrv.Reset();
        mDepthValueOutputBuffer.Reset();
        mDepthValueOutputUav.Reset();
        mDepthValueOutputBufferCopy.Reset();

        D3D11_BUFFER_DESC coord_buf_desc;
        coord_buf_desc.Usage = D3D11_USAGE_DYNAMIC;
        coord_buf_desc.ByteWidth = sizeof(Coord) * coordinates.size();
        coord_buf_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        coord_buf_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        coord_buf_desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        coord_buf_desc.StructureByteStride = sizeof(Coord);

        ThrowIfFailed(mDevice->CreateBuffer(&coord_buf_desc, nullptr, mCoordBuffer.GetAddressOf()));

        D3D11_SHADER_RESOURCE_VIEW_DESC coord_buf_srv_desc;
        coord_buf_srv_desc.Format = DXGI_FORMAT_UNKNOWN;
        coord_buf_srv_desc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
        coord_buf_srv_desc.Buffer.FirstElement = 0;
        coord_buf_srv_desc.Buffer.NumElements = coordinates.size();

        ThrowIfFailed(
            mDevice->CreateShaderResourceView(mCoordBuffer.Get(), &coord_buf_srv_desc, mCoordBufferSrv.GetAddressOf()));

        D3D11_BUFFER_DESC output_buffer_desc;
        output_buffer_desc.Usage = D3D11_USAGE_DEFAULT;
        output_buffer_desc.ByteWidth = sizeof(float) * coordinates.size();
        output_buffer_desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        output_buffer_desc.CPUAccessFlags = 0;
        output_buffer_desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        output_buffer_desc.StructureByteStride = sizeof(float);
        ThrowIfFailed(mDevice->CreateBuffer(&output_buffer_desc, nullptr, mDepthValueOutputBuffer.GetAddressOf()));

        D3D11_UNORDERED_ACCESS_VIEW_DESC output_buffer_uav_desc;
        output_buffer_uav_desc.Format = DXGI_FORMAT_UNKNOWN;
        output_buffer_uav_desc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        output_buffer_uav_desc.Buffer.FirstElement = 0;
        output_buffer_uav_desc.Buffer.NumElements = coordinates.size();
        output_buffer_uav_desc.Buffer.Flags = 0;
        ThrowIfFailed(mDevice->CreateUnorderedAccessView(mDepthValueOutputBuffer.Get(), &output_buffer_uav_desc,
                                                         mDepthValueOutputUav.GetAddressOf()));

        output_buffer_desc.Usage = D3D11_USAGE_STAGING;
        output_buffer_desc.BindFlags = 0;
        output_buffer_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ThrowIfFailed(mDevice->CreateBuffer(&output_buffer_desc, nullptr, mDepthValueOutputBufferCopy.GetAddressOf()));

        mCoordBufferSize = coordinates.size();
    }

    D3D11_MAPPED_SUBRESOURCE ms;

    if (fb.msaa_level > 1 && mComputeShaderMsaa.Get() == nullptr) {
        ThrowIfFailed(mDevice->CreateComputeShader(mComputeShaderMsaaBlob->GetBufferPointer(),
                                                   mComputeShaderMsaaBlob->GetBufferSize(), nullptr,
                                                   mComputeShaderMsaa.GetAddressOf()));
    }

    // ImGui overwrites these values, so we cannot set them once at init
    mContext->CSSetShader(fb.msaa_level > 1 ? mComputeShaderMsaa.Get() : mComputeShader.Get(), nullptr, 0);
    mContext->CSSetUnorderedAccessViews(0, 1, mDepthValueOutputUav.GetAddressOf(), nullptr);

    ThrowIfFailed(mContext->Map(mCoordBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &ms));
    Coord* coord_cb = (Coord*)ms.pData;
    {
        size_t i = 0;
        for (const auto& coord : coordinates) {
            coord_cb[i].x = coord.first;
            // We invert y because the gfx_pc assumes OpenGL coordinates (bottom-left corner is origin), while DX's
            // origin is top-left corner
            coord_cb[i].y = td.height - 1 - coord.second;
            ++i;
        }
    }
    mContext->Unmap(mCoordBuffer.Get(), 0);

    // The depth stencil texture can only have one mapping at a time, so unbind from the OM
    ID3D11RenderTargetView* null_arr1[1] = { nullptr };
    mContext->OMSetRenderTargets(1, null_arr1, nullptr);

    ID3D11ShaderResourceView* srvs[2] = { fb.depth_stencil_srv.Get(), mCoordBufferSrv.Get() };
    mContext->CSSetShaderResources(0, 2, srvs);

    mContext->Dispatch(coordinates.size(), 1, 1);

    mContext->CopyResource(mDepthValueOutputBufferCopy.Get(), mDepthValueOutputBuffer.Get());
    ThrowIfFailed(mContext->Map(mDepthValueOutputBufferCopy.Get(), 0, D3D11_MAP_READ, 0, &ms));
    std::unordered_map<std::pair<float, float>, uint16_t, hash_pair_ff> res;
    {
        size_t i = 0;
        for (const auto& coord : coordinates) {
            res.emplace(coord, ((float*)ms.pData)[i++] * 65532.0f);
        }
    }
    mContext->Unmap(mDepthValueOutputBufferCopy.Get(), 0);

    ID3D11ShaderResourceView* null_arr[2] = { nullptr, nullptr };
    mContext->CSSetShaderResources(0, 2, null_arr);

    return res;
}

ImTextureID GfxRenderingAPIDX11::GetTextureById(int id) {
    return mTextures[id].resource_view.Get();
}

void GfxRenderingAPIDX11::SetSrgbMode() {
    mSrgbMode = true;
}

#define RAND_NOISE "((random(float3(floor(screenSpace.xy * noise_scale), noise_frame)) + 1.0) / 2.0)"

static const char* prism_shader_item_to_str(uint32_t item, bool with_alpha, bool only_alpha, bool inputs_have_alpha,
                                            bool first_cycle, bool hint_single_element) {
    if (!only_alpha) {
        switch (item) {
            default:
            case SHADER_0:
                return with_alpha ? "float4(0.0, 0.0, 0.0, 0.0)" : "float3(0.0, 0.0, 0.0)";
            case SHADER_1:
                return with_alpha ? "float4(1.0, 1.0, 1.0, 1.0)" : "float3(1.0, 1.0, 1.0)";
            case SHADER_INPUT_1:
                return with_alpha || !inputs_have_alpha ? "input.input1" : "input.input1.rgb";
            case SHADER_INPUT_2:
                return with_alpha || !inputs_have_alpha ? "input.input2" : "input.input2.rgb";
            case SHADER_INPUT_3:
                return with_alpha || !inputs_have_alpha ? "input.input3" : "input.input3.rgb";
            case SHADER_INPUT_4:
                return with_alpha || !inputs_have_alpha ? "input.input4" : "input.input4.rgb";
            case SHADER_TEXEL0:
                return first_cycle ? (with_alpha ? "texVal0" : "texVal0.rgb")
                                   : (with_alpha ? "texVal1" : "texVal1.rgb");
            case SHADER_TEXEL0A:
                return first_cycle
                           ? (hint_single_element ? "texVal0.a"
                                                  : (with_alpha ? "float4(texVal0.a, texVal0.a, texVal0.a, texVal0.a)"
                                                                : "float3(texVal0.a, texVal0.a, texVal0.a)"))
                           : (hint_single_element ? "texVal1.a"
                                                  : (with_alpha ? "float4(texVal1.a, texVal1.a, texVal1.a, texVal1.a)"
                                                                : "float3(texVal1.a, texVal1.a, texVal1.a)"));
            case SHADER_TEXEL1A:
                return first_cycle
                           ? (hint_single_element ? "texVal1.a"
                                                  : (with_alpha ? "float4(texVal1.a, texVal1.a, texVal1.a, texVal1.a)"
                                                                : "float3(texVal1.a, texVal1.a, texVal1.a)"))
                           : (hint_single_element ? "texVal0.a"
                                                  : (with_alpha ? "float4(texVal0.a, texVal0.a, texVal0.a, texVal0.a)"
                                                                : "float3(texVal0.a, texVal0.a, texVal0.a)"));
            case SHADER_TEXEL1:
                return first_cycle ? (with_alpha ? "texVal1" : "texVal1.rgb")
                                   : (with_alpha ? "texVal0" : "texVal0.rgb");
            case SHADER_COMBINED:
                return with_alpha ? "texel" : "texel.rgb";
            case SHADER_NOISE:
                return with_alpha ? "float4(" RAND_NOISE ", " RAND_NOISE ", " RAND_NOISE ", " RAND_NOISE ")"
                                  : "float3(" RAND_NOISE ", " RAND_NOISE ", " RAND_NOISE ")";
        }
    } else {
        switch (item) {
            default:
            case SHADER_0:
                return "0.0";
            case SHADER_1:
                return "1.0";
            case SHADER_INPUT_1:
                return "input.input1.a";
            case SHADER_INPUT_2:
                return "input.input2.a";
            case SHADER_INPUT_3:
                return "input.input3.a";
            case SHADER_INPUT_4:
                return "input.input4.a";
            case SHADER_TEXEL0:
                return first_cycle ? "texVal0.a" : "texVal1.a";
            case SHADER_TEXEL0A:
                return first_cycle ? "texVal0.a" : "texVal1.a";
            case SHADER_TEXEL1A:
                return first_cycle ? "texVal1.a" : "texVal0.a";
            case SHADER_TEXEL1:
                return first_cycle ? "texVal1.a" : "texVal0.a";
            case SHADER_COMBINED:
                return "texel.a";
            case SHADER_NOISE:
                return RAND_NOISE;
        }
    }
}

bool prism_get_bool(prism::ContextTypes* value) {
    if (std::holds_alternative<int>(*value)) {
        return std::get<int>(*value) == 1;
    }
    return false;
}

#undef RAND_NOISE

prism::ContextTypes* prism_append_formula(prism::ContextTypes* _, prism::ContextTypes* a_arg,
                                          prism::ContextTypes* a_single, prism::ContextTypes* a_mult,
                                          prism::ContextTypes* a_mix, prism::ContextTypes* a_with_alpha,
                                          prism::ContextTypes* a_only_alpha, prism::ContextTypes* a_alpha,
                                          prism::ContextTypes* a_first_cycle) {
    auto c = std::get<prism::MTDArray<int>>(*a_arg);
    bool do_single = prism_get_bool(a_single);
    bool do_multiply = prism_get_bool(a_mult);
    bool do_mix = prism_get_bool(a_mix);
    bool with_alpha = prism_get_bool(a_with_alpha);
    bool only_alpha = prism_get_bool(a_only_alpha);
    bool opt_alpha = prism_get_bool(a_alpha);
    bool first_cycle = prism_get_bool(a_first_cycle);
    std::string out = "";
    if (do_single) {
        out += prism_shader_item_to_str(c.at(only_alpha, 3), with_alpha, only_alpha, opt_alpha, first_cycle, false);
    } else if (do_multiply) {
        out += prism_shader_item_to_str(c.at(only_alpha, 0), with_alpha, only_alpha, opt_alpha, first_cycle, false);
        out += " * ";
        out += prism_shader_item_to_str(c.at(only_alpha, 2), with_alpha, only_alpha, opt_alpha, first_cycle, true);
    } else if (do_mix) {
        out += "lerp(";
        out += prism_shader_item_to_str(c.at(only_alpha, 1), with_alpha, only_alpha, opt_alpha, first_cycle, false);
        out += ", ";
        out += prism_shader_item_to_str(c.at(only_alpha, 0), with_alpha, only_alpha, opt_alpha, first_cycle, false);
        out += ", ";
        out += prism_shader_item_to_str(c.at(only_alpha, 2), with_alpha, only_alpha, opt_alpha, first_cycle, true);
        out += ")";
    } else {
        out += "(";
        out += prism_shader_item_to_str(c.at(only_alpha, 0), with_alpha, only_alpha, opt_alpha, first_cycle, false);
        out += " - ";
        out += prism_shader_item_to_str(c.at(only_alpha, 1), with_alpha, only_alpha, opt_alpha, first_cycle, false);
        out += ") * ";
        out += prism_shader_item_to_str(c.at(only_alpha, 2), with_alpha, only_alpha, opt_alpha, first_cycle, true);
        out += " + ";
        out += prism_shader_item_to_str(c.at(only_alpha, 3), with_alpha, only_alpha, opt_alpha, first_cycle, false);
    }
    return new prism::ContextTypes{ out };
}

static size_t raw_numFloats = 0;

prism::ContextTypes* update_raw_floats(prism::ContextTypes* _, prism::ContextTypes* num) {
    raw_numFloats += std::get<int>(*num);
    return nullptr;
}

std::optional<std::string> dx_include_fs(const std::string& path) {
    auto init = std::make_shared<Ship::ResourceInitData>();
    init->Type = (uint32_t)Ship::ResourceType::Shader;
    init->ByteOrder = Ship::Endianness::Native;
    init->Format = RESOURCE_FORMAT_BINARY;
    auto res = static_pointer_cast<Ship::Shader>(
        Ship::Context::GetInstance()->GetResourceManager()->LoadResource(path, true, init));
    if (res == nullptr) {
        return std::nullopt;
    }

    auto inc = static_cast<std::string*>(res->GetRawPointer());
    return *inc;
}

std::string gfx_direct3d_common_build_shader(size_t& numFloats, const CCFeatures& cc_features,
                                             bool include_root_signature, bool three_point_filtering, bool use_srgb) {
    raw_numFloats = 4;

    prism::Processor processor;
    prism::ContextItems mContext = {
        { "SHADER_0", SHADER_0 },
        { "SHADER_INPUT_1", SHADER_INPUT_1 },
        { "SHADER_INPUT_2", SHADER_INPUT_2 },
        { "SHADER_INPUT_3", SHADER_INPUT_3 },
        { "SHADER_INPUT_4", SHADER_INPUT_4 },
        { "SHADER_INPUT_5", SHADER_INPUT_5 },
        { "SHADER_INPUT_6", SHADER_INPUT_6 },
        { "SHADER_INPUT_7", SHADER_INPUT_7 },
        { "SHADER_TEXEL0", SHADER_TEXEL0 },
        { "SHADER_TEXEL0A", SHADER_TEXEL0A },
        { "SHADER_TEXEL1", SHADER_TEXEL1 },
        { "SHADER_TEXEL1A", SHADER_TEXEL1A },
        { "SHADER_1", SHADER_1 },
        { "SHADER_COMBINED", SHADER_COMBINED },
        { "SHADER_NOISE", SHADER_NOISE },
        { "o_c", M_ARRAY(cc_features.c, int, 2, 2, 4) },
        { "o_alpha", cc_features.opt_alpha },
        { "o_fog", cc_features.opt_fog },
        { "o_texture_edge", cc_features.opt_texture_edge },
        { "o_noise", cc_features.opt_noise },
        { "o_2cyc", cc_features.opt_2cyc },
        { "o_alpha_threshold", cc_features.opt_alpha_threshold },
        { "o_invisible", cc_features.opt_invisible },
        { "o_grayscale", cc_features.opt_grayscale },
        { "o_prim_depth", cc_features.opt_prim_depth },
        { "o_textures", M_ARRAY(cc_features.usedTextures, bool, 2) },
        { "o_masks", M_ARRAY(cc_features.used_masks, bool, 2) },
        { "o_blend", M_ARRAY(cc_features.used_blend, bool, 2) },
        { "o_clamp", M_ARRAY(cc_features.clamp, bool, 2, 2) },
        { "o_inputs", cc_features.numInputs },
        { "o_do_mix", M_ARRAY(cc_features.do_mix, bool, 2, 2) },
        { "o_do_single", M_ARRAY(cc_features.do_single, bool, 2, 2) },
        { "o_do_multiply", M_ARRAY(cc_features.do_multiply, bool, 2, 2) },
        { "o_color_alpha_same", M_ARRAY(cc_features.color_alpha_same, bool, 2) },
        { "o_root_signature", include_root_signature },
        { "o_three_point_filtering", three_point_filtering },
        { "srgb_mode", use_srgb },
        { "append_formula", (InvokeFunc)prism_append_formula },
        { "update_floats", (InvokeFunc)update_raw_floats },
    };
    processor.populate(mContext);
    auto init = std::make_shared<Ship::ResourceInitData>();
    init->Type = (uint32_t)Ship::ResourceType::Shader;
    init->ByteOrder = Ship::Endianness::Native;
    init->Format = RESOURCE_FORMAT_BINARY;
    const char* shaderName = Fast::gfx_get_shader(cc_features.shader_id);
    std::string path = "shaders/directx/default.shader.hlsl";

    if (nullptr != shaderName) {
        path = std::string(shaderName) + ".hlsl";
    }

    auto res = static_pointer_cast<Ship::Shader>(Ship::Context::GetInstance()->GetResourceManager()->LoadResource(
        "shaders/directx/default.shader.hlsl", true, init));

    if (res == nullptr) {
        SPDLOG_ERROR("Failed to load default directx shader, missing gdiffuser.o2r?");
        abort();
    }

    auto shader = static_cast<std::string*>(res->GetRawPointer());
    processor.load(*shader);
    processor.bind_include_loader(dx_include_fs);
    auto result = processor.process();
    numFloats = raw_numFloats;
    // SPDLOG_INFO("=========== DX11 SHADER ============");
    // SPDLOG_INFO(result);
    // SPDLOG_INFO("====================================");
    return result;
}
} // namespace Fast
#endif
