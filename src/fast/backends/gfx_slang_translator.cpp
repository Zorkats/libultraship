#include "fast/backends/gfx_slang_translator.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <regex>
#include <sstream>
#include <unordered_set>

#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>
#include <SPIRV/GlslangToSpv.h>
#include <spirv_glsl.hpp>
#include <spirv_hlsl.hpp>

#include "spdlog/spdlog.h"
#include "ship/Context.h"

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

std::string GdxReadFile(const std::filesystem::path& path) {
    // Collapse ".." segments before opening: pack presets build long relative
    // chains that can exceed MAX_PATH once made absolute, and Win32 rejects
    // those even when the target exists.
    std::ifstream file(path.lexically_normal(), std::ios::binary);
    if (!file) {
        return "";
    }
    std::ostringstream ss;
    ss << file.rdbuf();
    std::string result = ss.str();
    if (result.size() >= 3 && (unsigned char)result[0] == 0xEF && (unsigned char)result[1] == 0xBB &&
        (unsigned char)result[2] == 0xBF) {
        result.erase(0, 3);
    }
    return result;
}

// Recursively expand #include directives relative to the including file.
// Cycles and excessive depth are rejected so a malformed shader cannot hang
// the compiler.
bool GdxResolveSlangIncludes(const std::filesystem::path& path, std::string* inOutText, std::string* error,
                             std::vector<std::filesystem::path>* stack, size_t depth = 0) {
    static constexpr size_t kMaxDepth = 8;
    if (depth > kMaxDepth) {
        *error = "#include depth exceeded in " + path.string();
        return false;
    }
    for (const auto& included : *stack) {
        if (included == path) {
            *error = "#include cycle detected: " + path.string();
            return false;
        }
    }

    stack->push_back(path);
    const std::filesystem::path baseDir = path.parent_path();

    std::string output;
    std::istringstream stream(*inOutText);
    std::string line;
    while (std::getline(stream, line)) {
        std::string trimmed = GdxTrim(line);
        if (trimmed.rfind("#include", 0) != 0) {
            output += line + "\n";
            continue;
        }

        std::string rest = GdxTrim(trimmed.substr(strlen("#include")));
        if (rest.size() < 2 ||
            !((rest.front() == '"' && rest.back() == '"') || (rest.front() == '<' && rest.back() == '>'))) {
            output += line + "\n";
            continue;
        }

        std::string includeName = rest.substr(1, rest.size() - 2);
        std::filesystem::path includePath = baseDir / includeName;
        std::string includedText = GdxReadFile(includePath);
        if (includedText.empty()) {
            *error = "Cannot open #include: " + includePath.string();
            stack->pop_back();
            return false;
        }
        if (!GdxResolveSlangIncludes(includePath, &includedText, error, stack, depth + 1)) {
            stack->pop_back();
            return false;
        }
        output += includedText;
    }

    stack->pop_back();
    *inOutText = std::move(output);
    return true;
}

struct GdxSlangStage {
    std::string source;
    bool present = false;
};

struct GdxSlangFile {
    GdxSlangStage vertex;
    GdxSlangStage fragment;
    std::vector<GdxSlangParameter> parameters;
};

GdxSlangFile GdxParseSlangFile(const std::string& text) {
    GdxSlangFile result;
    std::istringstream stream(text);
    std::string line;

    enum class Stage {
        None,
        Vertex,
        Fragment,
    } stage = Stage::None;

    std::string vertexLines;
    std::string fragmentLines;
    std::string commonLines;

    while (std::getline(stream, line)) {
        std::string trimmed = GdxTrim(line);
        if (trimmed.rfind("#pragma stage", 0) == 0) {
            std::string rest = GdxTrim(trimmed.substr(13));
            std::transform(rest.begin(), rest.end(), rest.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (rest == "vertex") {
                stage = Stage::Vertex;
            } else if (rest == "fragment") {
                stage = Stage::Fragment;
            } else {
                stage = Stage::None;
            }
            continue;
        }

        if (trimmed.rfind("#pragma parameter", 0) == 0) {
            std::string rest = GdxTrim(trimmed.substr(17));
            std::istringstream iss(rest);
            std::string name;
            if (!(iss >> name)) {
                continue;
            }
            // slang allows dashes in parameter names (guest's `ntsc-row0`
            // separator rows); GLSL identifiers do not, and the name is
            // emitted as a push-block member.
            for (char& c : name) {
                if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_') {
                    c = '_';
                }
            }
            // Slang order is NAME "Description" def min max step. The description is optional;
            // skipping it as a token is NOT safe — a failed float extraction sets failbit and
            // would silently zero the remaining numbers.
            GdxSlangParameter param;
            param.name = name;
            if (iss >> std::ws && iss.peek() == '"') {
                iss.get();
                std::string description;
                std::getline(iss, description, '"');
                param.description = GdxTrim(description);
            }
            float def = 0.0f, min = 0.0f, max = 1.0f, step = 0.01f;
            if (iss >> def) {
                param.defaultValue = def;
            }
            if (iss >> min) {
                param.min = min;
            }
            if (iss >> max) {
                param.max = max;
            }
            if (iss >> step) {
                param.step = step;
            }
            // Includes shared across stages can declare the same parameter
            // twice; a duplicate would emit the member name twice in the host
            // uniform block.
            bool seen = false;
            for (const GdxSlangParameter& existing : result.parameters) {
                if (existing.name == param.name) {
                    seen = true;
                    break;
                }
            }
            if (!seen) {
                result.parameters.push_back(std::move(param));
            }
            continue;
        }

        switch (stage) {
            case Stage::Vertex:
                vertexLines += line + "\n";
                break;
            case Stage::Fragment:
                fragmentLines += line + "\n";
                break;
            default:
                commonLines += line + "\n";
                break;
        }
    }

    // No stage directives means the file is a fragment shader body.
    if (vertexLines.empty() && fragmentLines.empty()) {
        fragmentLines = commonLines;
        commonLines.clear();
    }

    if (!vertexLines.empty()) {
        result.vertex.present = true;
        result.vertex.source = commonLines + vertexLines;
    }
    if (!fragmentLines.empty()) {
        result.fragment.present = true;
        result.fragment.source = commonLines + fragmentLines;
    }
    return result;
}

