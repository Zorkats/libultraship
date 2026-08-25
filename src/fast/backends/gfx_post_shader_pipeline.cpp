#include "fast/backends/gfx_post_shader_pipeline.h"

#include <algorithm>
#include <cctype>
#include <cinttypes>
#include <cstring>
#include <fstream>
#include <sstream>
#include <unordered_set>

#include "spdlog/spdlog.h"

namespace Fast {

namespace {

std::string GdxTrim(const std::string& s) {
    size_t first = 0;
    while (first < s.size() && std::isspace(static_cast<unsigned char>(s[first]))) {
        ++first;
    }
    size_t last = s.size();
    while (last > first && std::isspace(static_cast<unsigned char>(s[last - 1]))) {
        --last;
    }
    return s.substr(first, last - first);
}

std::string GdxToLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// Strip matching leading/trailing quotes if present.
std::string GdxUnquote(std::string s) {
    s = GdxTrim(s);
    if (s.size() >= 2 && ((s.front() == '"' && s.back() == '"') || (s.front() == '\'' && s.back() == '\''))) {
        s = s.substr(1, s.size() - 2);
    }
    return GdxTrim(s);
}

bool GdxParseBool(const std::string& value) {
    const std::string v = GdxToLower(GdxUnquote(value));
    return v == "true" || v == "1" || v == "yes" || v == "on";
}

GdxPostPassDesc::ScaleType GdxParseScaleType(const std::string& value) {
    const std::string v = GdxToLower(GdxUnquote(value));
    if (v == "absolute" || v == "abs") {
        return GdxPostPassDesc::ScaleType::Absolute;
    }
    if (v == "viewport") {
        return GdxPostPassDesc::ScaleType::Viewport;
    }
    return GdxPostPassDesc::ScaleType::Source;
}

GdxPostPassDesc::WrapMode GdxParseWrapMode(const std::string& value) {
    const std::string v = GdxToLower(GdxUnquote(value));
    if (v == "clamp_to_border" || v == "clamp_to_border_color" || v == "border") {
        return GdxPostPassDesc::WrapMode::ClampToBorder;
    }
    if (v == "repeat") {
        return GdxPostPassDesc::WrapMode::Repeat;
    }
    if (v == "mirrored_repeat" || v == "mirrored" || v == "reflect") {
        return GdxPostPassDesc::WrapMode::MirroredRepeat;
    }
    return GdxPostPassDesc::WrapMode::ClampToEdge;
}

// Strip a known shader extension from the shader path so the active backend can
// append its own. RetroArch uses .slang; Slice 1 also accepts .glsl/.hlsl.
std::string GdxNormalizeShaderStem(std::string stem) {
    stem = GdxUnquote(stem);
    const std::string lower = GdxToLower(stem);
    for (const char* ext : { ".slang", ".glsl", ".hlsl" }) {
        if (lower.size() >= strlen(ext) && lower.compare(lower.size() - strlen(ext), strlen(ext), ext) == 0) {
            stem.resize(stem.size() - strlen(ext));
            break;
        }
    }
    return GdxTrim(stem);
}

struct GdxPostPresetSource {
    std::filesystem::path path;
    std::vector<std::string> lines;
};

bool GdxLoadPresetLines(const std::filesystem::path& path, std::vector<GdxPostPresetSource>* outSources,
                        std::string* error, size_t depth = 0) {
    if (depth > 8) {
        *error = "Too many #reference levels (cycle?)";
        return false;
    }

    // Collapse ".." segments before opening: deep pack presets (Mega_Bezel) build
    // long relative chains that can exceed MAX_PATH once made absolute, and the
    // Win32 path resolver rejects those even when the target exists.
    const std::filesystem::path openPath = path.lexically_normal();
    std::ifstream file(openPath, std::ios::binary);
    if (!file) {
        *error = std::string("Cannot open preset: ") + openPath.string();
        return false;
    }

    std::vector<std::string> lines;
    std::string line;
    while (std::getline(file, line)) {
        // Strip UTF-8 BOM on the first line.
        if (lines.empty() && line.size() >= 3 && (unsigned char)line[0] == 0xEF && (unsigned char)line[1] == 0xBB &&
            (unsigned char)line[2] == 0xBF) {
            line = line.substr(3);
        }
        lines.push_back(line);
    }

    // A preset may carry several #reference lines (e.g. Mega_Bezel chains a root
    // preset plus a .params file); all of them load, in file order.
    std::vector<std::string> referencePaths;
    for (const std::string& l : lines) {
        const std::string trimmed = GdxTrim(l);
        if (trimmed.rfind("#reference", 0) == 0) {
            std::string rest = GdxTrim(trimmed.substr(strlen("#reference")));
            referencePaths.push_back(GdxUnquote(rest));
        }
    }

    for (const std::string& referencePath : referencePaths) {
        if (referencePath.empty()) {
            continue;
        }
        std::filesystem::path resolved = path.parent_path() / referencePath;
        if (!GdxLoadPresetLines(resolved, outSources, error, depth + 1)) {
            return false;
        }
    }

    GdxPostPresetSource source;
    source.path = path;
    source.lines = std::move(lines);
    outSources->push_back(std::move(source));
    return true;
}

// Paths inside a preset (shaderN, texture paths) are relative to the file that
// declares them. Once #reference chains are flattened, each key must be rebased
// against its own source file's directory and re-expressed relative to the root
// preset's directory, which is what every downstream consumer joins against.
std::string GdxRebasePresetPath(const std::filesystem::path& rootDir, const std::filesystem::path& sourceDir,
                                const std::string& value) {
    if (value.empty()) {
        return value;
    }
    const std::filesystem::path p(value);
    if (p.is_absolute()) {
        return value;
    }
    const std::filesystem::path resolved = (sourceDir / p).lexically_normal();
    const std::filesystem::path rel = resolved.lexically_relative(rootDir);
    // Fall back to the normalized path when the target escapes the root tree.
    return (rel.empty() ? resolved : rel).generic_string();
}

struct GdxPostPresetValue {
    std::string value;
    std::filesystem::path sourceDir;
};

} // namespace

uint64_t GdxGetMtime(const std::filesystem::path& path) {
    try {
        return static_cast<uint64_t>(std::filesystem::last_write_time(path).time_since_epoch().count());
    } catch (...) {
        return 0;
    }
}

bool GdxParsePostShaderPipeline(const std::filesystem::path& presetPath, GdxPostShaderPipeline* outPipeline,
                                std::string* error) {
    *outPipeline = {};
    outPipeline->presetPath = presetPath;

    std::vector<GdxPostPresetSource> sources;
    if (!GdxLoadPresetLines(presetPath, &sources, error)) {
        return false;
    }

    // Flatten inherited sources and the current file into one key/value map.
    // Later sources override earlier ones. Texture metadata lives here too and
    // is split out afterwards. Each entry keeps its source file's directory so
    // path-valued keys can be rebased at consumption time.
    std::unordered_map<std::string, GdxPostPresetValue> values;

    for (const GdxPostPresetSource& source : sources) {
        for (const std::string& raw : source.lines) {
            std::string line = GdxTrim(raw);
            // Skip comments, blank lines, and reference directives (already handled).
            if (line.empty() || line[0] == '#' || line[0] == ';') {
                continue;
            }

            size_t eq = line.find('=');
            if (eq == std::string::npos) {
                continue;
            }
            const std::string key = GdxTrim(line.substr(0, eq));
            const std::string value = GdxUnquote(line.substr(eq + 1));
            if (!key.empty()) {
                values[key] = { value, source.path.parent_path() };
            }
        }
    }

    const std::filesystem::path rootDir = presetPath.parent_path();

    auto shadersIt = values.find("shaders");
    if (shadersIt == values.end()) {
        *error = "Missing required key: shaders";
        return false;
    }

    const int numShaders = std::atoi(shadersIt->second.value.c_str());
    if (numShaders < 1 || numShaders > 64) {
        *error = "Invalid shaders count (must be 1..64)";
        return false;
    }

    outPipeline->passes.resize(numShaders);

    // Parse the LUT block. `textures` lists names in UserN order; each name has
    // a matching path and optional sampler-state keys.
    auto texturesIt = values.find("textures");
    if (texturesIt != values.end()) {
        std::istringstream iss(texturesIt->second.value);
        std::string name;
        while (std::getline(iss, name, ';')) {
            name = GdxTrim(name);
            if (!name.empty()) {
                outPipeline->textureOrder.push_back(name);
            }
        }
    }
    for (const std::string& name : outPipeline->textureOrder) {
        GdxPostTextureDesc desc;
        auto pathIt = values.find(name);
        if (pathIt != values.end()) {
            desc.path = GdxRebasePresetPath(rootDir, pathIt->second.sourceDir, pathIt->second.value);
        }
        auto linearIt = values.find(name + "_linear");
        if (linearIt != values.end()) {
            desc.linear = GdxParseBool(linearIt->second.value);
        }
        auto wrapIt = values.find(name + "_wrap_mode");
        if (wrapIt != values.end()) {
            desc.wrap = GdxParseWrapMode(wrapIt->second.value);
        }
        outPipeline->textures[name] = std::move(desc);
    }

    // Any top-level key that is not a known pass/preset key or texture metadata
    // is treated as a shader parameter override. The shader file declares
    // parameters via #pragma parameter; these values override the defaults.
    std::unordered_set<std::string> textureKeys;
    textureKeys.insert("textures");
    for (const std::string& name : outPipeline->textureOrder) {
        textureKeys.insert(name);
        textureKeys.insert(name + "_linear");
        textureKeys.insert(name + "_wrap_mode");
    }

    for (const auto& kv : values) {
        const std::string& key = kv.first;
        if (key == "shaders" || key == "feedback_pass" || key == "#reference" || textureKeys.count(key)) {
            continue;
        }
        bool known = false;
        static const char* prefixes[] = { "shader",       "filter_linear",    "wrap_mode",       "scale_type",
                                          "scale_type_x", "scale_type_y",     "scale",           "scale_x",
                                          "scale_y",      "alias",            "float_framebuffer",
                                          "srgb_framebuffer", "frame_count_mod" };
        for (const char* prefix : prefixes) {
            if (key.rfind(prefix, 0) == 0) {
                known = true;
                break;
            }
        }
        if (!known) {
            // Parameter keys use the same identifier sanitation as the slang
            // translator (dashes are legal in slang parameter names but not in
            // the emitted GLSL member).
            std::string paramKey = key;
            for (char& c : paramKey) {
                if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_') {
                    c = '_';
                }
            }
            outPipeline->parameterOverrides[paramKey] =
                static_cast<float>(std::atof(GdxUnquote(kv.second.value).c_str()));
        }
    }

