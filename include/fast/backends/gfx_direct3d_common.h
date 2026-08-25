#pragma once

#if defined(ENABLE_DX11) || defined(ENABLE_DX12)

#ifdef __cplusplus
#include "../interpreter.h"
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include "gfx_rendering_api.h"
#include "fast/backends/gfx_post_shader_pipeline.h"
#include "fast/backends/gfx_slang_translator.h"
#include "gfx_shader_cache.h"
#include "d3d11.h"
#include "d3dcompiler.h"

namespace Fast {

struct PerFrameCB {
    uint32_t noise_frame;
    float noise_scale;
    uint32_t padding[2]; // constant buffers must be multiples of 16 bytes in size
};

struct PerDrawCB {
    struct Texture {
        uint32_t width;
        uint32_t height;
        uint32_t linear_filtering;
        uint32_t padding;
    } mTextures[SHADER_MAX_TEXTURES];
};

struct PerPrimDepthCB {
    float prim_depth;
    float _pad[3]; // 16-byte CB alignment
};

struct PerAlphaThresholdCB {
    float alpha_compare_threshold;
    float _pad[3]; // 16-byte CB alignment
};

struct Coord {
    int x, y;
};

struct TextureData {
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> resource_view;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler_state;
    uint32_t width;
    uint32_t height;
    bool linear_filtering;
    DXGI_FORMAT lastFormat = DXGI_FORMAT_UNKNOWN;
};

struct FramebufferDX11 {
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> render_target_view;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilView> depth_stencil_view;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> depth_stencil_srv;
    uint32_t texture_id;
    bool has_depth_buffer;
    uint32_t msaa_level;
};

struct ShaderProgramD3D11 {
    Microsoft::WRL::ComPtr<ID3D11VertexShader> vertex_shader;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> pixel_shader;
    Microsoft::WRL::ComPtr<ID3D11InputLayout> input_layout;
    Microsoft::WRL::ComPtr<ID3D11BlendState> blend_state;