// Scan the fragment source for RetroArch texture-family builtins the shader
// actually references. We only declare/bind what is used so the host wrapper
// does not exhaust sampler slots.
GdxSlangUsedBuiltins GdxDetectUsedBuiltins(const std::string& source) {
    GdxSlangUsedBuiltins result;
    static const std::regex historyRe(R"(\bOriginalHistory(\d+)\b)");
    static const std::regex passOutRe(R"(\bPassOutput(\d+)\b)");
    static const std::regex passFbRe(R"(\bPassFeedback(\d+)\b)");
    static const std::regex userRe(R"(\bUser(\d+)\b)");

    std::sregex_iterator end;
    for (auto it = std::sregex_iterator(source.begin(), source.end(), historyRe); it != end; ++it) {
        int n = std::atoi((*it)[1].str().c_str());
        result.maxOriginalHistory = std::max(result.maxOriginalHistory, n);
    }
    for (auto it = std::sregex_iterator(source.begin(), source.end(), passOutRe); it != end; ++it) {
        result.passOutputIndices.insert(std::atoi((*it)[1].str().c_str()));
    }
    for (auto it = std::sregex_iterator(source.begin(), source.end(), passFbRe); it != end; ++it) {
        result.passFeedbackIndices.insert(std::atoi((*it)[1].str().c_str()));
    }
    for (auto it = std::sregex_iterator(source.begin(), source.end(), userRe); it != end; ++it) {
        result.userTextureIndices.insert(std::atoi((*it)[1].str().c_str()));
    }

    // sampler2D declarations outside the builtin families are preset LUT
    // samplers or `<Alias>` / `<Alias>Feedback` pass textures. The host remaps
    // them onto gdxNamedN slots; the backend binds by name at chain runtime.
    static const std::regex samplerRe(R"((?:layout\s*\([^)]*\)\s*)?uniform\s+sampler2D\s+(\w+))");
    for (auto it = std::sregex_iterator(source.begin(), source.end(), samplerRe); it != end; ++it) {
        const std::string name = (*it)[1].str();
        if (name == "Source" || name == "Original" || name == "uTex" || name.rfind("gdx", 0) == 0) {
            continue;
        }
        bool builtin = false;
        for (const std::regex* family : { &historyRe, &passOutRe, &passFbRe, &userRe }) {
            if (std::regex_match(name, *family)) {
                builtin = true;
                break;
            }
        }
        if (builtin) {
            continue;
        }
        // A shader that #defines the sampler name itself (hsm-custom-fast-sharpen:
        // `#define PrePass0 Source` in the non-NTSC branch) manages the name on
        // its own; remapping it onto gdxNamedN would collide with that define
        // and sample the wrong texture when the alias branch is active.
        const std::regex selfDefineRe("#define\\s+" + name + "\\b");
        if (std::regex_search(source, selfDefineRe)) {
            continue;
        }
        bool seen = false;
        for (const std::string& existing : result.namedSamplers) {
            if (existing == name) {
                seen = true;
                break;
            }
        }
        if (!seen) {
            result.namedSamplers.push_back(name);
        }
    }

    // A pass can re-point a builtin sampler at a pass-alias texture by macro
    // (`#define Source PhosphorPass`, crt-yah's sharp-blur). The alias is never
    // declared as a sampler, so register it for a gdxNamedN slot here; the
    // backend resolves the name to the aliased pass's output at chain runtime.
    static const std::regex aliasDefineRe(
        R"(#define\s+(?:Source|Original|OriginalHistory\d+|PassOutput\d+|PassFeedback\d+|User\d+)\s+(\w+))");
    for (auto it = std::sregex_iterator(source.begin(), source.end(), aliasDefineRe); it != end; ++it) {
        const std::string name = (*it)[1].str();
        if (name == "Source" || name == "Original" || name == "uTex" || name.rfind("gdx", 0) == 0) {
            continue;
        }
        bool builtin = false;
        for (const std::regex* family : { &historyRe, &passOutRe, &passFbRe, &userRe }) {
            if (std::regex_match(name, *family)) {
                builtin = true;
                break;
            }
        }
        if (builtin) {
            continue;
        }
        bool seen = false;
        for (const std::string& existing : result.namedSamplers) {
            if (existing == name) {
                seen = true;
                break;
            }
        }
        if (!seen) {
            result.namedSamplers.push_back(name);
        }
    }

    // `<Name>Size` vec4s beyond the four the host macro-maps (PassOutputNSize,
    // `<Alias>Size`, ...). The shader keeps referencing them by name, so the
    // host must declare them and the backend fill them at chain runtime.
    // Anything followed by `(` is a function (HSM_GetRotatedCoreOriginalSize),
    // not a vec4 — declaring it would collide with the real definition.
    std::unordered_set<std::string> functionNames;
    static const std::regex callRe(R"(\b([A-Za-z_]\w*)\s*\()");
    for (auto it = std::sregex_iterator(source.begin(), source.end(), callRe); it != end; ++it) {
        functionNames.insert((*it)[1].str());
    }
    // Stage varyings (crt-geom passes its own `TextureSize` vertex→fragment)
    // are the shader's data, not a size the backend must fill.
    std::unordered_set<std::string> varyingNames;
    static const std::regex varyingRe(
        R"((?:^|\n)\s*(?:layout\s*\([^)]*\)\s*)?(?:(?:noperspective|flat|smooth|centroid|sample|invariant)\s+)*(?:in|out)\s+\w+\s+(\w+)\s*;)");
    for (auto it = std::sregex_iterator(source.begin(), source.end(), varyingRe); it != end; ++it) {
        varyingNames.insert((*it)[1].str());
    }
    static const std::regex extraSizeRe(R"(\b([A-Za-z_]\w*Size)\b)");
    // Names the shader declares itself (hyllian's `const float LUT_Size`) are
    // not host-supplied sizes either.
    std::unordered_set<std::string> declaredGlobals;
    static const std::regex globalDeclRe(
        R"((?:^|\n)\s*(?:const\s+)?(?:float|int|uint|bool|vec[234]|mat[34])\s+(\w+)\s*[=;])");
    for (auto it = std::sregex_iterator(source.begin(), source.end(), globalDeclRe); it != end; ++it) {
        declaredGlobals.insert((*it)[1].str());
    }
    for (auto it = std::sregex_iterator(source.begin(), source.end(), extraSizeRe); it != end; ++it) {
        const std::string name = (*it)[1].str();
        if (name == "SourceSize" || name == "OriginalSize" || name == "OutputSize" ||
            name == "FinalViewportSize" || name == "textureSize" || name == "uSrcSize" || name == "uOutSize" ||
            name.rfind("gdx", 0) == 0 || functionNames.count(name) != 0 || varyingNames.count(name) != 0 ||
            declaredGlobals.count(name) != 0) {
            continue;
        }
        result.extraSizeNames.insert(name);
    }
    return result;
}

bool GdxLooksLikeBuiltinDeclaration(const std::string& line) {
    // Function signatures are not declarations: HSM helpers take parameters
    // named like builtins (`sampler2D original_pass`, `inout vec2 ...`),
    // and a substring heuristic eats the line, orphaning the body. The layout
    // qualifier's own parens must not count as a signature.
    static const std::regex layoutRe(R"(^\s*layout\s*\([^)]*\))");
    const std::string rest = std::regex_replace(line, layoutRe, "");
    if (rest.find('(') != std::string::npos) {
        return false;
    }
    static const std::regex declRe(R"(^\s*(uniform|in|out)\b)", std::regex_constants::icase);
    if (!std::regex_search(rest, declRe)) {
        return false;
    }
    static const std::regex builtinRe(
        R"(\b(source|original|sourcesize|originalsize|outputsize|finalviewportsize|framecount|framedirection|mvp|vtexcoord|fragcolor|texcoord)\b|\b(originalhistory|passoutput|passfeedback|user)\d+\b)",
        std::regex_constants::icase);
    return std::regex_search(rest, builtinRe);
}

