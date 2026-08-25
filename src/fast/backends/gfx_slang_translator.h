#pragma once

#include <cstdint>
#include <filesystem>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace Fast {

// One #pragma parameter line parsed from a .slang file. Runtime editing is
// Slice 3; Slice 2 only supplies the default value to the translated shader.
struct GdxSlangParameter {
    std::string name;
    std::string description; // quoted label from the pragma, may be empty
    float defaultValue = 0.0f;
    float min = 0.0f;
    float max = 1.0f;
    float step = 0.01f;
};

enum class GdxSlangTarget {
    Glsl330, // OpenGL 3.3 backend
    HlslSm50 // Direct3D 11 shader model 5.0 backend
};

// Built-in texture families detected in a .slang pass. The backend uses these
// to allocate/bind only what the shader actually references.
struct GdxSlangUsedBuiltins {
    int maxOriginalHistory = -1; // highest N referenced by OriginalHistoryN; -1 if none
    std::set<int> passOutputIndices;  // PassOutputN indices used
    std::set<int> passFeedbackIndices; // PassFeedbackN indices used
    std::set<int> userTextureIndices; // UserN indices used
    // Every `<Name>Size` vec4 the source references beyond the four the host contract
    // already provides (SourceSize/OriginalSize/OutputSize/FinalViewportSize):
    // PassOutputNSize, PassFeedbackNSize, OriginalHistoryNSize, UserNSize, and
    // `<Alias>Size` for aliased passes. Sorted set = deterministic cbuffer/uniform order.
    // The backend resolves each name to a texture size at chain runtime.
    std::set<std::string> extraSizeNames;

    // sampler2D declarations that are not builtin families: preset LUT textures
    // (sampler named after a `.slangp` `textures` entry) and aliased pass
    // textures (`<Alias>` for this frame's output, `<Alias>Feedback` for the
    // previous frame's). Ordered by first declaration so host emission and
    // backend binding agree on the slot for each name.
    std::vector<std::string> namedSamplers;
};

struct GdxSlangTranslation {
    // Host-ready shader source for the requested backend. For OpenGL this is a
    // complete fragment shader body compatible with the existing single-file
    // post contract (vUV, uTex, uSrcSize, uOutSize, gdxFragColor). For D3D11
    // this is a complete pixel shader source containing a PSMain entry point.
    std::string source;

    // Translated vertex stage (VSMain for D3D11, main for OpenGL), present when
    // the .slang file declares `#pragma stage vertex`. The stage's Position /
    // TexCoord inputs are generated from the vertex id, so the pass loop still
    // draws the fullscreen triangle without binding vertex buffers.
    std::string vertexSource;
    bool hasVertexStage = false;

    std::vector<GdxSlangParameter> parameters;
    GdxSlangUsedBuiltins usedBuiltins;

    bool ok = false;
    std::string error;
};

// Translate a RetroArch .slang file into backend-specific source. The returned
// source is ready to be handed to the existing backend post-shader compilers.
// If translation fails, ok is false and error contains a human-readable reason.
GdxSlangTranslation GdxTranslateSlangFile(const std::filesystem::path& path, GdxSlangTarget target);

// Lightweight parse of just the #pragma parameter lines in a .slang file. Used
// by the UI to build runtime sliders without invoking the full translator.
std::vector<GdxSlangParameter> GdxParseSlangParameters(const std::filesystem::path& path);

// Binding layout shared between the translator (when it emits sampler
// declarations) and the backends (when they bind textures). Keeping the layout
// in one place guarantees GL and D3D11 use identical slots.
struct GdxSlangTextureBindings {
    static constexpr int kSourceBinding = 0;
    static constexpr int kOriginalHistoryBase = 1;
    static constexpr int kPassOutputBase = 9;
    static constexpr int kPassFeedbackBase = 17;
    static constexpr int kUserBase = 25;
    static constexpr int kNamedBase = 33;

    static int OriginalHistory(int n) { return kOriginalHistoryBase + n; }
    static int PassOutput(int n) { return kPassOutputBase + n; }
    static int PassFeedback(int n) { return kPassFeedbackBase + n; }
    static int User(int n) { return kUserBase + n; }
    static int Named(int n) { return kNamedBase + n; }
};

// When GDX_DUMP_SLANG_TRANSLATION is set in the environment, write the
// translated source to <exe>/logs/slang-<stem>-<backend>.txt for debugging.
void GdxDumpSlangTranslation(const std::filesystem::path& originalPath, GdxSlangTarget target,
                             const std::string& translatedSource);

// Byte layout of the GdxPostCB cbuffer emitted for the HLSL backend. The same
// algorithm is used by the translator when it writes the cbuffer and by the
// D3D11 backend when it fills the constant buffer each frame.
struct GdxSlangHlslCBufferLayout {
    // uSrcSize (c0) + uOutSize (c1) + gdxFrameCount (c2.x) + pad + gdxMvp (c3-c6).
    // Extra per-pass sizes (`<Name>Size` vec4s) start at c7 = byte 112, one
    // register each, in GdxSlangUsedBuiltins::extraSizeNames (sorted) order.
    // Parameters follow at byte 112 + extraSizeCount * 16.
    static constexpr uint32_t BuiltinSize = 112;
    static uint32_t TotalSize(const std::vector<GdxSlangParameter>& parameters, size_t extraSizeCount = 0);
    // Writes builtins and parameter defaults/overrides into outBytes (size must
    // be at least TotalSize). Override values not found fall back to defaults.
    // Extra size slots are zeroed here; the backend fills them per pass.
    static void Pack(const std::vector<GdxSlangParameter>& parameters,
                     const std::unordered_map<std::string, float>& overrides, uint32_t frameCount, uint8_t* outBytes,
                     size_t extraSizeCount = 0);
    static constexpr uint32_t ExtraSizeOffset(size_t index) { return BuiltinSize + index * 16; }
    static uint32_t ParamsOffset(size_t extraSizeCount) { return BuiltinSize + extraSizeCount * 16; }
};

} // namespace Fast