    uint64_t shader_id0;
    uint64_t shader_id1;
    uint8_t numInputs;
    uint8_t numFloats;
    bool usedTextures[SHADER_MAX_TEXTURES];
};

// Async post-pipeline build (gfx_direct3d11.cpp). Selecting or hot-reloading a preset compiles
// its passes on a worker thread — file IO, slang translation, and D3DCompile to DXBC are all
// free-threaded — so the frame that applies the pick no longer stalls. The render thread only
// creates device objects from the finished blobs. While a build runs, ApplyPostShaderChain
// declines and the interpreter keeps publishing the unfiltered frame.
struct GdxPostPassBuildD3D11 {
    std::string cacheKey;
    uint64_t mtime = 0;
    bool ok = false;
    bool isSlang = false;
    std::vector<GdxSlangParameter> slangParameters;
    uint32_t slangCbSize = 0;
    GdxSlangUsedBuiltins slangUsedBuiltins;
    Microsoft::WRL::ComPtr<ID3DBlob> vsBlob, psBlob;
    D3D11_FILTER samplerFilter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    D3D11_TEXTURE_ADDRESS_MODE wrapU = D3D11_TEXTURE_ADDRESS_CLAMP;
    D3D11_TEXTURE_ADDRESS_MODE wrapV = D3D11_TEXTURE_ADDRESS_CLAMP;
};

struct GdxPostPipelineBuildD3D11 {
    std::string key;
    std::vector<GdxPostPassBuildD3D11> passes;
    std::atomic<int> state{ 0 }; // 0 running, 1 ready to install, 2 failed
    std::atomic<bool> cancel{ false };
};

class GfxWindowBackendDXGI;

class GfxRenderingAPIDX11 final : public GfxRenderingAPI {
  public:
    GfxRenderingAPIDX11() = default;
    ~GfxRenderingAPIDX11() override;
    GfxRenderingAPIDX11(GfxWindowBackendDXGI* backend);
    const char* GetName() override;
    int GetMaxTextureSize() override;
    GfxClipParameters GetClipParameters() override;
    void UnloadShader(struct ShaderProgram* oldPrg) override;
    void LoadShader(struct ShaderProgram* newPrg) override;
    struct ShaderProgram* CreateAndLoadNewShader(uint64_t shaderId0, uint64_t shaderId1) override;
    struct ShaderProgram* LookupShader(uint64_t shaderId0, uint64_t shaderId1) override;
    void ShaderGetInfo(struct ShaderProgram* prg, uint8_t* numInputs, bool usedTextures[2]) override;
    void ClearShaderCache() override;
    uint32_t NewTexture() override;
    void SelectTexture(int tile, uint32_t textureId) override;
    void UploadTexture(const uint8_t* rgba32Buf, uint32_t width, uint32_t height) override;
    void SetSamplerParameters(int sampler, bool linear_filter, uint32_t cms, uint32_t cmt) override;
    void SetDepthTestAndMask(bool depth_test, bool z_upd) override;
    void SetCurrentPrimDepth(float depth) override;
    void SetCurrentAlphaCompareThreshold(float threshold) override;
    void SetZmodeDecal(bool decal) override;
    void SetViewport(int x, int y, int width, int height) override;
    void SetScissor(int x, int y, int width, int height) override;
    void SetUseAlpha(bool useAlpha) override;
    void DrawTriangles(float buf_vbo[], size_t buf_vbo_len, size_t buf_vbo_num_tris) override;
    void Init() override;
    void OnResize() override;
    void StartFrame() override;
    void EndFrame() override;
    void FinishRender() override;
    int CreateFramebuffer() override;
    void UpdateFramebufferParameters(int fb_id, uint32_t width, uint32_t height, uint32_t msaa_level,
                                     bool opengl_invertY, bool render_target, bool has_depth_buffer,
                                     bool can_extract_depth,
                                     GdxFramebufferFormat format = GdxFramebufferFormat::R8G8B8A8_UNORM) override;
    void StartDrawToFramebuffer(int fbId, float noiseScale) override;
    void CopyFramebuffer(int fbDstId, int fbSrcId, int srcX0, int srcY0, int srcX1, int srcY1, int dstX0, int dstY0,
                         int dstX1, int dstY1) override;
    void ClearFramebuffer(bool color, bool depth) override;
    void ReadFramebufferToCPU(int fbId, uint32_t width, uint32_t height, uint16_t* rgba16Buf) override;
    void ResolveMSAAColorBuffer(int fbIdTarger, int fbIdSrc) override;
    std::unordered_map<std::pair<float, float>, uint16_t, hash_pair_ff>
    GetPixelDepth(int fb_id, const std::set<std::pair<float, float>>& coordinates) override;
    void* GetFramebufferTextureId(int fbId) override;
    void SelectTextureFb(int fbId) override;
    uintptr_t ApplyPostShader(int srcFbId, int mode, uint32_t nativeW, uint32_t nativeH, uint32_t outW,
                              uint32_t outH) override;
    uintptr_t ApplyPostShaderChain(int srcFbId, const GdxPostShaderPipeline& pipeline, uint32_t nativeW,
                                   uint32_t nativeH, uint32_t outW, uint32_t outH) override;
    void DeleteTexture(uint32_t texId) override;
    void SetTextureFilter(FilteringMode mode) override;
    FilteringMode GetTextureFilter() override;
    void SetSrgbMode() override;
    ImTextureID GetTextureById(int id) override;

    PFN_D3D11_CREATE_DEVICE mDX11CreateDevice;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> mContext;
    Microsoft::WRL::ComPtr<ID3D11Device> mDevice;
    GfxWindowBackendDXGI* mWindowBackend = nullptr;
    D3D_FEATURE_LEVEL mFeatureLevel;

  private:
    // CRT post-process pass (gEnhancements.Graphics.CRTShader / CustomShader). Programs compile
    // lazily once per mode or custom file; the downsample and output framebuffers are created on
    // first use and resized by UpdateFramebufferParameters.
    struct PostShaderProgramD3D11 {
        Microsoft::WRL::ComPtr<ID3D11VertexShader> vertex_shader;
        Microsoft::WRL::ComPtr<ID3D11PixelShader> pixel_shader;
        Microsoft::WRL::ComPtr<ID3D11InputLayout> input_layout;
        Microsoft::WRL::ComPtr<ID3D11Buffer> constant_buffer;
        Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler_state;
        Microsoft::WRL::ComPtr<ID3D11BlendState> blend_state;
        Microsoft::WRL::ComPtr<ID3D11RasterizerState> rasterizer_state;
        bool attempted = false; // a failed compile is cached so it is not retried per frame
        uint64_t mtime = 0;     // used by the pipeline cache for hot-reload on save
        bool isSlang = false;
        std::vector<GdxSlangParameter> slangParameters;
        uint32_t slangCbSize = 0;
        GdxSlangUsedBuiltins slangUsedBuiltins;
    };
    struct PostShaderCacheEntryD3D11 {
        PostShaderProgramD3D11 program;
        uint64_t mtime = 0;
    };