// RetroArch slang shaders declare their builtins and parameters inside named
// uniform blocks (`layout(push_constant) uniform Push { ... } params;` and
// `layout(std140) uniform UBO { mat4 MVP; } global;`). The host wrapper already
// provides those members in its own GdxPush block, so a native block collides
// with it and its instance references (params.X, global.MVP) dangle. Strip a
// block when every member is a known builtin, a `<Name>Size`, a #pragma
// parameter, or a plain float — float members without a pragma (Mega_Bezel
// declares its whole monolithic UBO in every pass, pragma or not) are promoted
// to parameters with default 0 so preset overrides still reach them by name —
// anything else is the shader's own data and stays — and remove the stripped
// instance prefixes from all references.
std::string GdxStripNativeUniformBlocks(const std::string& source,
                                        std::vector<GdxSlangParameter>& parameters) {
    static const std::regex blockRe(R"((?:layout\s*\([^)]*\)\s*)?uniform\s+\w+\s*\{([^{}]*)\}\s*(\w+)?\s*;)");
    // Statement-level member match so multi-declarator lines (`float deadline,
    // debug_toggle;`) register every name; the old per-name match saw only the first,
    // and `params.debug_toggle` kept its prefix after the block was stripped.
    static const std::regex memberRe(R"(\b(u?int|float|vec[234]|mat[34])\s+([^;]+);)");
    static const std::regex declaratorRe(R"(\b(\w+)\s*(?:\[[^\]]*\])?\s*(?:=[^,]*)?(?:,|$))");
    static const std::unordered_set<std::string> builtinMembers = {
        "sourcesize", "originalsize", "outputsize", "finalviewportsize", "framecount", "framedirection", "mvp",
    };

    std::string result;
    result.reserve(source.size());
    std::vector<std::string> instances;
    std::vector<std::string> promotions;
    std::unordered_set<std::string> strippedMembers;
    bool strippedAny = false;
    size_t last = 0;
    std::sregex_iterator end;
    for (auto it = std::sregex_iterator(source.begin(), source.end(), blockRe); it != end; ++it) {
        const std::smatch& match = *it;
        // Commented-out members (mame's `// float vectorscreen;` inside Push)
        // must not promote to parameters.
        std::string body = std::regex_replace(match[1].str(), std::regex(R"(//[^\n]*)"), "");
        body = std::regex_replace(body, std::regex(R"(/\*[\s\S]*?\*/)"), "");
        bool strippable = true;
        bool sawMember = false;
        std::vector<std::string> blockPromotions;
        std::vector<std::string> blockMembers;
        for (auto mit = std::sregex_iterator(body.begin(), body.end(), memberRe); mit != end; ++mit) {
            const std::string type = (*mit)[1].str();
            const std::string declarators = (*mit)[2].str();
            for (auto dit = std::sregex_iterator(declarators.begin(), declarators.end(), declaratorRe); dit != end; ++dit) {
                sawMember = true;
                const std::string member = (*dit)[1].str();
                // Comma inside an initializer (`= vec2(1.0, 2.0)`) can surface a
                // numeric fragment as a false declarator.
                if (member.find_first_not_of("0123456789") == std::string::npos) {
                    continue;
                }
                blockMembers.push_back(member);
                std::string lower;
                lower.reserve(member.size());
                std::transform(member.begin(), member.end(), std::back_inserter(lower),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (builtinMembers.count(lower) > 0) {
                    continue;
                }
                // `<Name>Size` members are slang's per-pass texture sizes: the host
                // contract supplies them, so they do not keep a block alive either.
                if (lower.size() > 4 && lower.compare(lower.size() - 4, 4, "size") == 0) {
                    continue;
                }
                bool isParam = false;
                for (const GdxSlangParameter& p : parameters) {
                    if (p.name == member) {
                        isParam = true;
                        break;
                    }
                }
                if (!isParam) {
                    // Scalars without a pragma promote to default-0 parameters; any
                    // other undeclared member type is the shader's own data.
                    if (type == "float" || type == "int" || type == "uint") {
                        blockPromotions.push_back(member);
                        continue;
                    }
                    strippable = false;
                    break;
                }
            }
            if (!strippable) {
                break;
            }
        }
        if (!strippable || !sawMember) {
            continue;
        }
        strippedAny = true;
        result += source.substr(last, static_cast<size_t>(match.position()) - last);
        last = static_cast<size_t>(match.position() + match.length());
        if (match[2].matched) {
            instances.push_back(match[2].str());
        }
        for (const std::string& member : blockMembers) {
            strippedMembers.insert(member);
        }
        for (const std::string& name : blockPromotions) {
            bool seen = false;
            for (const std::string& existing : promotions) {
                if (existing == name) {
                    seen = true;
                    break;
                }
            }
            if (!seen) {
                promotions.push_back(name);
            }
        }
    }
    // Anonymous blocks (`uniform UBO { ... };`, crt-beans) strip too; they just
    // have no instance prefix to remove afterward.
    if (!strippedAny) {
        return source;
    }
    result += source.substr(last);
    // Compatibility includes alias the stripped instance (`#define IN params`),
    // after which `IN.texture_size` expands to `params.(uSrcSize)...` and fails
    // to parse. Treat such aliases as instance prefixes too. Index loop: the
    // alias list grows while scanning.
    for (size_t i = 0; i < instances.size(); ++i) {
        const std::regex aliasRe("#define\\s+(\\w+)\\s+" + instances[i] + "\\s*\\n");
        std::smatch aliasMatch;
        if (std::regex_search(result, aliasMatch, aliasRe)) {
            instances.push_back(aliasMatch[1].str());
        }
    }
    // Only collapse `instance.member` when the bare name still resolves: a member
    // of the stripped block, or a compat macro (`#define texture_size ...`, which
    // is what `IN.texture_size` relies on). A local struct variable that reuses
    // the instance name (scanline-classic's `TimebaseConfig config;`) keeps its
    // prefix - its fields are neither. The macro census is hoisted: rebuilding it
    // per instance was quadratic on Mega_Bezel-scale sources.
    std::unordered_set<std::string> macroNames;
    static const std::regex defineRe("#define\\s+(\\w+)");
    for (auto dit = std::sregex_iterator(result.begin(), result.end(), defineRe); dit != end; ++dit) {
        macroNames.insert((*dit)[1].str());
    }
    for (const std::string& instance : instances) {
        const std::regex prefixRe("\\b" + instance + "\\.(\\w+)");
        std::string rebuilt;
        rebuilt.reserve(result.size());
        size_t cursor = 0;
        for (auto pit = std::sregex_iterator(result.begin(), result.end(), prefixRe); pit != end; ++pit) {
            const std::string member = (*pit)[1].str();
            if (strippedMembers.count(member) == 0 && macroNames.count(member) == 0) {
                continue;
            }
            rebuilt += result.substr(cursor, static_cast<size_t>(pit->position()) - cursor);
            rebuilt += member;
            cursor = static_cast<size_t>(pit->position() + pit->length());
        }
        if (cursor > 0) {
            rebuilt += result.substr(cursor);
            result = std::move(rebuilt);
        }
    }
    // `#define X X` self-guards (HSM shields builtins from compat macros this
    // way) clash with the host's macro mapping; with the blocks stripped the
    // host define is the only mapping left, so drop the self-define.
    result = std::regex_replace(result, std::regex("\\n[ \\t]*#define[ \\t]+(\\w+)[ \\t]+\\1[ \\t]*(?=\\n)"), "");
    for (const std::string& name : promotions) {
        bool seen = false;
        for (const GdxSlangParameter& p : parameters) {
            if (p.name == name) {
                seen = true;
                break;
            }
        }
        if (!seen) {
            GdxSlangParameter param;
            param.name = name;
            parameters.push_back(std::move(param));
        }
    }
    // Shorthand copies (`float X = global.X;`) became self-assignments when the
    // instance prefix was stripped and now redefine the host parameter of the
    // same name; the reference already resolves, so drop the line. All three
    // rules anchor at column 0 so function-local shadows are left alone.
    for (const GdxSlangParameter& p : parameters) {
        result = std::regex_replace(
            result,
            std::regex("\\n(?:float|int|uint)[ \\t]+" + p.name + "[ \\t]*=[ \\t]*" + p.name + "[ \\t]*;[ \\t]*(?=\\n)"),
            "");
    }
    // The same shorthand with an expression (`float X = global.X / 100;`) still
    // redefines the parameter. Rename the local and every later reference so the
    // initializer keeps reading the uniform.
    for (const GdxSlangParameter& p : parameters) {
        const std::regex shorthandRe("\\n(?:float|int|uint)[ \\t]+" + p.name + "[ \\t]*=[^;]*\\b" + p.name +
                                     "\\b[^;]*;[ \\t]*(?=\\n)");
        std::smatch shorthandMatch;
        if (!std::regex_search(result, shorthandMatch, shorthandRe)) {
            continue;
        }
        const std::string localName = "gdxLocal" + p.name;
        std::string decl = shorthandMatch[0].str();
        const size_t namePos = decl.find(p.name);
        decl.replace(namePos, p.name.size(), localName);
        const size_t declEnd = static_cast<size_t>(shorthandMatch.position() + shorthandMatch.length());
        result = result.substr(0, static_cast<size_t>(shorthandMatch.position())) + decl +
                 std::regex_replace(result.substr(declEnd), std::regex("\\b" + p.name + "\\b"), localName);
    }
    // A global `float X = <constant>;` with X also declared in the stripped
    // block is the shader hardcoding over the uniform (Mega_Bezel disables
    // features per pass this way). The local wins, so retire the parameter.
    std::unordered_set<std::string> retired;
    for (const GdxSlangParameter& p : parameters) {
        const std::regex hardcodeRe("\\n(?:float|int|uint)[ \\t]+" + p.name + "[ \\t]*=[^;]*;[ \\t]*(?=\\n)");
        if (std::regex_search(result, hardcodeRe)) {
            retired.insert(p.name);
        }
    }
    if (!retired.empty()) {
        parameters.erase(std::remove_if(parameters.begin(), parameters.end(),
                                        [&](const GdxSlangParameter& p) { return retired.count(p.name) > 0; }),
                         parameters.end());
    }
    return result;
}

// crt-geom declares its own `vec4 SourceSize = ...`, remapping the builtin's
// meaning (at global scope, which real slang allows because the builtin lives
// inside a UBO instance). The host maps builtins through #define, which would
// rewrite that declaration into a redefinition of the push-block member.
// Rename the shadow and its uses: to the end of the enclosing block for a
// function-local, to end of file for a global (globals are only visible below
// their declaration).
static std::string GdxRenameLocalBuiltinShadows(const std::string& source) {
    static const char* names[] = { "SourceSize", "OriginalSize", "OutputSize", "FinalViewportSize",
                                   "Source",     "Original",     "FrameCount", "FrameDirection",
                                   "MVP",        "vTexCoord",    "TexCoord",   "FragColor" };
    std::string result = source;
    std::sregex_iterator end;
    for (const char* nameC : names) {
        const std::string name(nameC);
        const std::regex declRe("(?:^|\\n)[ \\t]*(?:vec[234]|float|int|uint|mat[34])[ \\t]+" + name + "[ \\t]*=");
        std::vector<size_t> declPositions;
        for (std::sregex_iterator it(result.begin(), result.end(), declRe); it != end; ++it) {
            declPositions.push_back(static_cast<size_t>(it->position()));
        }
        // Replace from the last declaration backwards so earlier positions stay valid.
        for (auto rit = declPositions.rbegin(); rit != declPositions.rend(); ++rit) {
            const size_t declPos = *rit;
            int depthAtDecl = 0;
            for (size_t i = 0; i < declPos; ++i) {
                if (result[i] == '{') {
                    ++depthAtDecl;
                } else if (result[i] == '}') {
                    --depthAtDecl;
                }
            }
            // The declarator itself is renamed, but its initializer still binds to the
            // OUTER meaning (hsm-ntsc-pass1: `float OriginalSize = OriginalSize.x;` reads
            // the builtin). Renaming starts at the next statement, not at the name.
            const size_t namePos = result.find(name, declPos + 1);
            const size_t initEnd = result.find(';', namePos);
            if (namePos == std::string::npos || initEnd == std::string::npos) {
                continue;
            }
            result.replace(namePos, name.size(), "gdxLocal" + name);
            // Qualified references (`params.OriginalSize`) name the builtin
            // even with the shadow in scope; only unqualified uses rename. The
            // pass runs before uniform-block stripping, while the qualifier is
            // still present (guest-ntsc reads `params.OriginalSize.xy` next to
            // a local `float OriginalSize`).
            const auto renameUses = [&name](const std::string& text) {
                return std::regex_replace(text, std::regex("(^|[^.A-Za-z0-9_])" + name + "\\b"),
                                          "$1gdxLocal" + name);
            };
            if (depthAtDecl <= 0) {
                // crt-geom shadows at GLOBAL scope (legal in slang, where the
                // builtin lives inside a UBO instance). The global is visible
                // from its declaration to end of file.
                result.replace(initEnd, std::string::npos, renameUses(result.substr(initEnd)));
                continue;
            }
            size_t scopeEnd = result.size();
            int depth = depthAtDecl;
            for (size_t i = declPos; i < result.size(); ++i) {
                if (result[i] == '{') {
                    ++depth;
                } else if (result[i] == '}') {
                    --depth;
                    if (depth < depthAtDecl) {
                        scopeEnd = i;
                        break;
                    }
                }
            }
            result.replace(initEnd, scopeEnd - initEnd, renameUses(result.substr(initEnd, scopeEnd - initEnd)));
        }
    }
    return result;
}

// crt-yah's ntsc passes declare a genuine stage varying named FrameCount
// (`flat out uint FrameCount;` written by the vertex, read by the fragment)
// alongside the builtin of the same name (reached through `global.FrameCount`).
// The host macro-maps builtins, which would rewrite the varying declaration
// into a redefinition of the push-block member. Rename varyings that collide
// with a host-mapped builtin; stage interfaces match by location in SPIR-V, so
// the new name is safe. Runs before block stripping so qualified `global.X`
// references keep pointing at the builtin.
static std::string GdxRenameBuiltinCollidingVaryings(const std::string& source, bool vertexStage) {
    static const std::unordered_set<std::string> hostBuiltins = {
        "SourceSize", "OriginalSize", "OutputSize", "FinalViewportSize",
        "FrameCount", "FrameDirection", "MVP", "vTexCoord", "TexCoord", "FragColor", "Position",
    };
    static const std::regex varyingDeclRe(
        R"(layout\s*\(\s*location\s*=\s*(\d+)\s*\)\s*(?:(?:noperspective|flat|smooth|centroid|sample|invariant)\s+)*(in|out)\s+\w+\s+(\w+)\s*;)");
    std::unordered_set<std::string> renames;
    std::sregex_iterator end;
    for (auto it = std::sregex_iterator(source.begin(), source.end(), varyingDeclRe); it != end; ++it) {
        const std::string location = (*it)[1].str();
        const std::string direction = (*it)[2].str();
        const std::string name = (*it)[3].str();
        // Location 0 is the host vUV/gdxFragColor contract, remapped separately.
        if (location == "0" || hostBuiltins.count(name) == 0) {
            continue;
        }
        // Vertex inputs are synthesized from gl_VertexIndex, never user data.
        if (vertexStage && direction == "in") {
            continue;
        }
        renames.insert(name);
    }
    std::string result = source;
    if (renames.empty()) {
        return result;
    }
    // Uniform-block bodies are off limits: a `uint FrameCount;` member there is
    // the builtin itself (stripped later), not the varying.
    std::vector<std::pair<size_t, size_t>> blockSpans;
    static const std::regex uniformBlockRe(R"((?:layout\s*\([^)]*\)\s*)?uniform\s+\w+\s*\{[^{}]*\}\s*\w*\s*;)");
    for (auto it = std::sregex_iterator(source.begin(), source.end(), uniformBlockRe); it != end; ++it) {
        blockSpans.emplace_back(static_cast<size_t>(it->position()),
                                static_cast<size_t>(it->position() + it->length()));
    }
    auto insideBlock = [&blockSpans](size_t pos) {
        for (const auto& span : blockSpans) {
            if (pos >= span.first && pos < span.second) {
                return true;
            }
        }
        return false;
    };
    for (const std::string& name : renames) {
        // Unqualified uses only: `global.FrameCount` keeps naming the builtin.
        const std::regex useRe("(^|[^.A-Za-z0-9_])" + name + "\\b");
        std::string rebuilt;
        rebuilt.reserve(result.size());
        size_t cursor = 0;
        for (auto it = std::sregex_iterator(result.begin(), result.end(), useRe); it != end; ++it) {
            const size_t pos = static_cast<size_t>(it->position());
            if (insideBlock(pos)) {
                continue;
            }
            rebuilt += result.substr(cursor, pos - cursor);
            rebuilt += (*it)[1].str();
            rebuilt += "gdxVar" + name;
            cursor = pos + static_cast<size_t>(it->length());
        }
        rebuilt += result.substr(cursor);
        result = std::move(rebuilt);
    }
    return result;
}

// Build a Vulkan GLSL fragment shader that re-uses the host backend contract.
// We inject declarations for the names our backends already set up, then use
// macros to map slang builtins to those names. Slang's own declarations of the
// same builtins are stripped so the result compiles without redefinition.
std::string GdxBuildHostFragment(const std::string& slangFragment,
                                 const std::vector<GdxSlangParameter>& parameters,
                                 const GdxSlangUsedBuiltins& used) {
    // The host occupies location 0 in both directions (vUV in, gdxFragColor out). A pass
    // that declares its own location-0 varying under a name the builtin list does not know
    // (e.g. blur9fast's `layout(location = 0) in vec2 tex_uv;`) would collide with ours;
    // remap the name onto the host varying via #define and drop the declaration. Inputs at
    // other locations are vertex-stage varyings and pass through untouched.
    static const std::regex locInRe(
        R"(^\s*layout\s*\(\s*location\s*=\s*(\d+)\s*\)\s*((?:(?:noperspective|flat|smooth|centroid|sample|invariant)\s+)*)in\s+\w+\s+(\w+)\s*;)");
    static const std::regex locOutRe(
        R"(^\s*layout\s*\(\s*location\s*=\s*(\d+)\s*\)\s*((?:(?:noperspective|flat|smooth|centroid|sample|invariant)\s+)*)out\s+\w+\s+(\w+)\s*;)");

    // Interpolation qualifiers on the pass's location-0 varying (koko-aio's
    // `layout(location = 0) noperspective in vec2 vTexCoord;`) ride along onto the
    // host declaration, or the stage interface changes meaning.
    std::string loc0InQuals;
    std::string loc0InName;
    std::string loc0OutName;
    {
        std::istringstream scan(slangFragment);
        std::string scanLine;
        while (std::getline(scan, scanLine)) {
            const std::string trimmed = GdxTrim(scanLine);
            std::smatch m;
            if (std::regex_match(trimmed, m, locInRe) && m[1].str() == "0") {
                loc0InQuals = m[2].str();
                loc0InName = m[3].str();
            } else if (std::regex_match(trimmed, m, locOutRe) && m[1].str() == "0") {
                loc0OutName = m[3].str();
            }
        }
    }

    // A shader's own `#define <builtin>` shadows the host mapping (same as in real
    // slang); emitting ours too is a hard glslang redefinition error.
    std::unordered_set<std::string> shaderDefines;
    {
        static const std::regex selfDefineRe(R"(#define\s+(\w+))");
        std::sregex_iterator dend;
        for (auto it = std::sregex_iterator(slangFragment.begin(), slangFragment.end(), selfDefineRe); it != dend; ++it) {
            shaderDefines.insert((*it)[1].str());
        }
    }

    std::ostringstream host;
    auto emitDefine = [&host, &shaderDefines](const std::string& name, const std::string& expansion) {
        if (shaderDefines.count(name) == 0) {
            host << "#define " << name << " " << expansion << "\n";
        }
    };

    host << "#version 450\n";
    host << "layout(location = 0) " << loc0InQuals << "in vec2 vUV;\n";
    host << "layout(location = 0) out vec4 gdxFragColor;\n";
    host << "layout(set = 0, binding = 0) uniform sampler2D uTex;\n";
    // Sizes are vec4 with the slang convention: xy = size, zw = 1/size.
    host << "layout(push_constant) uniform GdxPush {\n";
    host << "    vec4 uSrcSize;\n";
    host << "    vec4 uOutSize;\n";
    host << "    uint gdxFrameCount;\n";
    host << "    mat4 gdxMvp;\n";
    for (const std::string& sizeName : used.extraSizeNames) {
        bool isParam = false;
        for (const auto& p : parameters) {
            if (p.name == sizeName) {
                isParam = true;
                break;
            }
        }
        if (!isParam) {
            host << "    vec4 " << sizeName << ";\n";
        }
    }
    for (const auto& p : parameters) {
        host << "    float " << p.name << ";\n";
    }
    host << "};\n";

    // Macros map the slang names the shader actually uses to the host names.
    // Size macros stay bare identifiers: HSM declares function parameters
    // named Source/SourceSize, and a parenthesized expansion breaks the
    // parameter list. A bare global name just gets shadowed, which is the
    // intended semantics for those helpers.
    emitDefine("Source", "uTex");
    emitDefine("Original", "uTex");
    emitDefine("SourceSize", "uSrcSize");
    emitDefine("OriginalSize", "uSrcSize");
    emitDefine("OutputSize", "uOutSize");
    emitDefine("FinalViewportSize", "uOutSize");
    emitDefine("FrameCount", "gdxFrameCount");
    // FrameDirection is 1 everywhere but RetroArch's rewind; a literal avoids
    // growing the push-constant layout for a value that never changes here.
    emitDefine("FrameDirection", "1");
    emitDefine("MVP", "gdxMvp");
    emitDefine("vTexCoord", "vUV");
    emitDefine("TexCoord", "vUV");
    emitDefine("FragColor", "gdxFragColor");

    // Declare only the extra samplers the shader actually references. Each
    // family uses a fixed binding base so GL and D3D11 can bind by convention.
    for (int n = 0; n <= used.maxOriginalHistory; ++n) {
        const int binding = GdxSlangTextureBindings::OriginalHistory(n);
        host << "layout(set = 0, binding = " << binding << ") uniform sampler2D gdxOriginalHistory" << n << ";\n";
        emitDefine("OriginalHistory" + std::to_string(n), "gdxOriginalHistory" + std::to_string(n));
    }
    for (int n : used.passOutputIndices) {
        const int binding = GdxSlangTextureBindings::PassOutput(n);
        host << "layout(set = 0, binding = " << binding << ") uniform sampler2D gdxPassOutput" << n << ";\n";
        emitDefine("PassOutput" + std::to_string(n), "gdxPassOutput" + std::to_string(n));
    }
    for (int n : used.passFeedbackIndices) {
        const int binding = GdxSlangTextureBindings::PassFeedback(n);
        host << "layout(set = 0, binding = " << binding << ") uniform sampler2D gdxPassFeedback" << n << ";\n";
        emitDefine("PassFeedback" + std::to_string(n), "gdxPassFeedback" + std::to_string(n));
    }
    for (int n : used.userTextureIndices) {
        const int binding = GdxSlangTextureBindings::User(n);
        host << "layout(set = 0, binding = " << binding << ") uniform sampler2D gdxUser" << n << ";\n";
        emitDefine("User" + std::to_string(n), "gdxUser" + std::to_string(n));
    }
    for (size_t i = 0; i < used.namedSamplers.size(); ++i) {
        const int binding = GdxSlangTextureBindings::Named(static_cast<int>(i));
        host << "layout(set = 0, binding = " << binding << ") uniform sampler2D gdxNamed" << i << ";\n";
        emitDefine(used.namedSamplers[i], "gdxNamed" + std::to_string(i));
    }

    if (!loc0InName.empty()) {
        emitDefine(loc0InName, "vUV");
    }
    if (!loc0OutName.empty()) {
        emitDefine(loc0OutName, "gdxFragColor");
    }

    std::istringstream stream(slangFragment);
    std::string line;
    while (std::getline(stream, line)) {
        std::string trimmed = GdxTrim(line);
        if (trimmed.rfind("#version", 0) == 0) {
            continue;
        }
        if (trimmed.rfind("#pragma", 0) == 0) {
            continue;
        }
        // Only location-0 declarations are dropped (remapped above); varyings at
        // other locations come from the pass's own vertex stage and must stay.
        std::smatch locMatch;
        if ((std::regex_match(trimmed, locMatch, locInRe) || std::regex_match(trimmed, locMatch, locOutRe)) &&
            locMatch[1].str() == "0") {
            continue;
        }
        if (GdxLooksLikeBuiltinDeclaration(trimmed)) {
            continue;
        }
        // Named samplers were remapped onto gdxNamedN above; drop the native
        // declaration so its (meaningless to us) binding does not linger.
        static const std::regex namedSamplerDeclRe(R"(^\s*(?:layout\s*\([^)]*\)\s*)?uniform\s+sampler2D\s+(\w+)\s*;)");
        std::smatch samplerMatch;
        if (std::regex_match(trimmed, samplerMatch, namedSamplerDeclRe)) {
            const std::string samplerName = samplerMatch[1].str();
            bool isNamed = false;
            for (const std::string& named : used.namedSamplers) {
                if (named == samplerName) {
                    isNamed = true;
                    break;
                }
            }
            if (isNamed) {
                continue;
            }
        }
        host << line << "\n";
    }
    return host.str();
}

// Vertex-stage counterpart of GdxBuildHostFragment. The pass loop draws a
// fullscreen triangle with no vertex buffers, so the slang Position/TexCoord
// stage inputs are synthesized from gl_VertexIndex and macro-mapped to globals;
// the user's main is renamed and driven by a host main. The user's location-0
// output is macro-mapped to the host vUV so the fragment-side contract is
// identical to the no-vertex path; other outputs (computed varyings such as
// scanline.slang's omega) pass through by location.
std::string GdxBuildHostVertex(const std::string& slangVertex, const std::vector<GdxSlangParameter>& parameters,
                               const GdxSlangUsedBuiltins& used) {
    static const std::regex stageInRe(
        R"(^\s*layout\s*\([^)]*\)\s*(?:(?:noperspective|flat|smooth|centroid|sample|invariant)\s+)*in\s+vec[234]\s+\w+\s*;)");
    static const std::regex loc0OutRe(
        R"(^\s*layout\s*\(\s*location\s*=\s*0\s*\)\s*((?:(?:noperspective|flat|smooth|centroid|sample|invariant)\s+)*)out\s+vec[234]\s+(\w+)\s*;)");

    // First pass: find the name of the user's location-0 output (the texcoord
    // varying by slang convention) so the macro is defined before any use, and
    // carry its interpolation qualifiers (koko-aio's noperspective) onto the
    // host declaration to keep the stage interface unchanged.
    std::string loc0OutName;
    std::string loc0OutQuals;
    std::istringstream scanStream(slangVertex);
    std::string scanLine;
    while (std::getline(scanStream, scanLine)) {
        std::smatch outMatch;
        if (std::regex_search(scanLine, outMatch, loc0OutRe)) {
            loc0OutQuals = outMatch[1].str();
            loc0OutName = outMatch[2].str();
            break;
        }
    }

    // A shader's own `#define <builtin>` shadows the host mapping (same as in real
    // slang); emitting ours too is a hard glslang redefinition error.
    std::unordered_set<std::string> shaderDefines;
    {
        static const std::regex selfDefineRe(R"(#define\s+(\w+))");
        std::sregex_iterator dend;
        for (auto it = std::sregex_iterator(slangVertex.begin(), slangVertex.end(), selfDefineRe); it != dend; ++it) {
            shaderDefines.insert((*it)[1].str());
        }
    }

    std::ostringstream host;
    auto emitDefine = [&host, &shaderDefines](const std::string& name, const std::string& expansion) {
        if (shaderDefines.count(name) == 0) {
            host << "#define " << name << " " << expansion << "\n";
        }
    };

    host << "#version 450\n";
    host << "layout(push_constant) uniform GdxPush {\n";
    host << "    vec4 uSrcSize;\n";
    host << "    vec4 uOutSize;\n";
    host << "    uint gdxFrameCount;\n";
    host << "    mat4 gdxMvp;\n";
    for (const std::string& sizeName : used.extraSizeNames) {
        bool isParam = false;
        for (const auto& p : parameters) {
            if (p.name == sizeName) {
                isParam = true;
                break;
            }
        }
        if (!isParam) {
            host << "    vec4 " << sizeName << ";\n";
        }
    }
    for (const auto& p : parameters) {
        host << "    float " << p.name << ";\n";
    }
    host << "};\n";
    emitDefine("SourceSize", "uSrcSize");
    emitDefine("OriginalSize", "uSrcSize");
    emitDefine("OutputSize", "uOutSize");
    emitDefine("FinalViewportSize", "uOutSize");
    emitDefine("FrameCount", "gdxFrameCount");
    // FrameDirection is 1 everywhere but RetroArch's rewind; a literal avoids
    // growing the push-constant layout for a value that never changes here.
    emitDefine("FrameDirection", "1");
    emitDefine("MVP", "gdxMvp");
    // slang convention puts the position input at location 0 and the texcoord
    // input at location 1, but the names are the shader's choice (crt-yah uses
    // `Coord`). Map the declared names onto the synthesized globals.
    std::string posInName = "Position";
    std::string texInName = "TexCoord";
    {
        static const std::regex stageInNameRe(
            R"(^\s*layout\s*\(\s*location\s*=\s*(\d+)\s*\)\s*(?:(?:noperspective|flat|smooth|centroid|sample|invariant)\s+)*in\s+vec[234]\s+(\w+)\s*;)");
        std::istringstream inScan(slangVertex);
        std::string inLine;
        while (std::getline(inScan, inLine)) {
            std::smatch inMatch;
            if (std::regex_search(inLine, inMatch, stageInNameRe)) {
                if (inMatch[1].str() == "0") {
                    posInName = inMatch[2].str();
                } else if (inMatch[1].str() == "1") {
                    texInName = inMatch[2].str();
                }
            }
        }
    }
    // The loc0-output define below must win when the shader names its texcoord
    // varying after a builtin (crt-yah's `out vec2 TexCoord;`): emitting both
    // is a glslang macro-redefinition error with different substitutions.
    if (loc0OutName != posInName) {
        emitDefine(posInName, "gdxPosition");
    }
    if (texInName != posInName && loc0OutName != texInName) {
        emitDefine(texInName, "gdxTexCoord");
    }
    if (!loc0OutName.empty()) {
        emitDefine(loc0OutName, "vUV");
    }
    host << "layout(location = 0) " << loc0OutQuals << "out vec2 vUV;\n";
    host << "vec4 gdxPosition;\n";
    host << "vec2 gdxTexCoord;\n";

    std::string body;
    std::istringstream stream(slangVertex);
    std::string line;
    while (std::getline(stream, line)) {
        std::string trimmed = GdxTrim(line);
        if (trimmed.rfind("#version", 0) == 0 || trimmed.rfind("#pragma", 0) == 0) {
            continue;
        }
        if (std::regex_search(line, stageInRe) || std::regex_search(line, loc0OutRe)) {
            continue;
        }
        body += line + "\n";
    }
    body = std::regex_replace(body, std::regex(R"(\bvoid\s+main\s*\()"), "void gdxUserVertexMain(");
    host << body;

    host << "void main() {\n";
    host << "    vec2 gdxP = vec2(gl_VertexIndex == 1 ? 3.0 : -1.0, gl_VertexIndex == 2 ? 3.0 : -1.0);\n";
    host << "    gdxPosition = vec4(gdxP, 0.0, 1.0);\n";
    host << "    gdxTexCoord = gdxP * 0.5 + 0.5;\n";
    host << "    gdxUserVertexMain();\n";
    host << "}\n";
    return host.str();
}

std::vector<uint32_t> GdxCompileToSpv(const std::string& source, EShLanguage stage, std::string* error) {
    static bool initialized = [] {
        glslang::InitializeProcess();
        return true;
    }();
    (void)initialized;

    const char* strings[] = { source.c_str() };
    glslang::TShader shader(stage);
    shader.setStrings(strings, 1);
    shader.setEnvInput(glslang::EShSourceGlsl, stage, glslang::EShClientVulkan, 450);
    shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_0);
    shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_0);

    TBuiltInResource resources = *GetDefaultResources();
    if (!shader.parse(&resources, 450, false, EShMsgDefault)) {
        *error = std::string("glslang parse: ") + shader.getInfoLog();
        return {};
    }

    glslang::TProgram program;
    program.addShader(&shader);
    if (!program.link(EShMsgDefault)) {
        *error = std::string("glslang link: ") + program.getInfoLog();
        return {};
    }

    std::vector<uint32_t> spv;
    glslang::SpvOptions spvOptions;
    spvOptions.generateDebugInfo = false;
    spvOptions.optimizeSize = false;
    spvOptions.disableOptimizer = true;
    glslang::GlslangToSpv(*program.getIntermediate(stage), spv, &spvOptions);
    if (spv.empty()) {
        *error = "glslang produced empty SPIR-V";
        return {};
    }
    return spv;
}