    uint64_t latestMtime = GdxGetMtime(presetPath);

    for (int i = 0; i < numShaders; ++i) {
        GdxPostPassDesc& pass = outPipeline->passes[i];

        const std::string shaderKey = "shader" + std::to_string(i);
        auto shaderIt = values.find(shaderKey);
        if (shaderIt == values.end()) {
            *error = "Missing required key: " + shaderKey;
            return false;
        }
        pass.shader = GdxRebasePresetPath(rootDir, shaderIt->second.sourceDir,
                                          GdxNormalizeShaderStem(shaderIt->second.value));
        if (pass.shader.empty()) {
            *error = "Empty shader path for pass " + std::to_string(i);
            return false;
        }

        // Verify the file exists for at least one backend so a missing pass is
        // reported at parse time rather than silently failing every frame.
        std::filesystem::path passPath = (presetPath.parent_path() / pass.shader).lexically_normal();
        const std::filesystem::path glslPath = passPath.replace_extension(".glsl");
        const std::filesystem::path hlslPath = passPath.replace_extension(".hlsl");
        const std::filesystem::path slangPath = passPath.replace_extension(".slang");
        if (!std::filesystem::exists(glslPath) && !std::filesystem::exists(hlslPath) &&
            !std::filesystem::exists(slangPath)) {
            *error = "Pass shader not found: " + passPath.string();
            return false;
        }
        latestMtime = std::max(latestMtime, GdxGetMtime(glslPath));
        latestMtime = std::max(latestMtime, GdxGetMtime(hlslPath));
        latestMtime = std::max(latestMtime, GdxGetMtime(slangPath));

        auto applyScaleType = [&](const std::string& key, GdxPostPassDesc::ScaleType* type) {
            auto it = values.find(key);
            if (it != values.end()) {
                *type = GdxParseScaleType(it->second.value);
            }
        };

        applyScaleType("scale_type" + std::to_string(i), &pass.scaleTypeX);
        pass.scaleTypeY = pass.scaleTypeX;

        auto sx = values.find("scale_type_x" + std::to_string(i));
        if (sx != values.end()) {
            pass.scaleTypeX = GdxParseScaleType(sx->second.value);
        }
        auto sy = values.find("scale_type_y" + std::to_string(i));
        if (sy != values.end()) {
            pass.scaleTypeY = GdxParseScaleType(sy->second.value);
        }

        auto scaleX = values.find("scale_x" + std::to_string(i));
        auto scaleY = values.find("scale_y" + std::to_string(i));
        auto scale = values.find("scale" + std::to_string(i));
        if (scaleX != values.end()) {
            pass.scaleX = std::max(1.0f, static_cast<float>(std::atof(GdxUnquote(scaleX->second.value).c_str())));
        } else if (scale != values.end()) {
            pass.scaleX = std::max(1.0f, static_cast<float>(std::atof(GdxUnquote(scale->second.value).c_str())));
        }
        if (scaleY != values.end()) {
            pass.scaleY = std::max(1.0f, static_cast<float>(std::atof(GdxUnquote(scaleY->second.value).c_str())));
        } else if (scale != values.end()) {
            pass.scaleY = std::max(1.0f, static_cast<float>(std::atof(GdxUnquote(scale->second.value).c_str())));
        }

        auto filterIt = values.find("filter_linear" + std::to_string(i));
        if (filterIt != values.end()) {
            pass.filterLinear = GdxParseBool(filterIt->second.value);
        }

        auto wrapIt = values.find("wrap_mode" + std::to_string(i));
        if (wrapIt != values.end()) {
            pass.wrapS = pass.wrapT = GdxParseWrapMode(wrapIt->second.value);
        }

        auto aliasIt = values.find("alias" + std::to_string(i));
        if (aliasIt != values.end()) {
            pass.alias = GdxUnquote(aliasIt->second.value);
        }
        if (pass.alias.empty()) {
            // `#pragma name X` is an implicit alias for the pass output: later
            // passes reference it as a sampler (crt-yah's phosphor pass is
            // sampled as `PhosphorPass`) without any aliasN key.
            std::ifstream slangIn(slangPath);
            std::string slangLine;
            while (std::getline(slangIn, slangLine)) {
                const std::string trimmed = GdxTrim(slangLine);
                if (trimmed.rfind("#pragma name", 0) == 0) {
                    pass.alias = GdxTrim(trimmed.substr(strlen("#pragma name")));
                    break;
                }
            }
        }

        // Slice 1 parses these but leaves them as no-ops.
        auto floatFb = values.find("float_framebuffer" + std::to_string(i));
        if (floatFb != values.end()) {
            pass.floatFramebuffer = GdxParseBool(floatFb->second.value);
        }
        auto srgbFb = values.find("srgb_framebuffer" + std::to_string(i));
        if (srgbFb != values.end()) {
            pass.srgbFramebuffer = GdxParseBool(srgbFb->second.value);
        }
        auto frameCountMod = values.find("frame_count_mod" + std::to_string(i));
        if (frameCountMod != values.end()) {
            pass.frameCountMod = static_cast<uint32_t>(std::atoi(GdxUnquote(frameCountMod->second.value).c_str()));
        }
        auto feedbackPass = values.find("feedback_pass" + std::to_string(i));
        if (feedbackPass != values.end()) {
            pass.feedbackPass = GdxParseBool(feedbackPass->second.value);
        }
    }