    // Helpers defined in gfx_direct3d11.cpp need to populate the private nested struct.
    friend bool GdxCompilePostShaderD3D11(GfxRenderingAPIDX11* self, const char* vsSource, const char* psSource,
                                          GfxRenderingAPIDX11::PostShaderProgramD3D11* outProgram,
                                          bool needsConstantBuffer, D3D11_FILTER samplerFilter,
                                          D3D11_TEXTURE_ADDRESS_MODE wrapU, D3D11_TEXTURE_ADDRESS_MODE wrapV,
                                          uint32_t constantBufferSize, const char* pixelShaderProfile);
    friend bool GdxCompilePostCopyShaderD3D11(GfxRenderingAPIDX11* self,
                                              GfxRenderingAPIDX11::PostShaderProgramD3D11* outProgram);
    friend bool GdxCompilePostModeShaderD3D11(GfxRenderingAPIDX11* self, int mode,
                                              GfxRenderingAPIDX11::PostShaderProgramD3D11* outProgram);
    friend bool GdxCompilePipelinePassD3D11(GfxRenderingAPIDX11* self, const std::filesystem::path& path,
                                            PostShaderProgramD3D11* outProgram, bool filterLinear,
                                            GdxPostPassDesc::WrapMode wrapS, GdxPostPassDesc::WrapMode wrapT);
    friend GdxSlangUsedBuiltins GdxUnionUsedBuiltins(const std::vector<PostShaderProgramD3D11*>& programs);
    struct PostPipelineLutD3D11;
    friend bool GdxLoadPipelineLutsD3D11(GfxRenderingAPIDX11* self, const GdxPostShaderPipeline& pipeline,
                                         std::vector<PostPipelineLutD3D11>* outLuts,
                                         std::vector<std::filesystem::path>* outPaths);

    // Render-thread side of the async pipeline build: device objects from worker-compiled blobs.
    friend bool GdxCreatePostProgramObjectsD3D11(GfxRenderingAPIDX11* self, ID3DBlob* vsBlob, ID3DBlob* psBlob,
                                                 PostShaderProgramD3D11* outProgram, bool needsConstantBuffer,
                                                 D3D11_FILTER samplerFilter, D3D11_TEXTURE_ADDRESS_MODE wrapU,
                                                 D3D11_TEXTURE_ADDRESS_MODE wrapV, uint32_t constantBufferSize);
    friend bool GdxInstallPostPipelineBuildD3D11(GfxRenderingAPIDX11* self, GdxPostPipelineBuildD3D11* build);

    void CreateDepthStencilObjects(uint32_t width, uint32_t height, uint32_t msaa_count, ID3D11DepthStencilView** view,
                                   ID3D11ShaderResourceView** srv);

    HMODULE mDX11Module;

    HMODULE mCompilerModule;
    pD3DCompile mD3dCompile;
    decltype(&D3DCreateBlob) mD3dCreateBlob = nullptr;

    uint32_t mMsaaNumQualityLevels[D3D11_MAX_MULTISAMPLE_SAMPLE_COUNT];

    Microsoft::WRL::ComPtr<ID3D11RasterizerState> mRasterizerState;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> mDepthStencilState;
    // D3D11 state objects are immutable and only a handful of descriptors ever recur, so building
    // one per depth/decal flip is device-object construction on the draw path -- multiplied by the
    // sub-frame count once interpolation is on. The GL backend needs no equivalent; it sets
    // glDepthMask/glDepthFunc/glPolygonOffset directly, with no object to build.
    //   depth key : bit0 test, bit1 mask, bit2 decal
    //   raster key: bit0 decal, then z-fighting mode and render-target height (both feed
    //               SlopeScaledDepthBias)
    std::unordered_map<uint8_t, Microsoft::WRL::ComPtr<ID3D11DepthStencilState>> mDepthStencilCache;
    std::unordered_map<uint64_t, Microsoft::WRL::ComPtr<ID3D11RasterizerState>> mRasterizerCache;
    Microsoft::WRL::ComPtr<ID3D11Buffer> mVertexBuffer;
    Microsoft::WRL::ComPtr<ID3D11Buffer> mPerFrameCb;
    Microsoft::WRL::ComPtr<ID3D11Buffer> mPerDrawCb;
    Microsoft::WRL::ComPtr<ID3D11Buffer> mPerPrimDepthCb;
    Microsoft::WRL::ComPtr<ID3D11Buffer> mPerAlphaThresholdCb;
    Microsoft::WRL::ComPtr<ID3D11Buffer> mCoordBuffer;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> mCoordBufferSrv;
    Microsoft::WRL::ComPtr<ID3D11Buffer> mDepthValueOutputBuffer;
    Microsoft::WRL::ComPtr<ID3D11Buffer> mDepthValueOutputBufferCopy;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> mDepthValueOutputUav;
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> mComputeShader;
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> mComputeShaderMsaa;
    Microsoft::WRL::ComPtr<ID3DBlob> mComputeShaderMsaaBlob;
    size_t mCoordBufferSize;

#if DEBUG_D3D
    Microsoft::WRL::ComPtr<ID3D11Debug> debug;
#endif