std::string GdxCrossCompileGlsl(const std::vector<uint32_t>& spv, std::string* error) {
    try {
        spirv_cross::CompilerGLSL compiler(spv);
        spirv_cross::CompilerGLSL::Options options;
        options.version = 330;
        options.es = false;
        options.enable_420pack_extension = false;
        compiler.set_common_options(options);
        return compiler.compile();
    } catch (const std::exception& e) {
        *error = std::string("SPIRV-Cross GLSL: ") + e.what();
        return "";
    }
}

std::string GdxCrossCompileHlsl(const std::vector<uint32_t>& spv, std::string* error) {
    try {
        spirv_cross::CompilerHLSL compiler(spv);
        spirv_cross::CompilerHLSL::Options options;
        options.shader_model = 50;
        options.point_size_compat = true;
        compiler.set_hlsl_options(options);
        return compiler.compile();
    } catch (const std::exception& e) {
        *error = std::string("SPIRV-Cross HLSL: ") + e.what();
        return "";
    }
}

// SPIRV-Cross GLSL lowers the push_constant block to a plain struct plus a
// uniform instance (`uniform GdxPush _18;`) and qualifies every member access
// with the instance name. Flatten it to standalone uniforms so the existing
// backend uniform lookups (uSrcSize, gdxFrameCount, parameters by name, ...)
// resolve against the compiled program. Used for both shader stages.
std::string GdxFinalizeGlslFragment(std::string source, const std::vector<GdxSlangParameter>& parameters,
                                    const GdxSlangUsedBuiltins& used) {
    std::string decls = "uniform vec4 uSrcSize;\n"
                        "uniform vec4 uOutSize;\n"
                        "uniform uint gdxFrameCount;\n"
                        "uniform mat4 gdxMvp;\n";
    for (const std::string& sizeName : used.extraSizeNames) {
        bool isParam = false;
        for (const GdxSlangParameter& p : parameters) {
            if (p.name == sizeName) {
                isParam = true;
                break;
            }
        }
        if (!isParam) {
            decls += "uniform vec4 " + sizeName + ";\n";
        }
    }
    for (const GdxSlangParameter& p : parameters) {
        decls += "uniform float " + p.name + ";\n";
    }

    std::smatch match;
    static const std::regex instanceRe("uniform\\s+GdxPush\\s+(\\w+)\\s*;");
    if (std::regex_search(source, match, instanceRe)) {
        const std::string instance = match[1].str();
        source = std::regex_replace(source, instanceRe, decls);
        source = std::regex_replace(source, std::regex("\\b" + instance + "\\."), "");
    }
    return source;
}

