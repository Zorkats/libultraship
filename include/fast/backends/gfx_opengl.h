#ifdef ENABLE_OPENGL
#pragma once

#include "gfx_rendering_api.h"
#include "gfx_shader_cache.h"
#include "fast/backends/gfx_slang_translator.h"
#include "../interpreter.h"

#include <atomic>
#include <memory>

#ifdef _MSC_VER
#include <SDL2/SDL.h>
// #define GL_GLEXT_PROTOTYPES 1
#include <GL/glew.h>
#elif FOR_WINDOWS
#include <GL/glew.h>
#include "SDL.h"
#define GL_GLEXT_PROTOTYPES 1
#include "SDL_opengl.h"
#elif __APPLE__
#include <SDL2/SDL.h>
#include <GL/glew.h>
#elif USE_OPENGLES
#include <SDL2/SDL.h>
#include <GLES3/gl3.h>
#else
#include <SDL2/SDL.h>
#define GL_GLEXT_PROTOTYPES 1
#include <SDL2/SDL_opengl.h>
#endif
namespace Fast {

// Async post-pipeline build state: the worker thread fills passes with translated GLSL sources
// (no GL calls); the render thread compiles and installs them when state flips to ready.
struct GdxPostPassBuildOGL {
    std::string cacheKey;
    uint64_t mtime = 0;
    bool ok = false;
    bool isSlang = false;
    std::string vsSource;
    std::string fsSource;
    std::vector<GdxSlangParameter> slangParameters;
    GdxSlangUsedBuiltins slangUsedBuiltins;
};

struct GdxPostPipelineBuildOGL {
    std::string key;
    std::vector<GdxPostPassBuildOGL> passes;
    std::atomic<int> state{ 0 }; // 0 = running, 1 = ready, 2 = failed
    std::atomic<bool> cancel{ false };
};
struct ShaderProgram {
    GLuint openglProgramId;
    uint8_t numInputs;
    bool usedTextures[SHADER_MAX_TEXTURES];
    uint8_t numFloats;
    GLint attribLocations[16];
    uint8_t attribSizes[16];
    uint8_t numAttribs;
    GLint frameCountLocation;
    GLint noiseScaleLocation;
    GLint prim_depth_location;
    GLint alpha_compare_threshold_location;
    /* Per-element locations, not one location per array. GLSL compilers trim an array uniform
       to its active size, so when a material's second texel is dead-stripped a two-element
       glUniform1iv is rejected wholesale: element 0 stays 0, filter3point divides by that zero
       size and every sample goes NaN/black. Inactive elements report -1 and are ignored. */
    GLint texture_width_locations[2];
    GLint texture_height_locations[2];
    GLint texture_filtering_locations[2];
};

struct FramebufferOGL {
    uint32_t width, height;
    bool has_depth_buffer;
    uint32_t msaa_level;
    bool invertY;
    GLint lastFormat = 0; // internal format last passed to glTexImage2D; 0 means unset

    GLuint fbo, clrbuf, clrbufMsaa, rbo;
};

struct TextureInfo {
    uint16_t width;
    uint16_t height;
    uint16_t filtering;
};

class GfxRenderingAPIOGL final : public GfxRenderingAPI {
  public:
    ~GfxRenderingAPIOGL() override = default;
    const char* GetName() override;
    int GetMaxTextureSize() override;
    GfxClipParameters GetClipParameters() override;
    void UnloadShader(ShaderProgram* oldPrg) override;
    void LoadShader(ShaderProgram* newPrg) override;
    ShaderProgram* CreateAndLoadNewShader(uint64_t shaderId0, uint64_t shaderId1) override;
    ShaderProgram* LookupShader(uint64_t shaderId0, uint64_t shaderId1) override;
    void ShaderGetInfo(ShaderProgram* prg, uint8_t* numInputs, bool usedTextures[2]) override;
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
    void ClearDepthRegion(int x, int y, int w, int h) override;
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

  private:
    void SetUniforms(ShaderProgram* prg) const;
    std::string BuildFsShader(const CCFeatures& cc_features);
    void SetPerDrawUniforms();

    std::vector<TextureInfo> textures;
    GLuint mCurrentTextureIds[SHADER_MAX_TEXTURES] = {};
    GLuint mLastBoundTextures[SHADER_MAX_TEXTURES] = {};
    uint8_t mCurrentTile;
    int8_t mLastActiveTexture = -1;
    int8_t mLastBlendEnabled = -1;
    int8_t mLastScissorEnabled = -1;