    PerFrameCB mPerFrameCbData;
    PerDrawCB mPerDrawCbData;
    PerPrimDepthCB mPerPrimDepthCbData;
    PerAlphaThresholdCB mPerAlphaThresholdCbData;

    std::map<std::pair<uint64_t, uint32_t>, struct ShaderProgramD3D11> mShaderProgramPool;
    // Survives across runs, unlike mShaderProgramPool: a variant seen on a previous launch, or
    // shipped in the port archive, skips D3DCompile entirely. See gfx_shader_cache.h.
    ShaderBlobCache mShaderCache;

    std::vector<struct TextureData> mTextures;
    int mCurrentTile;
    uint32_t mCurrentTextureIds[SHADER_MAX_TEXTURES] = {};

    std::vector<FramebufferDX11> mFrameBuffers;

    // Current state

    struct ShaderProgramD3D11* mShaderProgram;

    int32_t mRenderTargetHeight;
    int mCurrentFramebuffer;
    FilteringMode mCurrentFilterMode = FILTER_NONE;
    uint32_t mFrameCount = 0;

    // Previous states (to prevent setting states needlessly)

    struct ShaderProgramD3D11* mLastShaderProgram = nullptr;
    uint32_t mLastVertexBufferStride = 0;
    Microsoft::WRL::ComPtr<ID3D11BlendState> mLastBlendState = nullptr;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> mLastResourceViews[SHADER_MAX_TEXTURES] = { nullptr, nullptr };
    Microsoft::WRL::ComPtr<ID3D11SamplerState> mLastSamplerStates[SHADER_MAX_TEXTURES] = { nullptr, nullptr };

    D3D_PRIMITIVE_TOPOLOGY mLastPrimitaveTopology = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;

    // Cached staging texture for ReadFramebufferToCPU — avoids CreateTexture2D/Release per frame
    Microsoft::WRL::ComPtr<ID3D11Texture2D> mReadbackStaging;
    uint32_t mReadbackStagingW = 0;
    uint32_t mReadbackStagingH = 0;


    PostShaderProgramD3D11 mPostShaderPrograms[2];
    std::unordered_map<std::string, PostShaderCacheEntryD3D11> mPostShaderCustomCache;
    PostShaderProgramD3D11 mPostCopyProgram;
    int mPostDownsampleFb = -1;
    int mPostOutputFb = -1;

    // Multi-pass pipeline state. Recompiled and reallocated when the pipeline or any pass file
    // changes on disk.
    std::unordered_map<std::string, PostShaderProgramD3D11> mPostPipelineProgramCache;
    std::vector<int> mPostPipelineFbs;

    // Slice 3: history ring for OriginalHistoryN, feedback storage for
    // PassFeedbackN, and LUT textures for UserN.
    std::vector<int> mPostPipelineHistoryFbs;
    size_t mPostPipelineHistoryIndex = 0;
    std::vector<int> mPostPipelineFeedbackFbs;
    struct PostPipelineLutD3D11 {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
        Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler;
        uint32_t width = 0; // 0 until stbi_load succeeds; UserNSize fills from these
        uint32_t height = 0;
    };
    std::vector<PostPipelineLutD3D11> mPostPipelineLuts;
    std::vector<std::filesystem::path> mPostPipelineLutPaths;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> mPostPipelineClampSampler;

    // In-flight async build plus the pipeline keys already resolved this session (see the
    // structs above). Keyed by preset path + content mtime so a disk edit re-triggers a build.
    std::shared_ptr<GdxPostPipelineBuildD3D11> mPostPipelineBuild;
    std::string mPostPipelineReadyKey;
    std::string mPostPipelineFailedKey;
};

std::string gfx_direct3d_common_build_shader(size_t& numFloats, const CCFeatures& cc_features,
                                             bool include_root_signature, bool three_point_filtering, bool use_srgb);
} // namespace Fast
#endif
#endif