// SPIRV-Cross HLSL emits a cbuffer from the push_constant block. Replace it
// with a fixed-layout cbuffer using explicit packoffset so the CPU side can
// fill constants without reflecting the compiled blob. Sizes follow the slang
// vec4 convention (xy = size, zw = 1/size); c0/c1 are 16 bytes either way, so
// the layout is unchanged from the vec2 era.
std::string GdxReplaceHlslCBuffer(std::string source, const std::vector<GdxSlangParameter>& parameters,
                                  const GdxSlangUsedBuiltins& used) {
    std::string newCbuffer =
        "cbuffer GdxPostCB : register(b0) {\n"
        "    float4 uSrcSize : packoffset(c0);\n"
        "    float4 uOutSize : packoffset(c1);\n"
        "    uint gdxFrameCount : packoffset(c2.x);\n"
        "    float3 _gdxPad0 : packoffset(c2.y);\n"
        "    float4x4 gdxMvp : packoffset(c3);\n";
    // Extra per-pass texture sizes sit at c7..c(7+N), parameters follow after.
    int reg = 7;
    for (const std::string& sizeName : used.extraSizeNames) {
        bool isParam = false;
        for (const GdxSlangParameter& p : parameters) {
            if (p.name == sizeName) {
                isParam = true;
                break;
            }
        }
        if (!isParam) {
            newCbuffer += "    float4 " + sizeName + " : packoffset(c" + std::to_string(reg) + ");\n";
            ++reg;
        }
    }
    int comp = 0;
    for (const GdxSlangParameter& p : parameters) {
        newCbuffer += "    float " + p.name + " : packoffset(c" + std::to_string(reg) + ".";
        switch (comp) {
            case 0:
                newCbuffer += "x";
                break;
            case 1:
                newCbuffer += "y";
                break;
            case 2:
                newCbuffer += "z";
                break;
            default:
                newCbuffer += "w";
                break;
        }
        newCbuffer += ");\n";
        if (++comp == 4) {
            comp = 0;
            ++reg;
        }
    }
    newCbuffer += "};\n";

    // SPIRV-Cross renames each cbuffer member to "<varid>_<name>" (e.g.
    // _28_uSrcSize) and uses those names at every access. Recover the prefix
    // from a builtin or parameter reference before replacing the block, then
    // strip it so the body references the flat names declared above.
    std::string prefix;
    static const std::regex builtinPrefixRe("\\b(_\\d+_)(?:uSrcSize|uOutSize|gdxFrameCount|gdxMvp)\\b");
    std::smatch prefixMatch;
    if (std::regex_search(source, prefixMatch, builtinPrefixRe)) {
        prefix = prefixMatch[1].str();
    } else {
        for (const GdxSlangParameter& p : parameters) {
            const std::regex paramPrefixRe("\\b(_\\d+_)" + p.name + "\\b");
            if (std::regex_search(source, prefixMatch, paramPrefixRe)) {
                prefix = prefixMatch[1].str();
                break;
            }
        }
        if (prefix.empty()) {
            for (const std::string& sizeName : used.extraSizeNames) {
                const std::regex sizePrefixRe("\\b(_\\d+_)" + sizeName + "\\b");
                if (std::regex_search(source, prefixMatch, sizePrefixRe)) {
                    prefix = prefixMatch[1].str();
                    break;
                }
            }
        }
    }

    source = std::regex_replace(source, std::regex("cbuffer\\s+GdxPush\\s*\\{[\\s\\S]*?\\}\\s*;"), newCbuffer);
    if (!prefix.empty()) {
        source = std::regex_replace(source, std::regex("\\b" + prefix), "");
    }
    return source;
}

