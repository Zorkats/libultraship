#pragma once

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace Fast {

// Description of one pass in a RetroArch-style .slangp preset. Slice 1 keeps
// the contract identical to the existing single-file post shader: the backend
// injects vertex/fragment boilerplate and the user file only supplies the
// per-pixel body. The parser fills this struct from the preset keys.
struct GdxPostPassDesc {
    enum class ScaleType {
        Source,   // multiply the source resolution
        Absolute, // fixed pixel size
        Viewport, // multiply the final output resolution
    };

    enum class WrapMode {
        ClampToEdge,
        ClampToBorder,
        Repeat,
        MirroredRepeat,
    };

    // Shader file stem, relative to the preset directory. The active backend
    // appends .glsl or .hlsl when it loads the pass.
    std::string shader;

    ScaleType scaleTypeX = ScaleType::Source;
    ScaleType scaleTypeY = ScaleType::Source;
    float scaleX = 1.0f;
    float scaleY = 1.0f;

    bool filterLinear = true;
    WrapMode wrapS = WrapMode::ClampToEdge;
    WrapMode wrapT = WrapMode::ClampToEdge;

    std::string alias;

    // Parsed in Slice 1 but intentionally no-ops until Slice 3.
    bool floatFramebuffer = false;
    bool srgbFramebuffer = false;
    uint32_t frameCountMod = 0;
    bool feedbackPass = false;
};

// Resolve the effective pixel size for one pipeline pass.
inline uint32_t GdxResolvePostPassScale(GdxPostPassDesc::ScaleType type, float scale, uint32_t sourceDim,
                                        uint32_t viewportDim) {
    switch (type) {
        case GdxPostPassDesc::ScaleType::Absolute:
            return sourceDim == 0 ? 1 : std::max(1u, static_cast<uint32_t>(scale));
        case GdxPostPassDesc::ScaleType::Viewport:
            return viewportDim == 0 ? 1 : std::max(1u, static_cast<uint32_t>(static_cast<float>(viewportDim) * scale));
        case GdxPostPassDesc::ScaleType::Source:
        default:
            return sourceDim == 0 ? 1 : std::max(1u, static_cast<uint32_t>(static_cast<float>(sourceDim) * scale));
    }
}

// Best-effort file modification time for hot-reload checks. Returns 0 on error.
uint64_t GdxGetMtime(const std::filesystem::path& path);

// LUT texture description from a .slangp preset. UserN mapping follows the
// declaration order of the `textures` key.
struct GdxPostTextureDesc {
    std::string path;
    bool linear = true;
    GdxPostPassDesc::WrapMode wrap = GdxPostPassDesc::WrapMode::ClampToEdge;
};

// Parsed representation of a .slangp preset. Both backends consume the same
// description so the interpreter can feed sizes without knowing GL/D3D11.
struct GdxPostShaderPipeline {
    std::filesystem::path presetPath;
    std::vector<GdxPostPassDesc> passes;

    // LUT texture block. `textureOrder` preserves the `textures = "a;b;c"`
    // declaration order so User0, User1, ... map consistently with RetroArch.
    std::vector<std::string> textureOrder;
    std::unordered_map<std::string, GdxPostTextureDesc> textures;

    // Slice 2: parameter overrides parsed from the preset root. The shader file
    // itself supplies defaults via #pragma parameter; these keys override them.
    std::unordered_map<std::string, float> parameterOverrides;

    // Last known modification time of the preset file and every resolved pass
    // file. Used by the backends to invalidate cached compiled programs.
    uint64_t mtime = 0;
};

// Parse a .slangp preset into outPipeline. Returns false and fills error on
// failure. Supports #reference inheritance and the subset of keys defined for
// Slice 1.
bool GdxParsePostShaderPipeline(const std::filesystem::path& presetPath, GdxPostShaderPipeline* outPipeline,
                                std::string* error);

// Re-scan the preset and every resolved pass file. Returns true if any mtime
// differs from what was recorded in pipeline->mtime.
bool GdxPostShaderPipelineChanged(const GdxPostShaderPipeline& pipeline);

// Resolve a PostPipeline CVar value to an on-disk path. Values containing a path
// separator are relative to <appDir>/shaders (recursive scan results); bare
// filenames keep the legacy locations: loose .slang in shaders/, .slangp in
// shaders/pipelines/.
std::filesystem::path GdxResolvePostShaderPath(const std::filesystem::path& appDir, const std::string& value);

// CVar namespace stem for parameter overrides. Presets nested below shaders/ use
// their dotted relative path ("slang-shaders.crt.crt-royale") so same-named
// presets in different folders don't share overrides; the legacy pipelines/
// prefix is stripped so existing per-preset CVars keep applying.
std::string GdxPostShaderCvarStem(const std::filesystem::path& shadersDir, const std::filesystem::path& presetPath);

} // namespace Fast
