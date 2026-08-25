#pragma once

#include <stdint.h>

#include <unordered_map>
#include <set>
#include "imconfig.h"

namespace Fast {
struct ShaderProgram;
struct GdxPostShaderPipeline;

struct GfxClipParameters {
    bool z_is_from_0_to_1;
    bool invertY;
};

enum FilteringMode { FILTER_THREE_POINT, FILTER_LINEAR, FILTER_NONE };

// Color formats used when a framebuffer is created as a render target. Most
// callers use the default RGBA8; the post-processing pipeline may request
// floating-point or sRGB storage per pass.
enum class GdxFramebufferFormat {
    R8G8B8A8_UNORM,
    R16G16B16A16_FLOAT,
    R8G8B8A8_UNORM_SRGB,
};

// A hash function used to hash a: pair<float, float>
struct hash_pair_ff {
    size_t operator()(const std::pair<float, float>& p) const {
        const auto hash1 = std::hash<float>{}(p.first);
        const auto hash2 = std::hash<float>{}(p.second);

        // If hash1 == hash2, their XOR is zero.
        return (hash1 != hash2) ? hash1 ^ hash2 : hash1;
    }
};

class GfxRenderingAPI {
  public:
    virtual ~GfxRenderingAPI() = default;
    virtual const char* GetName() = 0;
    virtual int GetMaxTextureSize() = 0;
    virtual GfxClipParameters GetClipParameters() = 0;
    virtual void UnloadShader(ShaderProgram* oldPrg) = 0;
    virtual void LoadShader(ShaderProgram* newPrg) = 0;
    virtual void ClearShaderCache() = 0;
    virtual ShaderProgram* CreateAndLoadNewShader(uint64_t shaderId0, uint64_t shaderId1) = 0;
    virtual ShaderProgram* LookupShader(uint64_t shaderId0, uint64_t shaderId1) = 0;
    virtual void ShaderGetInfo(ShaderProgram* prg, uint8_t* numInputs, bool usedTextures[2]) = 0;
    virtual uint32_t NewTexture() = 0;
    virtual void SelectTexture(int tile, uint32_t textureId) = 0;
    virtual void UploadTexture(const uint8_t* rgba32Buf, uint32_t width, uint32_t height) = 0;
    virtual void SetSamplerParameters(int sampler, bool linear_filter, uint32_t cms, uint32_t cmt) = 0;
    virtual void SetDepthTestAndMask(bool depth_test, bool z_upd) = 0;
    virtual void SetZmodeDecal(bool decal) = 0;
    virtual void SetViewport(int x, int y, int width, int height) = 0;
    virtual void SetScissor(int x, int y, int width, int height) = 0;
    virtual void SetUseAlpha(bool useAlpha) = 0;
    virtual void DrawTriangles(float buf_vbo[], size_t buf_vbo_len, size_t buf_vbo_num_tris) = 0;
    virtual void Init() = 0;
    virtual void OnResize() = 0;
    virtual void StartFrame() = 0;
    virtual void EndFrame() = 0;
    virtual void FinishRender() = 0;
    virtual int CreateFramebuffer() = 0;
    virtual void UpdateFramebufferParameters(int fb_id, uint32_t width, uint32_t height, uint32_t msaa_level,
                                             bool opengl_invertY, bool render_target, bool has_depth_buffer,
                                             bool can_extract_depth,
                                             GdxFramebufferFormat format = GdxFramebufferFormat::R8G8B8A8_UNORM) = 0;
    virtual void StartDrawToFramebuffer(int fbId, float noiseScale) = 0;
    virtual void CopyFramebuffer(int fbDstId, int fbSrcId, int srcX0, int srcY0, int srcX1, int srcY1, int dstX0,
                                 int dstY0, int dstX1, int dstY1) = 0;
    virtual void ClearFramebuffer(bool color, bool depth) = 0;
    virtual void ClearDepthRegion(int x, int y, int w, int h) {
        // Default: full depth clear. Backends that support scissored depth clears
        // (e.g. OpenGL) should override for a more precise partial clear.
        ClearFramebuffer(false, true);
    }
    virtual void ReadFramebufferToCPU(int fbId, uint32_t width, uint32_t height, uint16_t* rgba16Buf) = 0;
    virtual void ResolveMSAAColorBuffer(int fbIdTarger, int fbIdSrc) = 0;
    virtual std::unordered_map<std::pair<float, float>, uint16_t, hash_pair_ff>
    GetPixelDepth(int fb_id, const std::set<std::pair<float, float>>& coordinates) = 0;
    virtual void* GetFramebufferTextureId(int fbId) = 0;
    virtual void SelectTextureFb(int fbId) = 0;
    // Post-process hook (gEnhancements.Graphics.CRTShader / CustomShader): downsample
    // srcFbId's colour to nativeW x nativeH, run it through post-shader `mode` (1 = scanlines,
    // 2 = CRT) into an outW x outH target, and return that target's texture id for the interpreter
    // to publish. 0 = not applied; the caller then presents the source framebuffer as usual.
    virtual uintptr_t ApplyPostShader(int srcFbId, int mode, uint32_t nativeW, uint32_t nativeH, uint32_t outW,
                                      uint32_t outH) {
        return 0;
    }
    // Multi-pass pipeline hook (gEnhancements.Graphics.PostPipeline). Runs the parsed .slangp
    // chain through per-pass render targets and returns the final texture id. 0 = not applied.
    virtual uintptr_t ApplyPostShaderChain(int srcFbId, const GdxPostShaderPipeline& pipeline, uint32_t nativeW,
                                           uint32_t nativeH, uint32_t outW, uint32_t outH) {
        return 0;
    }
    virtual void DeleteTexture(uint32_t texId) = 0;
    virtual void SetTextureFilter(FilteringMode mode) = 0;
    virtual FilteringMode GetTextureFilter() = 0;
    virtual void SetSrgbMode() = 0;
    virtual ImTextureID GetTextureById(int id) = 0;
    virtual void SetCurrentPrimDepth(float depth) = 0;
    // For G_AC_THRESHOLD the RDP compares texel alpha against the SETBLENDCOLOR alpha
    // register, not a fixed constant; COPY-mode HUD sprite cutouts depend on it. Backends
    // must route this into the alpha discard test. Pushed every Flush(), like prim depth.
    virtual void SetCurrentAlphaCompareThreshold(float threshold) = 0;

  protected:
    int8_t mCurrentDepthTest = 0;
    int8_t mCurrentDepthMask = 0;
    int8_t mCurrentZmodeDecal = 0;
    int8_t mLastDepthTest = -1;
    int8_t mLastDepthMask = -1;
    int8_t mLastZmodeDecal = -1;
    bool mSrgbMode = false;
    float mCurrentPrimDepth = 0.0f;
    bool mPrimDepthDirty = true;
    float mCurrentAlphaCompareThreshold = 1.0f;
    bool mAlphaCompareThresholdDirty = true;
};
} // namespace Fast