// D3D11 links VS/PS signatures by register, and an SV_Position member declared
// first shifts every TEXCOORD one register up. The pipeline's own vertex shader
// (sGdxPostVsSource) and the translated vertex shaders all lead with
// SV_Position, so the pixel-side input struct must too — otherwise D3D11
// refuses the linkage (debug id 343). Move the SV_Position member to the front
// of the named interface struct, inserting one when absent.
void GdxNormalizeHlslInterfaceStruct(std::string& source, const char* structName) {
    const std::regex structRe("(struct\\s+" + std::string(structName) + "\\s*\\{)([\\s\\S]*?)(\\};)");
    std::smatch structMatch;
    if (!std::regex_search(source, structMatch, structRe)) {
        return;
    }
    const std::string body = structMatch[2].str();
    static const std::regex svPosRe("[^\\n]*\\bSV_Position\\b[^\\n]*\\n?");
    std::smatch svMatch;
    std::string newBody;
    if (std::regex_search(body, svMatch, svPosRe)) {
        newBody = "\n" + svMatch[0].str() + std::regex_replace(body, svPosRe, "");
    } else {
        newBody = "\n    float4 pos : SV_Position;" + body;
    }
    source = structMatch.prefix().str() + structMatch[1].str() + newBody + structMatch[3].str() +
             structMatch.suffix().str();
}