    outPipeline->mtime = latestMtime;
    return true;
}

bool GdxPostShaderPipelineChanged(const GdxPostShaderPipeline& pipeline) {
    if (pipeline.presetPath.empty()) {
        return false;
    }
    uint64_t latestMtime = GdxGetMtime(pipeline.presetPath);
    for (const GdxPostPassDesc& pass : pipeline.passes) {
        std::filesystem::path passPath = (pipeline.presetPath.parent_path() / pass.shader).lexically_normal();
        latestMtime = std::max(latestMtime, GdxGetMtime(passPath.replace_extension(".glsl")));
        latestMtime = std::max(latestMtime, GdxGetMtime(passPath.replace_extension(".hlsl")));
        latestMtime = std::max(latestMtime, GdxGetMtime(passPath.replace_extension(".slang")));
    }
    return latestMtime != pipeline.mtime;
}

std::filesystem::path GdxResolvePostShaderPath(const std::filesystem::path& appDir, const std::string& value) {
    const std::filesystem::path shaderDir = appDir / "shaders";
    if (value.find('/') != std::string::npos || value.find('\\') != std::string::npos) {
        return shaderDir / value;
    }
    constexpr size_t kSlangExtLen = 6; // ".slang"
    if (value.size() > kSlangExtLen && value.compare(value.size() - kSlangExtLen, kSlangExtLen, ".slang") == 0) {
        return shaderDir / value;
    }
    return shaderDir / "pipelines" / value;
}

std::string GdxPostShaderCvarStem(const std::filesystem::path& shadersDir, const std::filesystem::path& presetPath) {
    std::error_code ec;
    std::filesystem::path rel = std::filesystem::relative(presetPath, shadersDir, ec);
    std::string stem = (ec || rel.empty()) ? presetPath.stem().string() : rel.replace_extension().generic_string();
    constexpr char kPipelinesPrefix[] = "pipelines/";
    if (stem.rfind(kPipelinesPrefix, 0) == 0) {
        stem.erase(0, strlen(kPipelinesPrefix));
    }
    std::replace(stem.begin(), stem.end(), '/', '.');
    return stem;
}

} // namespace Fast