    std::map<std::pair<uint64_t, uint32_t>, ShaderProgram> mShaderProgramPool;
    ShaderProgram* mCurrentShaderProgram;
    ShaderProgram* mLastLoadedShader = nullptr;

    GLuint mOpenglVbo = 0;
#if defined(__APPLE__) || defined(USE_OPENGLES)
    GLuint mOpenglVao;
#endif

    uint32_t mFrameCount = 0;

    std::vector<FramebufferOGL> mFrameBuffers;
    size_t mCurrentFrameBuffer = 0;
    float mCurrentNoiseScale = 0.0f;
    FilteringMode mCurrentFilterMode = FILTER_THREE_POINT;

    GLint mMaxMsaaLevel = 1;
    GLuint mPixelDepthRb = 0;
    GLuint mPixelDepthFb = 0;
    size_t mPixelDepthRbSize = 0;

    // Survives across runs, unlike mShaderProgramPool: a variant seen on a previous launch skips
    // template expansion, glCompileShader and glLinkProgram. Sidecar only -- GL program binaries
    // are specific to the vendor, GPU and driver version, so nothing here can be shipped.
    ShaderBlobCache mShaderCache;

    // CRT post-process pass (gEnhancements.Graphics.CRTShader, ApplyPostShader). Programs compile
    // lazily once per mode and deliberately stay out of mShaderCache; the downsample and output
    // framebuffers are created on first use and resized by UpdateFramebufferParameters.
    struct PostShaderProgramOGL {
        GLuint program = 0;
        GLint srcSizeLocation = -1;
        GLint outSizeLocation = -1;
        bool attempted = false; // a failed compile is cached too, so it is not retried per frame
        uint64_t mtime = 0;     // used by the pipeline cache for hot-reload on save
        bool isSlang = false;
        std::vector<GdxSlangParameter> slangParameters;
        std::unordered_map<std::string, GLint> slangParameterLocations;
        GdxSlangUsedBuiltins slangUsedBuiltins;
        std::unordered_map<std::string, GLint> slangSamplerLocations;
        std::unordered_map<std::string, GLint> slangExtraSizeLocations; // `<Name>Size` vec4 uniforms
        GLint slangFrameCountLocation = -1;
        GLint slangMvpLocation = -1;
    };
    void GdxInitPostProgramOGL(PostShaderProgramOGL* prg);
    friend bool GdxCompilePipelinePassOGL(const std::filesystem::path& path, GfxRenderingAPIOGL* self,
                                          PostShaderProgramOGL* outPrg);
    friend GdxSlangUsedBuiltins GdxUnionUsedBuiltins(const std::vector<PostShaderProgramOGL*>& programs);
    friend bool GdxLoadPipelineLutsOGL(const GdxPostShaderPipeline& pipeline, std::vector<GLuint>* outTextures,
                                       std::vector<std::filesystem::path>* outPaths,
                                       std::vector<std::pair<uint32_t, uint32_t>>* outSizes);
    PostShaderProgramOGL mPostShaderPrograms[2];
    std::unordered_map<std::string, std::pair<PostShaderProgramOGL, uint64_t>> mPostShaderCustomCache;
    int mPostDownsampleFb = -1;
    int mPostOutputFb = -1;
#if defined(__APPLE__) || defined(USE_OPENGLES)
    GLuint mPostVao = 0; // attribute-less fullscreen triangle still needs a VAO on core profiles
#endif

    // Multi-pass pipeline state. Recompiled and reallocated when the pipeline or any pass file
    // changes on disk.
    std::unordered_map<std::string, PostShaderProgramOGL> mPostPipelineProgramCache;
    std::vector<int> mPostPipelineFbs;

    // Slice 3: history ring for OriginalHistoryN, feedback storage for
    // PassFeedbackN, and LUT textures for UserN.
    std::vector<int> mPostPipelineHistoryFbs;
    size_t mPostPipelineHistoryIndex = 0;
    std::vector<int> mPostPipelineFeedbackFbs;
    std::vector<GLuint> mPostPipelineLutTextures;
    std::vector<std::pair<uint32_t, uint32_t>> mPostPipelineLutSizes; // UserNSize fills from these
    std::vector<std::filesystem::path> mPostPipelineLutPaths;

    // Async pipeline build (see GdxPostPipelineBuildOGL above).
    std::shared_ptr<GdxPostPipelineBuildOGL> mPostPipelineBuild;
    std::string mPostPipelineReadyKey;
    std::string mPostPipelineFailedKey;

    friend bool GdxInstallPostPipelineBuildOGL(GfxRenderingAPIOGL* self, GdxPostPipelineBuildOGL* build);
};

} // namespace Fast
#endif