std::string GdxFinalizeHlslFragment(std::string source, const std::vector<GdxSlangParameter>& parameters,
                                    const GdxSlangUsedBuiltins& used) {
    source = GdxReplaceHlslCBuffer(std::move(source), parameters, used);
    // The D3D11 post-shader compiler expects a PSMain entry point; SPIRV-Cross
    // emits main(). `\b` keeps frag_main() untouched ('_' is a word char).
    source = std::regex_replace(source, std::regex("\\bmain\\s*\\(\\s*SPIRV_Cross_Input"), "PSMain(SPIRV_Cross_Input");
    GdxNormalizeHlslInterfaceStruct(source, "SPIRV_Cross_Input");
    return source;
}

std::string GdxFinalizeHlslVertex(std::string source, const std::vector<GdxSlangParameter>& parameters,
                                  const GdxSlangUsedBuiltins& used) {
    source = GdxReplaceHlslCBuffer(std::move(source), parameters, used);
    // The translated vertex stage keeps SPIRV-Cross's void-returning main with
    // whatever signature it emitted (SV_VertexID input when present).
    source = std::regex_replace(source, std::regex("\\bmain\\s*\\("), "VSMain(");
    GdxNormalizeHlslInterfaceStruct(source, "SPIRV_Cross_Output");
    return source;
}

} // namespace

uint32_t GdxSlangHlslCBufferLayout::TotalSize(const std::vector<GdxSlangParameter>& parameters,
                                              size_t extraSizeCount) {
    const uint32_t size =
        ParamsOffset(extraSizeCount) + static_cast<uint32_t>(parameters.size()) * sizeof(float);
    // D3D11 requires constant buffer ByteWidth to be a multiple of 16.
    return (size + 15) & ~15u;
}

void GdxSlangHlslCBufferLayout::Pack(const std::vector<GdxSlangParameter>& parameters,
                                     const std::unordered_map<std::string, float>& overrides, uint32_t frameCount,
                                     uint8_t* outBytes, size_t extraSizeCount) {
    std::memset(outBytes, 0, TotalSize(parameters, extraSizeCount));
    std::memcpy(outBytes + 32, &frameCount, sizeof(frameCount));
    for (int i = 0; i < 4; ++i) {
        float one = 1.0f;
        std::memcpy(outBytes + 48 + (i * 4 + i) * sizeof(float), &one, sizeof(float));
    }
    for (size_t i = 0; i < parameters.size(); ++i) {
        float value = parameters[i].defaultValue;
        auto it = overrides.find(parameters[i].name);
        if (it != overrides.end()) {
            value = it->second;
        }
        std::memcpy(outBytes + ParamsOffset(extraSizeCount) + i * sizeof(float), &value, sizeof(float));
    }
}

GdxSlangTranslation GdxTranslateSlangFile(const std::filesystem::path& path, GdxSlangTarget target) {
    GdxSlangTranslation result;
    std::string text = GdxReadFile(path);
    if (text.empty()) {
        result.error = "Cannot read slang shader: " + path.string();
        return result;
    }

    std::string resolveError;
    std::vector<std::filesystem::path> includeStack;
    if (!GdxResolveSlangIncludes(path, &text, &resolveError, &includeStack)) {
        result.error = resolveError;
        return result;
    }

    GdxSlangFile slang = GdxParseSlangFile(text);
    if (!slang.fragment.present || slang.fragment.source.empty()) {
        result.error = "No fragment stage found in slang shader: " + path.string();
        return result;
    }

    // Real libretro shaders carry their own Push/UBO uniform blocks; fold them
    // into the host contract before building either stage. Shadow renaming runs
    // first, while builtin references are still instance-qualified.
    if (slang.vertex.present) {
        slang.vertex.source = GdxRenameBuiltinCollidingVaryings(slang.vertex.source, true);
        slang.vertex.source = GdxRenameLocalBuiltinShadows(slang.vertex.source);
        slang.vertex.source = GdxStripNativeUniformBlocks(slang.vertex.source, slang.parameters);
    }
    slang.fragment.source = GdxRenameBuiltinCollidingVaryings(slang.fragment.source, false);
    slang.fragment.source = GdxRenameLocalBuiltinShadows(slang.fragment.source);
    slang.fragment.source = GdxStripNativeUniformBlocks(slang.fragment.source, slang.parameters);

    result.usedBuiltins = GdxDetectUsedBuiltins(slang.fragment.source + slang.vertex.source);
    std::string hostFragment = GdxBuildHostFragment(slang.fragment.source, slang.parameters, result.usedBuiltins);

    std::string spvError;
    std::vector<uint32_t> spv = GdxCompileToSpv(hostFragment, EShLangFragment, &spvError);
    if (spv.empty()) {
        // Dump the failing host source too: a parse error is only debuggable against
        // the generated wrapper, not the original .slang file.
        GdxDumpSlangTranslation(path, target, hostFragment);
        result.error = spvError;
        return result;
    }

    std::string crossError;
    if (target == GdxSlangTarget::Glsl330) {
        result.source = GdxCrossCompileGlsl(spv, &crossError);
        if (result.source.empty()) {
            result.error = crossError;
            return result;
        }
        result.source = GdxFinalizeGlslFragment(result.source, slang.parameters, result.usedBuiltins);
    } else {
        result.source = GdxCrossCompileHlsl(spv, &crossError);
        if (result.source.empty()) {
            result.error = crossError;
            return result;
        }
        result.source = GdxFinalizeHlslFragment(result.source, slang.parameters, result.usedBuiltins);
    }

    if (slang.vertex.present && !slang.vertex.source.empty()) {
        const std::string hostVertex = GdxBuildHostVertex(slang.vertex.source, slang.parameters, result.usedBuiltins);
        std::vector<uint32_t> vertexSpv = GdxCompileToSpv(hostVertex, EShLangVertex, &spvError);
        if (vertexSpv.empty()) {
            GdxDumpSlangTranslation(path, target, hostVertex);
            result.error = spvError;
            return result;
        }
        if (target == GdxSlangTarget::Glsl330) {
            result.vertexSource = GdxCrossCompileGlsl(vertexSpv, &crossError);
            if (result.vertexSource.empty()) {
                result.error = crossError;
                return result;
            }
            result.vertexSource = GdxFinalizeGlslFragment(result.vertexSource, slang.parameters, result.usedBuiltins);
        } else {
            result.vertexSource = GdxCrossCompileHlsl(vertexSpv, &crossError);
            if (result.vertexSource.empty()) {
                result.error = crossError;
                return result;
            }
            result.vertexSource = GdxFinalizeHlslVertex(result.vertexSource, slang.parameters, result.usedBuiltins);
        }
        result.hasVertexStage = true;
    }

    result.parameters = slang.parameters;
    result.ok = true;
    return result;
}

std::vector<GdxSlangParameter> GdxParseSlangParameters(const std::filesystem::path& path) {
    std::vector<GdxSlangParameter> result;
    std::string text = GdxReadFile(path);
    if (text.empty()) {
        return result;
    }

    std::string resolveError;
    std::vector<std::filesystem::path> includeStack;
    if (!GdxResolveSlangIncludes(path, &text, &resolveError, &includeStack)) {
        return result;
    }

    GdxSlangFile slang = GdxParseSlangFile(text);
    return slang.parameters;
}

void GdxDumpSlangTranslation(const std::filesystem::path& originalPath, GdxSlangTarget target,
                             const std::string& translatedSource) {
    const char* env = std::getenv("GDX_DUMP_SLANG_TRANSLATION");
    if (env == nullptr || env[0] == '\0') {
        return;
    }
    std::string stem = originalPath.stem().string();
    std::string backend = target == GdxSlangTarget::Glsl330 ? "glsl" : "hlsl";
    std::filesystem::path outDir = std::filesystem::path(Ship::Context::GetAppDirectoryPath()) / "logs";
    std::error_code ec;
    std::filesystem::create_directories(outDir, ec);
    std::filesystem::path outPath = outDir / ("slang-" + stem + "-" + backend + ".txt");
    std::ofstream file(outPath, std::ios::binary);
    if (file) {
        file << translatedSource;
        SPDLOG_INFO("[slang] dumped translation to {}", outPath.string());
    }
}

} // namespace Fast
