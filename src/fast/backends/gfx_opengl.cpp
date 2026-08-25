#include "ship/window/Window.h"
#ifdef ENABLE_OPENGL

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <cstring>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <map>
#include <regex>
#include <sstream>
#include <thread>
#include <unordered_map>

#ifndef _LANGUAGE_C
#define _LANGUAGE_C
#endif

#ifdef __MINGW32__
#define FOR_WINDOWS 1
#else
#define FOR_WINDOWS 0
#endif

#include "fast/backends/gfx_opengl.h"
#include "fast/backends/gfx_post_shader_pipeline.h"
#include "ship/window/gui/Gui.h"
#include <stb_image.h>
#include <prism/processor.h>
#include <fstream>
#include "ship/Context.h"
#include "ship/resource/factory/ShaderFactory.h"
#include "fast/interpreter.h"
#include "spdlog/spdlog.h"
#include <spdlog/fmt/fmt.h>
#include "ship/config/ConsoleVariable.h"
#include "libultraship/bridge/consolevariablebridge.h"

namespace Fast {
int GfxRenderingAPIOGL::GetMaxTextureSize() {
    GLint max_texture_size;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &max_texture_size);
    return max_texture_size;
}

const char* GfxRenderingAPIOGL::GetName() {
    return "OpenGL";
}

GfxClipParameters GfxRenderingAPIOGL::GetClipParameters() {
    return { false, mFrameBuffers[mCurrentFrameBuffer].invertY };
}

static void VertexArraySetAttribs(ShaderProgram* prg) {
    size_t numFloats = prg->numFloats;
    size_t pos = 0;

    for (int i = 0; i < prg->numAttribs; i++) {
        if (prg->attribLocations[i] >= 0) {
            glEnableVertexAttribArray(prg->attribLocations[i]);
            glVertexAttribPointer(prg->attribLocations[i], prg->attribSizes[i], GL_FLOAT, GL_FALSE,
                                  numFloats * sizeof(float), (void*)(pos * sizeof(float)));
        }
        pos += prg->attribSizes[i];
    }
}

void GfxRenderingAPIOGL::SetUniforms(ShaderProgram* prg) const {
    glUniform1i(prg->frameCountLocation, mFrameCount);
    glUniform1f(prg->noiseScaleLocation, mCurrentNoiseScale);
}

void GfxRenderingAPIOGL::SetPerDrawUniforms() {
    glUniform1f(mCurrentShaderProgram->prim_depth_location, mCurrentPrimDepth);
    glUniform1f(mCurrentShaderProgram->alpha_compare_threshold_location, mCurrentAlphaCompareThreshold);

    if (mCurrentShaderProgram->usedTextures[0] || mCurrentShaderProgram->usedTextures[1]) {
        // One element at a time: a two-element glUniform1iv is rejected in full when the
        // driver trimmed the array. See the ShaderProgram header comment.
        for (int i = 0; i < 2; i++) {
            // The loop covers both slots whenever either is used, so slot 1 is indexed even for
            // a single-texture material and can still hold an id from an earlier draw. Stale
            // rather than absent, but stale is enough to index past the end. D3D11 guards the
            // same way.
            if (mCurrentTextureIds[i] >= textures.size()) {
                continue;
            }
            glUniform1i(mCurrentShaderProgram->texture_filtering_locations[i],
                        textures[mCurrentTextureIds[i]].filtering);
            glUniform1i(mCurrentShaderProgram->texture_width_locations[i], textures[mCurrentTextureIds[i]].width);
            glUniform1i(mCurrentShaderProgram->texture_height_locations[i], textures[mCurrentTextureIds[i]].height);
        }
    }
}

void GfxRenderingAPIOGL::UnloadShader(ShaderProgram* old_prg) {
    if (old_prg != nullptr && old_prg == mLastLoadedShader) {
        for (unsigned int i = 0; i < old_prg->numAttribs; i++) {
            if (old_prg->attribLocations[i] >= 0) {
                glDisableVertexAttribArray(old_prg->attribLocations[i]);
            }
        }
        mLastLoadedShader = nullptr;
    }
}

void GfxRenderingAPIOGL::LoadShader(ShaderProgram* new_prg) {
    // if (!new_prg) return;
    mCurrentShaderProgram = new_prg;
    if (new_prg != mLastLoadedShader) {
        glUseProgram(new_prg->openglProgramId);
        VertexArraySetAttribs(new_prg);
        mLastLoadedShader = new_prg;
    }
    SetUniforms(new_prg);
}

#define RAND_NOISE "((random(vec3(floor(gl_FragCoord.xy * noise_scale), float(frame_count))) + 1.0) / 2.0)"

static const char* shader_item_to_str(uint32_t item, bool with_alpha, bool only_alpha, bool inputs_have_alpha,
                                      bool first_cycle, bool hint_single_element) {
    if (!only_alpha) {
        switch (item) {
            // The default is load-bearing: only SHADER_INPUT_1..4 are spelled out below, but
            // gfx_cc_get_features derives numInputs from slots as high as SHADER_INPUT_7, so the
            // gap is reachable. Falling through to the `return ""` at the tail splices an empty
            // string into the GLSL, which fails to compile and hits the abort() in
            // CreateAndLoadNewShader -- a killed process for a wrong-looking material. D3D11
            // folds unknown values into SHADER_0 the same way.
            default:
            case SHADER_0:
                return with_alpha ? "vec4(0.0, 0.0, 0.0, 0.0)" : "vec3(0.0, 0.0, 0.0)";
            case SHADER_1:
                return with_alpha ? "vec4(1.0, 1.0, 1.0, 1.0)" : "vec3(1.0, 1.0, 1.0)";
            case SHADER_INPUT_1:
                return with_alpha || !inputs_have_alpha ? "vInput1" : "vInput1.rgb";
            case SHADER_INPUT_2:
                return with_alpha || !inputs_have_alpha ? "vInput2" : "vInput2.rgb";
            case SHADER_INPUT_3:
                return with_alpha || !inputs_have_alpha ? "vInput3" : "vInput3.rgb";
            case SHADER_INPUT_4:
                return with_alpha || !inputs_have_alpha ? "vInput4" : "vInput4.rgb";
            case SHADER_TEXEL0:
                return first_cycle ? (with_alpha ? "texVal0" : "texVal0.rgb")
                                   : (with_alpha ? "texVal1" : "texVal1.rgb");
            case SHADER_TEXEL0A:
                return first_cycle
                           ? (hint_single_element ? "texVal0.a"
                                                  : (with_alpha ? "vec4(texVal0.a, texVal0.a, texVal0.a, texVal0.a)"
                                                                : "vec3(texVal0.a, texVal0.a, texVal0.a)"))
                           : (hint_single_element ? "texVal1.a"
                                                  : (with_alpha ? "vec4(texVal1.a, texVal1.a, texVal1.a, texVal1.a)"
                                                                : "vec3(texVal1.a, texVal1.a, texVal1.a)"));
            case SHADER_TEXEL1A:
                return first_cycle
                           ? (hint_single_element ? "texVal1.a"
                                                  : (with_alpha ? "vec4(texVal1.a, texVal1.a, texVal1.a, texVal1.a)"
                                                                : "vec3(texVal1.a, texVal1.a, texVal1.a)"))
                           : (hint_single_element ? "texVal0.a"
                                                  : (with_alpha ? "vec4(texVal0.a, texVal0.a, texVal0.a, texVal0.a)"
                                                                : "vec3(texVal0.a, texVal0.a, texVal0.a)"));
            case SHADER_TEXEL1:
                return first_cycle ? (with_alpha ? "texVal1" : "texVal1.rgb")
                                   : (with_alpha ? "texVal0" : "texVal0.rgb");
            case SHADER_COMBINED:
                return with_alpha ? "texel" : "texel.rgb";
            case SHADER_NOISE:
                return with_alpha ? "vec4(" RAND_NOISE ", " RAND_NOISE ", " RAND_NOISE ", " RAND_NOISE ")"
                                  : "vec3(" RAND_NOISE ", " RAND_NOISE ", " RAND_NOISE ")";
        }
    } else {
        switch (item) {
            // Same reachable gap as the colour switch above.
            default:
            case SHADER_0:
                return "0.0";
            case SHADER_1:
                return "1.0";
            case SHADER_INPUT_1:
                return "vInput1.a";
            case SHADER_INPUT_2:
                return "vInput2.a";
            case SHADER_INPUT_3:
                return "vInput3.a";
            case SHADER_INPUT_4:
                return "vInput4.a";
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
    return "";
}

bool get_bool(prism::ContextTypes* value) {
    if (std::holds_alternative<int>(*value)) {
        return std::get<int>(*value) == 1;
    }
    return false;
}

prism::ContextTypes* append_formula(prism::ContextTypes* _, prism::ContextTypes* a_arg, prism::ContextTypes* a_single,
                                    prism::ContextTypes* a_mult, prism::ContextTypes* a_mix,
                                    prism::ContextTypes* a_with_alpha, prism::ContextTypes* a_only_alpha,
                                    prism::ContextTypes* a_alpha, prism::ContextTypes* a_first_cycle) {
    auto c = std::get<prism::MTDArray<int>>(*a_arg);
    bool do_single = get_bool(a_single);
    bool do_multiply = get_bool(a_mult);
    bool do_mix = get_bool(a_mix);
    bool with_alpha = get_bool(a_with_alpha);
    bool only_alpha = get_bool(a_only_alpha);
    bool opt_alpha = get_bool(a_alpha);
    bool first_cycle = get_bool(a_first_cycle);
    std::string out = "";
    if (do_single) {
        out += shader_item_to_str(c.at(only_alpha, 3), with_alpha, only_alpha, opt_alpha, first_cycle, false);
    } else if (do_multiply) {
        out += shader_item_to_str(c.at(only_alpha, 0), with_alpha, only_alpha, opt_alpha, first_cycle, false);
        out += " * ";
        out += shader_item_to_str(c.at(only_alpha, 2), with_alpha, only_alpha, opt_alpha, first_cycle, true);
    } else if (do_mix) {
        out += "mix(";
        out += shader_item_to_str(c.at(only_alpha, 1), with_alpha, only_alpha, opt_alpha, first_cycle, false);
        out += ", ";
        out += shader_item_to_str(c.at(only_alpha, 0), with_alpha, only_alpha, opt_alpha, first_cycle, false);
        out += ", ";
        out += shader_item_to_str(c.at(only_alpha, 2), with_alpha, only_alpha, opt_alpha, first_cycle, true);
        out += ")";
    } else {
        out += "(";
        out += shader_item_to_str(c.at(only_alpha, 0), with_alpha, only_alpha, opt_alpha, first_cycle, false);
        out += " - ";
        out += shader_item_to_str(c.at(only_alpha, 1), with_alpha, only_alpha, opt_alpha, first_cycle, false);
        out += ") * ";
        out += shader_item_to_str(c.at(only_alpha, 2), with_alpha, only_alpha, opt_alpha, first_cycle, true);
        out += " + ";
        out += shader_item_to_str(c.at(only_alpha, 3), with_alpha, only_alpha, opt_alpha, first_cycle, false);
    }
    return new prism::ContextTypes{ out };
}

std::optional<std::string> opengl_include_fs(const std::string& path) {
    auto init = std::make_shared<Ship::ResourceInitData>();
    init->Type = (uint32_t)Ship::ResourceType::Shader;
    init->ByteOrder = Ship::Endianness::Native;
    init->Format = RESOURCE_FORMAT_BINARY;
    auto res = std::static_pointer_cast<Ship::Shader>(
        Ship::Context::GetInstance()->GetResourceManager()->LoadResource(path, true, init));
    if (res == nullptr) {
        return std::nullopt;
    }
    auto inc = static_cast<std::string*>(res->GetRawPointer());
    return *inc;
}

std::string GfxRenderingAPIOGL::BuildFsShader(const CCFeatures& cc_features) {
    prism::Processor processor;
    prism::ContextItems mContext = {
        { "VERTEX_SHADER", false },
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
        { "FILTER_THREE_POINT", FILTER_THREE_POINT },
        { "FILTER_LINEAR", FILTER_LINEAR },
        { "FILTER_NONE", FILTER_NONE },
        { "srgb_mode", mSrgbMode },
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
        { "o_three_point_filtering", mCurrentFilterMode == FILTER_THREE_POINT },
        { "append_formula", (InvokeFunc)append_formula },
#ifdef __APPLE__
        { "GLSL_VERSION", "#version 410 core" },
        { "attr", "in" },
        { "opengles", false },
        { "core_opengl", true },
        { "texture", "texture" },
        { "vOutColor", "vOutColor" },
#elif defined(USE_OPENGLES)
        { "GLSL_VERSION", "#version 300 es\nprecision mediump float;" },
        { "attr", "in" },
        { "opengles", true },
        { "core_opengl", false },
        { "texture", "texture" },
        { "vOutColor", "vOutColor" },
#else
        { "GLSL_VERSION", "#version 130" },
        { "attr", "varying" },
        { "opengles", false },
        { "core_opengl", false },
        { "texture", "texture2D" },
        { "vOutColor", "gl_FragColor" },
#endif
    };
    processor.populate(mContext);
    auto init = std::make_shared<Ship::ResourceInitData>();
    init->Type = (uint32_t)Ship::ResourceType::Shader;
    init->ByteOrder = Ship::Endianness::Native;
    init->Format = RESOURCE_FORMAT_BINARY;
    const char* shaderName = Fast::gfx_get_shader(cc_features.shader_id);
    std::string path = "shaders/opengl/default.shader.glsl";

    if (nullptr != shaderName) {
        path = std::string(shaderName) + ".glsl";
    }

    auto res = static_pointer_cast<Ship::Shader>(
        Ship::Context::GetInstance()->GetResourceManager()->LoadResource(path, true, init));

    if (res == nullptr) {
        SPDLOG_ERROR("Failed to load default fragment shader, missing gdiffuser.o2r?");
        abort();
    }

    auto shader = static_cast<std::string*>(res->GetRawPointer());
    processor.load(*shader);
    processor.bind_include_loader(opengl_include_fs);
    auto result = processor.process();
    // SPDLOG_INFO("=========== FRAGMENT SHADER ============");
    // SPDLOG_INFO(result);
    // SPDLOG_INFO("========================================");
    return result;
}

static size_t numFloats = 0;

static prism::ContextTypes* UpdateFloats(prism::ContextTypes* _, prism::ContextTypes* num) {
    numFloats += std::get<int>(*num);
    return nullptr;
}

static std::string BuildVsShader(const CCFeatures& cc_features) {
    numFloats = 4;
    prism::Processor processor;
    prism::ContextItems mContext = { { "VERTEX_SHADER", true },
                                     { "o_textures", M_ARRAY(cc_features.usedTextures, bool, 2) },
                                     { "o_clamp", M_ARRAY(cc_features.clamp, bool, 2, 2) },
                                     { "o_fog", cc_features.opt_fog },
                                     { "o_grayscale", cc_features.opt_grayscale },
                                     { "o_alpha", cc_features.opt_alpha },
                                     { "o_inputs", cc_features.numInputs },
                                     { "update_floats", (InvokeFunc)UpdateFloats },
#ifdef __APPLE__
                                     { "GLSL_VERSION", "#version 410 core" },
                                     { "attr", "in" },
                                     { "out", "out" },
                                     { "opengles", false }
#elif defined(USE_OPENGLES)
                                     { "GLSL_VERSION", "#version 300 es" },
                                     { "attr", "in" },
                                     { "out", "out" },
                                     { "opengles", true }
#else
                                     { "GLSL_VERSION", "#version 110" },
                                     { "attr", "attribute" },
                                     { "out", "varying" },
                                     { "opengles", false }
#endif
    };
    processor.populate(mContext);

    auto init = std::make_shared<Ship::ResourceInitData>();
    init->Type = (uint32_t)Ship::ResourceType::Shader;
    init->ByteOrder = Ship::Endianness::Native;
    init->Format = RESOURCE_FORMAT_BINARY;
    const char* shaderName = Fast::gfx_get_shader(cc_features.shader_id);
    std::string path = "shaders/opengl/default.shader.glsl";

    if (nullptr != shaderName) {
        path = std::string(shaderName) + ".glsl";
    }

    auto res = static_pointer_cast<Ship::Shader>(
        Ship::Context::GetInstance()->GetResourceManager()->LoadResource(path, true, init));

    if (res == nullptr) {
        SPDLOG_ERROR("Failed to load default vertex shader, missing gdiffuser.o2r?");
        abort();
    }

    auto shader = static_cast<std::string*>(res->GetRawPointer());
    processor.load(*shader);
    processor.bind_include_loader(opengl_include_fs);
    auto result = processor.process();
    // SPDLOG_INFO("=========== VERTEX SHADER ============");
    // SPDLOG_INFO(result);
    // SPDLOG_INFO("========================================");
    return result;
}

/*
 * Program-binary entry points. These are GL 4.1 core / ARB_get_program_binary, but the port
 * targets "#version 130" and GLES 3.0, so linking against them directly risks a missing symbol
 * taking the process down over an optimisation. Resolved through SDL at Init instead; absence
 * means "compile every run", as before the cache existed.
 */
#ifndef GL_PROGRAM_BINARY_LENGTH
#define GL_PROGRAM_BINARY_LENGTH 0x8741
#endif
#ifndef GL_NUM_PROGRAM_BINARY_FORMATS
#define GL_NUM_PROGRAM_BINARY_FORMATS 0x87FE
#endif
#ifndef GL_PROGRAM_BINARY_RETRIEVABLE_HINT
#define GL_PROGRAM_BINARY_RETRIEVABLE_HINT 0x8257
#endif
#ifndef APIENTRY
#define APIENTRY
#endif

typedef void(APIENTRY* GdxGlGetProgramBinaryFn)(GLuint, GLsizei, GLsizei*, GLenum*, void*);
typedef void(APIENTRY* GdxGlProgramBinaryFn)(GLuint, GLenum, const void*, GLsizei);
typedef void(APIENTRY* GdxGlProgramParameteriFn)(GLuint, GLenum, GLint);

static GdxGlGetProgramBinaryFn sGdxGlGetProgramBinary = nullptr;
static GdxGlProgramBinaryFn sGdxGlProgramBinary = nullptr;
static GdxGlProgramParameteriFn sGdxGlProgramParameteri = nullptr;
static bool sGdxProgramBinarySupported = false;

static void GdxResolveProgramBinaryEntryPoints() {
    sGdxGlGetProgramBinary = (GdxGlGetProgramBinaryFn)SDL_GL_GetProcAddress("glGetProgramBinary");
    sGdxGlProgramBinary = (GdxGlProgramBinaryFn)SDL_GL_GetProcAddress("glProgramBinary");
    sGdxGlProgramParameteri = (GdxGlProgramParameteriFn)SDL_GL_GetProcAddress("glProgramParameteri");

    // A driver may advertise the entry points and still support zero binary formats, in which
    // case every glProgramBinary call fails. Ask up front.
    GLint formats = 0;
    glGetIntegerv(GL_NUM_PROGRAM_BINARY_FORMATS, &formats);
    while (glGetError() != GL_NO_ERROR) {
        // On a driver without the enum this query itself raises GL_INVALID_ENUM, which would
        // otherwise be misattributed to whatever draws next.
    }

    sGdxProgramBinarySupported = sGdxGlGetProgramBinary != nullptr && sGdxGlProgramBinary != nullptr &&
                                 sGdxGlProgramParameteri != nullptr && formats > 0;
}

/**
 * @brief Fold the driver identity into the cache fingerprint.
 *
 * A GL program binary is only loadable by the exact driver that produced it, and vendor/renderer/
 * version are the strongest identity the API exposes. A driver update or a GPU swap must
 * invalidate the store wholesale rather than feed binaries to a driver that may misinterpret them.
 */
static uint64_t GdxGlDriverFingerprint() {
    uint64_t h = 1469598103934665603ull; // FNV-1a 64-bit offset basis
    const GLenum fields[3] = { GL_VENDOR, GL_RENDERER, GL_VERSION };
    for (GLenum field : fields) {
        const char* s = (const char*)glGetString(field);
        if (s != nullptr) {
            for (; *s != '\0'; ++s) {
                h ^= (uint64_t)(uint8_t)*s;
                h *= 1099511628211ull;
            }
        }
        h ^= 0xFFull; // field separator, so ("ab","c") and ("a","bc") differ
        h *= 1099511628211ull;
    }
    return h;
}

void GfxRenderingAPIOGL::ClearShaderCache() {
    mShaderProgramPool.clear();
}

ShaderProgram* GfxRenderingAPIOGL::CreateAndLoadNewShader(uint64_t shader_id0, uint64_t shader_id1) {
    CCFeatures cc_features;
    gfx_cc_get_features(shader_id0, shader_id1, &cc_features);

    // Both change the generated GLSL without being part of the shader id, so they must be in
    // the cache key or a filter-mode switch resurrects the wrong program.
    const uint32_t cacheFlags =
        (mCurrentFilterMode == FILTER_THREE_POINT ? (uint32_t)SHADER_CACHE_FLAG_THREE_POINT : 0u) |
        (mSrgbMode ? (uint32_t)SHADER_CACHE_FLAG_SRGB : 0u);

    /*
     * Cached payload layout, little-endian: numFloats u32, binaryFormat u32, binaryLength u32,
     * then the program binary.
     *
     * numFloats has to ride along: the prism update_floats callback accumulates it into a
     * file-static during template expansion, which a cache hit skips entirely. Reading the stale
     * static instead would mis-stride every vertex attribute for this program.
     */
    GLuint shader_program = 0;
    size_t programNumFloats = 0;
    static constexpr size_t kPayloadHeader = 12;

    const std::vector<uint8_t>* cached = mShaderCache.Lookup(shader_id0, shader_id1, cacheFlags);
    if (cached != nullptr && cached->size() > kPayloadHeader && sGdxProgramBinarySupported) {
        uint32_t storedFloats = 0, storedFormat = 0, storedLength = 0;
        memcpy(&storedFloats, cached->data(), sizeof(storedFloats));
        memcpy(&storedFormat, cached->data() + 4, sizeof(storedFormat));
        memcpy(&storedLength, cached->data() + 8, sizeof(storedLength));

        if ((size_t)storedLength + kPayloadHeader == cached->size() && storedLength > 0) {
            GLuint restored = glCreateProgram();
            sGdxGlProgramBinary(restored, (GLenum)storedFormat, cached->data() + kPayloadHeader,
                                (GLsizei)storedLength);

            // glProgramBinary may reject a binary it once produced (a driver update between
            // runs), signalling it through GL_LINK_STATUS rather than an error. Falling through
            // to a source compile is the documented recovery, not a failure path.
            GLint linked = GL_FALSE;
            glGetProgramiv(restored, GL_LINK_STATUS, &linked);
            while (glGetError() != GL_NO_ERROR) {
            }
            if (linked == GL_TRUE) {
                shader_program = restored;
                programNumFloats = storedFloats;
            } else {
                glDeleteProgram(restored);
            }
        }
    }

    if (shader_program == 0) {
        // Mirrors the D3D11 probe: template expansion, compile and link all happen inside the
        // draw call here too. The numbers are not expected to match D3D11, since some drivers
        // defer the real work to first use. Only reached on a genuine cache miss.
        const auto gdxShaderCompileStart = std::chrono::steady_clock::now();
        static int sGdxShaderCompileCount = 0;
        static double sGdxShaderCompileTotalMs = 0.0;

        const auto fs_buf = BuildFsShader(cc_features);
        const auto vs_buf = BuildVsShader(cc_features);
        const GLchar* sources[2] = { vs_buf.data(), fs_buf.data() };
        const GLint lengths[2] = { (GLint)vs_buf.size(), (GLint)fs_buf.size() };
        GLint success;

        GLuint vertex_shader = glCreateShader(GL_VERTEX_SHADER);
        glShaderSource(vertex_shader, 1, &sources[0], &lengths[0]);
        glCompileShader(vertex_shader);
        glGetShaderiv(vertex_shader, GL_COMPILE_STATUS, &success);
        if (!success) {
            GLint max_length = 0;
            glGetShaderiv(vertex_shader, GL_INFO_LOG_LENGTH, &max_length);
            char error_log[1024];
            // fprintf(stderr, "Vertex shader compilation failed\n");
            glGetShaderInfoLog(vertex_shader, max_length, &max_length, &error_log[0]);
            // fprintf(stderr, "%s\n", &error_log[0]);
            abort();
        }

        GLuint fragment_shader = glCreateShader(GL_FRAGMENT_SHADER);
        glShaderSource(fragment_shader, 1, &sources[1], &lengths[1]);
        glCompileShader(fragment_shader);
        glGetShaderiv(fragment_shader, GL_COMPILE_STATUS, &success);
        if (!success) {
            GLint max_length = 0;
            glGetShaderiv(fragment_shader, GL_INFO_LOG_LENGTH, &max_length);
            char error_log[1024];
            fprintf(stderr, "Fragment shader compilation failed\n");
            glGetShaderInfoLog(fragment_shader, max_length, &max_length, &error_log[0]);
            fprintf(stderr, "%s\n", &error_log[0]);
            abort();
        }

        shader_program = glCreateProgram();
        glAttachShader(shader_program, vertex_shader);
        glAttachShader(shader_program, fragment_shader);

        // Must be set before linking, or a driver is free to discard what it would need to hand
        // a binary back and glGetProgramBinary legitimately returns nothing.
        if (sGdxProgramBinarySupported) {
            sGdxGlProgramParameteri(shader_program, GL_PROGRAM_BINARY_RETRIEVABLE_HINT, GL_TRUE);
        }

        glLinkProgram(shader_program);
        programNumFloats = numFloats;

        {
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                                        gdxShaderCompileStart)
                                  .count();
            ++sGdxShaderCompileCount;
            sGdxShaderCompileTotalMs += ms;
            SPDLOG_ERROR("[shader-compile] opengl #{} id0={:016X} id1={:016X} {:.2f}ms (total {:.1f}ms)",
                         sGdxShaderCompileCount, shader_id0, shader_id1, ms, sGdxShaderCompileTotalMs);
        }

        if (sGdxProgramBinarySupported && mShaderCache.Enabled()) {
            GLint binaryLength = 0;
            glGetProgramiv(shader_program, GL_PROGRAM_BINARY_LENGTH, &binaryLength);
            if (binaryLength > 0) {
                std::vector<uint8_t> payload(kPayloadHeader + (size_t)binaryLength);
                GLenum binaryFormat = 0;
                GLsizei written = 0;
                sGdxGlGetProgramBinary(shader_program, binaryLength, &written, &binaryFormat,
                                       payload.data() + kPayloadHeader);
                if (written > 0) {
                    const uint32_t storedFloats = (uint32_t)programNumFloats;
                    const uint32_t storedFormat = (uint32_t)binaryFormat;
                    const uint32_t storedLength = (uint32_t)written;
                    memcpy(payload.data(), &storedFloats, sizeof(storedFloats));
                    memcpy(payload.data() + 4, &storedFormat, sizeof(storedFormat));
                    memcpy(payload.data() + 8, &storedLength, sizeof(storedLength));
                    payload.resize(kPayloadHeader + (size_t)written);
                    mShaderCache.Store(shader_id0, shader_id1, cacheFlags, payload.data(), payload.size());
                }
            }
            while (glGetError() != GL_NO_ERROR) {
            }
        }
    }

    size_t cnt = 0;

    struct ShaderProgram* prg = &mShaderProgramPool[std::make_pair(shader_id0, shader_id1)];
    prg->attribLocations[cnt] = glGetAttribLocation(shader_program, "aVtxPos");
    prg->attribSizes[cnt] = 4;
    ++cnt;

    for (int i = 0; i < 2; i++) {
        if (cc_features.usedTextures[i]) {
            char name[32];
            snprintf(name, sizeof(name), "aTexCoord%d", i);
            prg->attribLocations[cnt] = glGetAttribLocation(shader_program, name);
            prg->attribSizes[cnt] = 2;
            ++cnt;

            for (int j = 0; j < 2; j++) {
                if (cc_features.clamp[i][j]) {
                    snprintf(name, sizeof(name), "aTexClamp%s%d", j == 0 ? "S" : "T", i);
                    prg->attribLocations[cnt] = glGetAttribLocation(shader_program, name);
                    prg->attribSizes[cnt] = 1;
                    ++cnt;
                }
            }
        }
    }

    if (cc_features.opt_fog) {
        prg->attribLocations[cnt] = glGetAttribLocation(shader_program, "aFog");
        prg->attribSizes[cnt] = 4;
        ++cnt;
    }

    if (cc_features.opt_grayscale) {
        prg->attribLocations[cnt] = glGetAttribLocation(shader_program, "aGrayscaleColor");
        prg->attribSizes[cnt] = 4;
        ++cnt;
    }

    for (int i = 0; i < cc_features.numInputs; i++) {
        char name[16];
        snprintf(name, sizeof(name), "aInput%d", i + 1);
        prg->attribLocations[cnt] = glGetAttribLocation(shader_program, name);
        prg->attribSizes[cnt] = cc_features.opt_alpha ? 4 : 3;
        ++cnt;
    }

    prg->openglProgramId = shader_program;
    prg->numInputs = cc_features.numInputs;
    prg->usedTextures[0] = cc_features.usedTextures[0];
    prg->usedTextures[1] = cc_features.usedTextures[1];
    prg->usedTextures[2] = cc_features.used_masks[0];
    prg->usedTextures[3] = cc_features.used_masks[1];
    prg->usedTextures[4] = cc_features.used_blend[0];
    prg->usedTextures[5] = cc_features.used_blend[1];
    prg->numFloats = programNumFloats;
    prg->numAttribs = cnt;

    prg->frameCountLocation = glGetUniformLocation(shader_program, "frame_count");
    prg->noiseScaleLocation = glGetUniformLocation(shader_program, "noise_scale");
    prg->prim_depth_location = glGetUniformLocation(shader_program, "prim_depth");
    prg->alpha_compare_threshold_location = glGetUniformLocation(shader_program, "alpha_compare_threshold");
    // Per-element lookups: "name[i]" resolves even when the compiler trimmed the array's active
    // size, and inactive elements yield -1, which glUniform1i ignores.
    for (int i = 0; i < 2; i++) {
        char uname[32];
        snprintf(uname, sizeof(uname), "texture_width[%d]", i);
        prg->texture_width_locations[i] = glGetUniformLocation(shader_program, uname);
        snprintf(uname, sizeof(uname), "texture_height[%d]", i);
        prg->texture_height_locations[i] = glGetUniformLocation(shader_program, uname);
        snprintf(uname, sizeof(uname), "texture_filtering[%d]", i);
        prg->texture_filtering_locations[i] = glGetUniformLocation(shader_program, uname);
    }

    LoadShader(prg);

    if (cc_features.usedTextures[0]) {
        GLint sampler_location = glGetUniformLocation(shader_program, "uTex0");
        glUniform1i(sampler_location, 0);
    }
    if (cc_features.usedTextures[1]) {
        GLint sampler_location = glGetUniformLocation(shader_program, "uTex1");
        glUniform1i(sampler_location, 1);
    }
    if (cc_features.used_masks[0]) {
        GLint sampler_location = glGetUniformLocation(shader_program, "uTexMask0");
        glUniform1i(sampler_location, 2);
    }
    if (cc_features.used_masks[1]) {
        GLint sampler_location = glGetUniformLocation(shader_program, "uTexMask1");
        glUniform1i(sampler_location, 3);
    }
    if (cc_features.used_blend[0]) {
        GLint sampler_location = glGetUniformLocation(shader_program, "uTexBlend0");
        glUniform1i(sampler_location, 4);
    }
    if (cc_features.used_blend[1]) {
        GLint sampler_location = glGetUniformLocation(shader_program, "uTexBlend1");
        glUniform1i(sampler_location, 5);
    }

    return prg;
}

struct ShaderProgram* GfxRenderingAPIOGL::LookupShader(uint64_t shader_id0, uint64_t shader_id1) {
    auto it = mShaderProgramPool.find(std::make_pair(shader_id0, shader_id1));
    return it == mShaderProgramPool.end() ? nullptr : &it->second;
}

void GfxRenderingAPIOGL::ShaderGetInfo(struct ShaderProgram* prg, uint8_t* numInputs, bool usedTextures[2]) {
    *numInputs = prg->numInputs;
    usedTextures[0] = prg->usedTextures[0];
    usedTextures[1] = prg->usedTextures[1];
}

GLuint GfxRenderingAPIOGL::NewTexture() {
    GLuint ret;
    glGenTextures(1, &ret);
    textures.resize(std::max(textures.size(), (size_t)ret + 1));
    return ret;
}

void GfxRenderingAPIOGL::DeleteTexture(uint32_t texID) {
    glDeleteTextures(1, &texID);
}

void GfxRenderingAPIOGL::SelectTexture(int tile, GLuint texture_id) {
    if (mLastActiveTexture != tile) {
        mLastActiveTexture = tile;
        glActiveTexture(GL_TEXTURE0 + tile);
    }
    if (mLastBoundTextures[tile] != texture_id) {
        mLastBoundTextures[tile] = texture_id;
        glBindTexture(GL_TEXTURE_2D, texture_id);
    }
    mCurrentTextureIds[tile] = texture_id;
    mCurrentTile = tile;
}

void GfxRenderingAPIOGL::UploadTexture(const uint8_t* rgba32_buf, uint32_t width, uint32_t height) {
    if (width == 0 || height == 0) {
        return;
    }
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba32_buf);
    textures[mCurrentTextureIds[mCurrentTile]].width = width;
    textures[mCurrentTextureIds[mCurrentTile]].height = height;
}

#ifdef USE_OPENGLES
#define GL_MIRROR_CLAMP_TO_EDGE 0x8743
#endif

static uint32_t gfx_cm_to_opengl(uint32_t val) {
    switch (val) {
        case G_TX_NOMIRROR | G_TX_CLAMP:
            return GL_CLAMP_TO_EDGE;
        case G_TX_MIRROR | G_TX_WRAP:
            return GL_MIRRORED_REPEAT;
        case G_TX_MIRROR | G_TX_CLAMP:
            return GL_MIRROR_CLAMP_TO_EDGE;
        case G_TX_NOMIRROR | G_TX_WRAP:
            return GL_REPEAT;
    }
    return 0;
}

void GfxRenderingAPIOGL::SetSamplerParameters(int tile, bool linear_filter, uint32_t cms, uint32_t cmt) {
    if (mLastActiveTexture != tile) {
        mLastActiveTexture = tile;
        glActiveTexture(GL_TEXTURE0 + tile);
    }
    const GLint filter = linear_filter && mCurrentFilterMode == FILTER_LINEAR ? GL_LINEAR : GL_NEAREST;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
    textures[mCurrentTextureIds[tile]].filtering = !linear_filter ? FILTER_LINEAR : FILTER_THREE_POINT;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, gfx_cm_to_opengl(cms));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, gfx_cm_to_opengl(cmt));
}

void GfxRenderingAPIOGL::SetDepthTestAndMask(bool depth_test, bool z_upd) {
    mCurrentDepthTest = depth_test;
    mCurrentDepthMask = z_upd;
}

void GfxRenderingAPIOGL::SetCurrentAlphaCompareThreshold(float threshold) {
    // SetPerDrawUniforms re-sends this every draw, so unlike D3D11's cbuffer it needs no
    // dirty-flag gating.
    mCurrentAlphaCompareThreshold = threshold;
}

void GfxRenderingAPIOGL::SetCurrentPrimDepth(float depth) {
    if (depth != mCurrentPrimDepth) {
        mCurrentPrimDepth = depth;
        mPrimDepthDirty = true;
    }
}

void GfxRenderingAPIOGL::SetZmodeDecal(bool zmode_decal) {
    mCurrentZmodeDecal = zmode_decal;
}

void GfxRenderingAPIOGL::SetViewport(int x, int y, int width, int height) {
    glViewport(x, y, width, height);
}

void GfxRenderingAPIOGL::SetScissor(int x, int y, int width, int height) {
    glScissor(x, y, width, height);
}

void GfxRenderingAPIOGL::SetUseAlpha(bool use_alpha) {
    int8_t val = use_alpha ? 1 : 0;
    if (mLastBlendEnabled != val) {
        mLastBlendEnabled = val;
        if (use_alpha) {
            glEnable(GL_BLEND);
        } else {
            glDisable(GL_BLEND);
        }
    }
}

void GfxRenderingAPIOGL::DrawTriangles(float buf_vbo[], size_t buf_vbo_len, size_t buf_vbo_num_tris) {
    // mCurrentZmodeDecal has to be in this key because glDepthFunc below derives from it.
    // Interpreter::GfxSpTri1 pushes the test/mask pair and the decal bit through two independent
    // dirty checks, so a batch that flips only the decal bit skips this block and keeps the
    // previous draw's compare, while the polygon-offset block below does update -- compare and
    // bias then disagree, and which stale compare a decal batch inherits depends on what was
    // drawn before it, hence flicker rather than a static error. Upstream added the decal term to
    // DepthFunc in #612 without extending the key.
    //
    // Do not latch mLastZmodeDecal here: the polygon-offset block owns that latch and must still
    // observe the transition.
    if (mCurrentDepthTest != mLastDepthTest || mCurrentDepthMask != mLastDepthMask ||
        mCurrentZmodeDecal != mLastZmodeDecal) {
        mLastDepthTest = mCurrentDepthTest;
        mLastDepthMask = mCurrentDepthMask;

        if (mCurrentDepthTest || mLastDepthMask) {
            glEnable(GL_DEPTH_TEST);
            glDepthMask(mLastDepthMask ? GL_TRUE : GL_FALSE);
            glDepthFunc(mCurrentDepthTest ? (mCurrentZmodeDecal ? GL_LEQUAL : GL_LESS) : GL_ALWAYS);
        } else {
            glDisable(GL_DEPTH_TEST);
        }
    }

    if (mCurrentZmodeDecal != mLastZmodeDecal) {
        mLastZmodeDecal = mCurrentZmodeDecal;
        if (mCurrentZmodeDecal) {
            // SSDB = SlopeScaledDepthBias 120 leads to -2 at 240p which is the same as N64 mode which has very little
            // fighting
            const int n64modeFactor = 120;
            const int noVanishFactor = 100;
            GLfloat SSDB = -2;
            switch (Ship::Context::GetInstance()->GetConsoleVariables()->GetInteger(CVAR_Z_FIGHTING_MODE, 0)) {
                // scaled z-fighting (N64 mode like)
                case 1:
                    if (mFrameBuffers.size() >
                        mCurrentFrameBuffer) { // safety check for vector size can probably be removed
                        SSDB = -1.0f * (GLfloat)mFrameBuffers[mCurrentFrameBuffer].height / n64modeFactor;
                    }
                    break;
                // no vanishing paths
                case 2:
                    if (mFrameBuffers.size() >
                        mCurrentFrameBuffer) { // safety check for vector size can probably be removed
                        SSDB = -1.0f * (GLfloat)mFrameBuffers[mCurrentFrameBuffer].height / noVanishFactor;
                    }
                    break;
                // disabled
                case 0:
                default:
                    SSDB = -2;
            }
            glPolygonOffset(SSDB, -2);
            glEnable(GL_POLYGON_OFFSET_FILL);
        } else {
            glPolygonOffset(0, 0);
            glDisable(GL_POLYGON_OFFSET_FILL);
        }
    }

    SetPerDrawUniforms();

    // printf("flushing %d tris\n", buf_vbo_num_tris);
    glBufferData(GL_ARRAY_BUFFER, sizeof(float) * buf_vbo_len, buf_vbo, GL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, 3 * buf_vbo_num_tris);
}

void GfxRenderingAPIOGL::Init() {
#if !defined(__linux__) && !defined(__OpenBSD__)
    glewInit();
#endif

    glGenBuffers(1, &mOpenglVbo);
    glBindBuffer(GL_ARRAY_BUFFER, mOpenglVbo);

#if defined(__APPLE__) || defined(USE_OPENGLES)
    glGenVertexArrays(1, &mOpenglVao);
    glBindVertexArray(mOpenglVao);
#endif

#ifndef USE_OPENGLES // not supported on gles
    glEnable(GL_DEPTH_CLAMP);
#endif
    glDepthFunc(GL_LEQUAL);
    // Separate alpha factors, matching the per-shader blend state D3D11 builds: colour blends
    // conventionally but destination alpha is preserved. Render targets are cleared to alpha 1.0
    // and everything downstream assumes they stay opaque, which the single-function form eroded
    // on every blended draw.
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);

    // Bring the driver and the cached flag into agreement before the first draw. Scissor-enabled
    // is this backend's steady state, but until some blit path happened to run, the cache
    // asserted nothing while GL had the test off -- and a cache that later reads 1 never issues
    // the glEnable it believes is redundant.
    glEnable(GL_SCISSOR_TEST);
    mLastScissorEnabled = 1;

    mFrameBuffers.resize(1); // for the default screen buffer

    glGenRenderbuffers(1, &mPixelDepthRb);
    glBindRenderbuffer(GL_RENDERBUFFER, mPixelDepthRb);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, 1, 1);
    glBindRenderbuffer(GL_RENDERBUFFER, 0);

    glGenFramebuffers(1, &mPixelDepthFb);
    glBindFramebuffer(GL_FRAMEBUFFER, mPixelDepthFb);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, mPixelDepthRb);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    mPixelDepthRbSize = 1;

    glGetIntegerv(GL_MAX_SAMPLES, &mMaxMsaaLevel);

    // Must come after glewInit: the driver strings and the program-binary entry points are only
    // reachable on a live context. No shipped seed here, unlike D3D11 -- a GL program binary is
    // loadable only by the driver that produced it, so the driver identity goes into the
    // fingerprint and every machine builds its own store.
    GdxResolveProgramBinaryEntryPoints();
    if (sGdxProgramBinarySupported) {
        mShaderCache.Init(SHADER_CACHE_TAG_OPENGL, GdxGlDriverFingerprint(), nullptr,
                          "gdiffuser-shadercache-opengl.bin");
    } else {
        SPDLOG_WARN("[shader-cache] opengl: no usable program-binary support; shaders compile every run");
    }
}

void GfxRenderingAPIOGL::OnResize() {
}

void GfxRenderingAPIOGL::StartFrame() {
    mFrameCount++;
}

void GfxRenderingAPIOGL::EndFrame() {
    glFlush();
}

void GfxRenderingAPIOGL::FinishRender() {
}

int GfxRenderingAPIOGL::CreateFramebuffer() {
    GLuint clrbuf;
    glGenTextures(1, &clrbuf);
    glBindTexture(GL_TEXTURE_2D, clrbuf);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, 1, 1, 0, GL_RGB, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glBindTexture(GL_TEXTURE_2D, 0);
    // The bind/unbind pair above lands on whichever unit SelectTexture last activated, leaving
    // that unit's cached name claiming a texture id while the driver has the default bound.
    // SelectTexture early-outs on a cache hit, so the next call with that same id would skip its
    // glBindTexture and sample the incomplete default texture.
    if (mLastActiveTexture >= 0 && mLastActiveTexture < SHADER_MAX_TEXTURES) {
        mLastBoundTextures[mLastActiveTexture] = 0;
    }

    GLuint clrbufMsaa;
    glGenRenderbuffers(1, &clrbufMsaa);

    GLuint rbo;
    glGenRenderbuffers(1, &rbo);
    glBindRenderbuffer(GL_RENDERBUFFER, rbo);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, 1, 1);
    glBindRenderbuffer(GL_RENDERBUFFER, 0);

    GLuint fbo;
    glGenFramebuffers(1, &fbo);

    size_t i = mFrameBuffers.size();
    mFrameBuffers.resize(i + 1);

    mFrameBuffers[i].fbo = fbo;
    mFrameBuffers[i].clrbuf = clrbuf;
    mFrameBuffers[i].clrbufMsaa = clrbufMsaa;
    mFrameBuffers[i].rbo = rbo;

    return i;
}

void GfxRenderingAPIOGL::UpdateFramebufferParameters(int fb_id, uint32_t width, uint32_t height, uint32_t msaa_level,
                                                     bool opengl_invertY, bool render_target, bool has_depth_buffer,
                                                     bool can_extract_depth, GdxFramebufferFormat format) {
    FramebufferOGL& fb = mFrameBuffers[fb_id];

    width = std::max(width, 1U);
    height = std::max(height, 1U);
    msaa_level = std::min(msaa_level, (uint32_t)mMaxMsaaLevel);

    glBindFramebuffer(GL_FRAMEBUFFER, fb.fbo);

    if (fb_id != 0) {
        // Post-pipeline passes need alpha-capable storage; keep GL_RGB8 for the
        // default path so existing callers are unaffected.
        GLenum internalFormat = GL_RGB8;
        GLenum pixelFormat = GL_RGB;
        switch (format) {
            case GdxFramebufferFormat::R8G8B8A8_UNORM:
                internalFormat = GL_RGBA8;
                pixelFormat = GL_RGBA;
                break;
            case GdxFramebufferFormat::R16G16B16A16_FLOAT:
                internalFormat = GL_RGBA16F;
                pixelFormat = GL_RGBA;
                break;
            case GdxFramebufferFormat::R8G8B8A8_UNORM_SRGB:
                internalFormat = GL_SRGB8_ALPHA8;
                pixelFormat = GL_RGBA;
                break;
            default:
                break;
        }

        if (fb.width != width || fb.height != height || fb.msaa_level != msaa_level || fb.lastFormat != internalFormat) {
            if (msaa_level <= 1) {
                glBindTexture(GL_TEXTURE_2D, fb.clrbuf);
                glTexImage2D(GL_TEXTURE_2D, 0, internalFormat, width, height, 0, pixelFormat,
                             internalFormat == GL_RGBA16F ? GL_HALF_FLOAT : GL_UNSIGNED_BYTE, NULL);
                glBindTexture(GL_TEXTURE_2D, 0);
                // Same cache invalidation as in CreateFramebuffer above. Gated on a size/MSAA
                // change, so the symptom is a one-frame wrong-texture glitch after a resize
                // rather than a per-frame artefact.
                if (mLastActiveTexture >= 0 && mLastActiveTexture < SHADER_MAX_TEXTURES) {
                    mLastBoundTextures[mLastActiveTexture] = 0;
                }
                glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, fb.clrbuf, 0);
            } else {
                glBindRenderbuffer(GL_RENDERBUFFER, fb.clrbufMsaa);
                glRenderbufferStorageMultisample(GL_RENDERBUFFER, msaa_level, internalFormat, width, height);
                glBindRenderbuffer(GL_RENDERBUFFER, 0);
                glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, fb.clrbufMsaa);
            }
            fb.lastFormat = internalFormat;
        }

        if (has_depth_buffer &&
            (fb.width != width || fb.height != height || fb.msaa_level != msaa_level || !fb.has_depth_buffer)) {
            glBindRenderbuffer(GL_RENDERBUFFER, fb.rbo);
            if (msaa_level <= 1) {
                glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, width, height);
            } else {
                glRenderbufferStorageMultisample(GL_RENDERBUFFER, msaa_level, GL_DEPTH24_STENCIL8, width, height);
            }
            glBindRenderbuffer(GL_RENDERBUFFER, 0);
        }

        if (!fb.has_depth_buffer && has_depth_buffer) {
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, fb.rbo);
        } else if (fb.has_depth_buffer && !has_depth_buffer) {
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, 0);
        }
    }

    fb.width = width;
    fb.height = height;
    fb.has_depth_buffer = has_depth_buffer;
    fb.msaa_level = msaa_level;
    fb.invertY = opengl_invertY;

    // Restore the draw target this call found bound. The glBindFramebuffer near the top exists
    // only because the attach calls act on whatever is bound to GL_FRAMEBUFFER; it is not a
    // request to move rendering. The sibling entry points below restore the same way.
    //
    // Leaving the binding behind is fatal in this port's frame loop, not merely untidy: the host
    // runs the entire game frame inside gdx_vi_tick() before Gui::StartDraw(), and ImGui's GL3
    // backend issues no glBindFramebuffer of its own, so the stray offscreen binding survived
    // into EndDraw() and the menu was rendered into the offscreen texture while the window
    // presented its bare clear.
    glBindFramebuffer(GL_FRAMEBUFFER, mFrameBuffers[mCurrentFrameBuffer].fbo);
}

void GfxRenderingAPIOGL::StartDrawToFramebuffer(int fb_id, float noise_scale) {
    FramebufferOGL& fb = mFrameBuffers[fb_id];

    if (noise_scale != 0.0f) {
        mCurrentNoiseScale = 1.0f / noise_scale;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, fb.fbo);
    mCurrentFrameBuffer = fb_id;
}

void GfxRenderingAPIOGL::ClearFramebuffer(bool color, bool depth) {
    // Clear the framebuffer this backend is drawing to, not whatever happens to be bound. Every
    // call site already pairs StartDrawToFramebuffer immediately before this, so the two have
    // always agreed; binding explicitly makes that structural, as D3D11's equivalent already is.
    const FramebufferOGL& fb = mFrameBuffers[mCurrentFrameBuffer];
    glBindFramebuffer(GL_FRAMEBUFFER, fb.fbo);

    if (mLastScissorEnabled != 0) {
        mLastScissorEnabled = 0;
        glDisable(GL_SCISSOR_TEST);
    }
    glDepthMask(GL_TRUE);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    // Gated on the attachment, like D3D11's ClearDepthStencilView call: a depth clear against a
    // target with no depth attachment means nothing.
    glClear((color ? GL_COLOR_BUFFER_BIT : 0) | (depth && fb.has_depth_buffer ? GL_DEPTH_BUFFER_BIT : 0));
    glDepthMask(mCurrentDepthMask ? GL_TRUE : GL_FALSE);
    if (mLastScissorEnabled != 1) {
        mLastScissorEnabled = 1;
        glEnable(GL_SCISSOR_TEST);
    }
}

void GfxRenderingAPIOGL::ClearDepthRegion(int x, int y, int w, int h) {
    // Save current scissor state so callers don't need to manually invalidate.
    GLint prevScissor[4];
    GLboolean scissorWasEnabled = glIsEnabled(GL_SCISSOR_TEST);
    glGetIntegerv(GL_SCISSOR_BOX, prevScissor);

    glEnable(GL_SCISSOR_TEST);
    glScissor(x, y, w, h);
    glDepthMask(GL_TRUE);
    glClear(GL_DEPTH_BUFFER_BIT);
    glDepthMask(mCurrentDepthMask ? GL_TRUE : GL_FALSE);

    // Restore previous scissor state.
    glScissor(prevScissor[0], prevScissor[1], prevScissor[2], prevScissor[3]);
    if (!scissorWasEnabled) {
        glDisable(GL_SCISSOR_TEST);
    }
}

void GfxRenderingAPIOGL::ResolveMSAAColorBuffer(int fb_id_target, int fb_id_source) {
    FramebufferOGL& fb_dst = mFrameBuffers[fb_id_target];
    FramebufferOGL& fb_src = mFrameBuffers[fb_id_source];
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fb_dst.fbo);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fb_src.fbo);

    // Disabled for blit
    if (mLastScissorEnabled != 0) {
        mLastScissorEnabled = 0;
        glDisable(GL_SCISSOR_TEST);
    }

    GLint dstY0 = 0, dstY1 = (GLint)fb_dst.height;
    if (fb_src.invertY != fb_dst.invertY) {
        std::swap(dstY0, dstY1);
    }
    glBlitFramebuffer(0, 0, fb_src.width, fb_src.height, 0, dstY0, fb_dst.width, dstY1, GL_COLOR_BUFFER_BIT,
                      GL_NEAREST);
    // mCurrentFrameBuffer is an index into mFrameBuffers, not a GL name; passing it straight to
    // glBindFramebuffer only worked when the index happened to be 0.
    glBindFramebuffer(GL_FRAMEBUFFER, mFrameBuffers[mCurrentFrameBuffer].fbo);

    if (mLastScissorEnabled != 1) {
        mLastScissorEnabled = 1;
        glEnable(GL_SCISSOR_TEST);
    }
}

void* GfxRenderingAPIOGL::GetFramebufferTextureId(int fb_id) {
    return (void*)(uintptr_t)mFrameBuffers[fb_id].clrbuf;
}

void GfxRenderingAPIOGL::SelectTextureFb(int fb_id) {
    // glDisable(GL_DEPTH_TEST);
    int tile = 0;
    const FramebufferOGL& fb = mFrameBuffers[fb_id];
    GLuint texId = fb.clrbuf;
    // Ensure the textures metadata vector can hold this FB texture handle.
    // FB color buffers are created outside NewTexture(), so the vector may
    // not have been resized for them yet.
    if (texId >= textures.size()) {
        textures.resize((size_t)texId + 1);
    }
    SelectTexture(tile, texId);

    // SetSamplerParameters never runs for a framebuffer texture, because no tile descriptor
    // describes it, so it kept the GL_LINEAR set once in CreateFramebuffer plus a
    // default-constructed TextureInfo -- whose zeroed filtering field reads as FILTER_THREE_POINT
    // with width and height 0. The shader then ran its three-point filter, dividing by a zero
    // texel size, on top of hardware bilinear taps. The values below are what
    // SetSamplerParameters(tile, true, G_TX_WRAP, G_TX_WRAP) would produce, which is what D3D11
    // does for its framebuffer textures. FILTER_THREE_POINT here means "the shader filters", and
    // is only correct because the hardware filter drops to GL_NEAREST in that mode.
    //
    // Keep this in SelectTextureFb rather than CreateFramebuffer: GetFramebufferTextureId hands
    // this texture to ImGui for the final present, and ImGui samples it with the texture object's
    // own parameters, so the creation-time GL_LINEAR must survive for framebuffers that are only
    // presented and never sampled by the game.
    const GLint filter = mCurrentFilterMode == FILTER_LINEAR ? GL_LINEAR : GL_NEAREST;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    textures[texId].width = (uint16_t)fb.width;
    textures[texId].height = (uint16_t)fb.height;
    textures[texId].filtering = FILTER_THREE_POINT;
}

/*
 * CRT post-process pass (gEnhancements.Graphics.CRTShader). The vertex shader is attribute-less
 * (gl_VertexID, available in every profile this file targets): the combiner programs own the
 * vertex attrib state and LoadShader re-applies it only on a combiner change, so a pass that
 * touched attrib pointers could leave stale state behind for the next frame's first draw.
 */
#ifdef __APPLE__
#define GDX_POST_VS                                                                            \
    "#version 410 core\n"                                                                      \
    "out vec2 vUV;\n"
#define GDX_POST_FS_PRELUDE                                                                    \
    "#version 410 core\n"                                                                      \
    "in vec2 vUV;\n"                                                                           \
    "out vec4 gdxFragColor;\n"
#define GDX_POST_SAMPLE "texture"
#elif defined(USE_OPENGLES)
#define GDX_POST_VS                                                                            \
    "#version 300 es\n"                                                                        \
    "out vec2 vUV;\n"
#define GDX_POST_FS_PRELUDE                                                                    \
    "#version 300 es\n"                                                                        \
    "precision mediump float;\n"                                                               \
    "in vec2 vUV;\n"                                                                           \
    "out vec4 gdxFragColor;\n"
#define GDX_POST_SAMPLE "texture"
#else
#define GDX_POST_VS                                                                            \
    "#version 130\n"                                                                           \
    "varying vec2 vUV;\n"
#define GDX_POST_FS_PRELUDE                                                                    \
    "#version 130\n"                                                                           \
    "varying vec2 vUV;\n"                                                                      \
    "#define gdxFragColor gl_FragColor\n"
#define GDX_POST_SAMPLE "texture2D"
#endif

static const char* sGdxPostVsSource = GDX_POST_VS
    "void main() {\n"
    "    vec2 pos = vec2(gl_VertexID == 1 ? 3.0 : -1.0, gl_VertexID == 2 ? 3.0 : -1.0);\n"
    "    vUV = pos * 0.5 + 0.5;\n"
    "    gl_Position = vec4(pos, 0.0, 1.0);\n"
    "}\n";

// Translated .slang shaders emit GLSL 330 with in/out varyings, so the vertex
// shader must match that profile.
static const char* sGdxPostVsSource330 = R"(
#version 330
out vec2 vUV;
void main() {
    vec2 pos = vec2(gl_VertexID == 1 ? 3.0 : -1.0, gl_VertexID == 2 ? 3.0 : -1.0);
    vUV = pos * 0.5 + 0.5;
    gl_Position = vec4(pos, 0.0, 1.0);
}
)";

// uSrcSize is the native N64 resolution the downsample target holds: scanlines key on source
// rows, not output pixels, so the line count stays authentic at any window size.
static const char* sGdxPostFsScanlines = GDX_POST_FS_PRELUDE
    "uniform sampler2D uTex;\n"
    "uniform vec2 uSrcSize;\n"
    "uniform vec2 uOutSize;\n"
    "void main() {\n"
    "    vec2 uv = vec2(vUV.x, 1.0 - vUV.y);\n"
    "    vec3 color = " GDX_POST_SAMPLE "(uTex, uv).rgb;\n"
    "    float scan = 0.80 + 0.20 * cos(6.2831853 * (fract(uv.y * uSrcSize.y) - 0.5));\n"
    "    gdxFragColor = vec4(color * scan, 1.0);\n"
    "}\n";

// Scanlines plus an aperture-grille mask: each output column keeps one channel at full gain.
// The gamma lift recovers the brightness the mask and scanlines remove.
static const char* sGdxPostFsCrt = GDX_POST_FS_PRELUDE
    "uniform sampler2D uTex;\n"
    "uniform vec2 uSrcSize;\n"
    "uniform vec2 uOutSize;\n"
    "void main() {\n"
    "    vec2 uv = vec2(vUV.x, 1.0 - vUV.y);\n"
    "    vec3 color = " GDX_POST_SAMPLE "(uTex, uv).rgb;\n"
    "    float scan = 0.80 + 0.20 * cos(6.2831853 * (fract(uv.y * uSrcSize.y) - 0.5));\n"
    "    float phase = mod(floor(uv.x * uOutSize.x), 3.0);\n"
    "    vec3 mask = vec3(0.75);\n"
    "    if (phase < 0.5) {\n"
    "        mask.r = 1.0;\n"
    "    } else if (phase < 1.5) {\n"
    "        mask.g = 1.0;\n"
    "    } else {\n"
    "        mask.b = 1.0;\n"
    "    }\n"
    "    color = pow(color * scan * mask, vec3(0.85));\n"
    "    gdxFragColor = vec4(color, 1.0);\n"
    "}\n";

// Unlike the combiner path, a failed compile must not abort(): the interpreter falls back to
// presenting the unfiltered framebuffer when this returns 0.
static GLuint GdxCompilePostProgram(const char* vsSource, const char* fsSource) {
    GLint success;
    GLuint vertexShader = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vertexShader, 1, &vsSource, nullptr);
    glCompileShader(vertexShader);
    glGetShaderiv(vertexShader, GL_COMPILE_STATUS, &success);
    if (!success) {
        char log[1024];
        glGetShaderInfoLog(vertexShader, sizeof(log), nullptr, log);
        SPDLOG_ERROR("Post vertex shader compilation failed: {}", log);
        glDeleteShader(vertexShader);
        return 0;
    }

    GLuint fragmentShader = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fragmentShader, 1, &fsSource, nullptr);
    glCompileShader(fragmentShader);
    glGetShaderiv(fragmentShader, GL_COMPILE_STATUS, &success);
    if (!success) {
        char log[1024];
        glGetShaderInfoLog(fragmentShader, sizeof(log), nullptr, log);
        SPDLOG_ERROR("Post fragment shader compilation failed: {}", log);
        glDeleteShader(vertexShader);
        glDeleteShader(fragmentShader);
        return 0;
    }

    GLuint program = glCreateProgram();
    glAttachShader(program, vertexShader);
    glAttachShader(program, fragmentShader);
    glLinkProgram(program);
    glDeleteShader(vertexShader);
    glDeleteShader(fragmentShader);

    glGetProgramiv(program, GL_LINK_STATUS, &success);
    if (!success) {
        char log[1024];
        glGetProgramInfoLog(program, sizeof(log), nullptr, log);
        SPDLOG_ERROR("Post shader link failed: {}", log);
        glDeleteProgram(program);
        return 0;
    }
    return program;
}

// The user writes only the fragment-shader body (a void main() that uses uTex/uSrcSize/uOutSize/vUV
// and writes gdxFragColor). The backend injects the correct #version and declarations for the
// active GL profile, so a single .glsl file works on desktop, Apple core, and GLES.
static std::string GdxBuildCustomPostFsSource(const std::string& userBody) {
#ifdef __APPLE__
    return std::string("#version 410 core\n"
                       "in vec2 vUV;\n"
                       "out vec4 gdxFragColor;\n"
                       "uniform sampler2D uTex;\n"
                       "uniform vec2 uSrcSize;\n"
                       "uniform vec2 uOutSize;\n") +
           userBody;
#elif defined(USE_OPENGLES)
    return std::string("#version 300 es\n"
                       "precision mediump float;\n"
                       "in vec2 vUV;\n"
                       "out vec4 gdxFragColor;\n"
                       "uniform sampler2D uTex;\n"
                       "uniform vec2 uSrcSize;\n"
                       "uniform vec2 uOutSize;\n") +
           userBody;
#else
    return std::string("#version 130\n"
                       "varying vec2 vUV;\n"
                       "#define gdxFragColor gl_FragColor\n"
                       "uniform sampler2D uTex;\n"
                       "uniform vec2 uSrcSize;\n"
                       "uniform vec2 uOutSize;\n") +
           userBody;
#endif
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
    // Strip UTF-8 BOM so GLSL compilers do not choke on it.
    if (result.size() >= 3 && (unsigned char)result[0] == 0xEF && (unsigned char)result[1] == 0xBB &&
        (unsigned char)result[2] == 0xBF) {
        result.erase(0, 3);
    }
    return result;
}

void GfxRenderingAPIOGL::GdxInitPostProgramOGL(PostShaderProgramOGL* prg) {
    if (prg->program == 0) {
        return;
    }
    prg->srcSizeLocation = glGetUniformLocation(prg->program, "uSrcSize");
    prg->outSizeLocation = glGetUniformLocation(prg->program, "uOutSize");
    // Sampler bindings are program state, but glUniform writes to the BOUND program:
    // restore whatever was bound so mLastLoadedShader's cache stays truthful.
    GLint curProgram = 0;
    glGetIntegerv(GL_CURRENT_PROGRAM, &curProgram);
    glUseProgram(prg->program);
    glUniform1i(glGetUniformLocation(prg->program, "uTex"), 0);
    for (const GdxSlangParameter& p : prg->slangParameters) {
        prg->slangParameterLocations[p.name] = glGetUniformLocation(prg->program, p.name.c_str());
    }
    prg->slangFrameCountLocation = glGetUniformLocation(prg->program, "gdxFrameCount");
    prg->slangMvpLocation = glGetUniformLocation(prg->program, "gdxMvp");

    // Slice 3: cache locations for the extra sampler families so each pass can
    // bind them to the fixed convention slots emitted by the translator.
    auto cacheSampler = [&](const std::string& base, int n, int binding) {
        const std::string name = base + std::to_string(n);
        GLint loc = glGetUniformLocation(prg->program, name.c_str());
        if (loc >= 0) {
            glUniform1i(loc, binding);
            prg->slangSamplerLocations[name] = loc;
        }
    };
    for (int n = 0; n <= prg->slangUsedBuiltins.maxOriginalHistory; ++n) {
        cacheSampler("gdxOriginalHistory", n, GdxSlangTextureBindings::OriginalHistory(n));
    }
    for (int n : prg->slangUsedBuiltins.passOutputIndices) {
        cacheSampler("gdxPassOutput", n, GdxSlangTextureBindings::PassOutput(n));
    }
    for (int n : prg->slangUsedBuiltins.passFeedbackIndices) {
        cacheSampler("gdxPassFeedback", n, GdxSlangTextureBindings::PassFeedback(n));
    }
    for (int n : prg->slangUsedBuiltins.userTextureIndices) {
        cacheSampler("gdxUser", n, GdxSlangTextureBindings::User(n));
    }
    for (size_t k = 0; k < prg->slangUsedBuiltins.namedSamplers.size(); ++k) {
        cacheSampler("gdxNamed", static_cast<int>(k), GdxSlangTextureBindings::Named(static_cast<int>(k)));
    }
    for (const std::string& sizeName : prg->slangUsedBuiltins.extraSizeNames) {
        prg->slangExtraSizeLocations[sizeName] = glGetUniformLocation(prg->program, sizeName.c_str());
    }

    glUseProgram((GLuint)curProgram);
}

uintptr_t GfxRenderingAPIOGL::ApplyPostShader(int srcFbId, int mode, uint32_t nativeW, uint32_t nativeH,
                                              uint32_t outW, uint32_t outH) {
    if (srcFbId <= 0 || srcFbId >= (int)mFrameBuffers.size() || nativeW == 0 || nativeH == 0 || outW == 0 ||
        outH == 0) {
        return 0;
    }

    PostShaderProgramOGL* prg = nullptr;
    const char* customStem = CVarGetString("gEnhancements.Graphics.CustomShader", "");
    if (customStem != nullptr && customStem[0] != '\0') {
        if (strchr(customStem, '\\') != nullptr || strchr(customStem, '/') != nullptr ||
            strchr(customStem, '.') != nullptr) {
            SPDLOG_ERROR("Invalid custom shader stem '{}': must be a plain file name", customStem);
            return 0;
        }
        std::filesystem::path shaderPath =
            std::filesystem::path(Ship::Context::GetAppDirectoryPath()) / "shaders" / (std::string(customStem) + ".glsl");
        GdxLogPostShaderInfo(fmt::format("scan/select: custom stem '{}' -> {}", customStem, shaderPath.string()));
        if (!std::filesystem::exists(shaderPath)) {
            SPDLOG_ERROR("Custom shader file not found: {}", shaderPath.string());
            return 0;
        }
        uint64_t mtimeTicks =
            static_cast<uint64_t>(std::filesystem::last_write_time(shaderPath).time_since_epoch().count());
        auto& entry = mPostShaderCustomCache[customStem];
        if (!entry.first.attempted || entry.second != mtimeTicks) {
            entry.second = mtimeTicks;
            entry.first = {}; // drop stale GL program before recompiling
            entry.first.attempted = true;
            std::string userBody = GdxReadShaderFile(shaderPath);
            if (userBody.empty()) {
                SPDLOG_ERROR("Custom shader file is empty or could not be read: {}", shaderPath.string());
                return 0;
            }
            std::string fsSource = GdxBuildCustomPostFsSource(userBody);
            entry.first.program = GdxCompilePostProgram(sGdxPostVsSource, fsSource.c_str());
            if (entry.first.program == 0) {
                SPDLOG_ERROR("Custom shader compile failed: {}", shaderPath.string());
                return 0;
            }
            GdxLogPostShaderInfo(fmt::format("compile: custom shader '{}' compiled successfully",
                                             shaderPath.string()));
            GdxInitPostProgramOGL(&entry.first);
        }
        prg = &entry.first;
    } else {
        if (mode < 1 || mode > 2) {
            return 0;
        }
        PostShaderProgramOGL& modePrg = mPostShaderPrograms[mode - 1];
        if (!modePrg.attempted) {
            modePrg.attempted = true;
            modePrg.program = GdxCompilePostProgram(sGdxPostVsSource, mode == 1 ? sGdxPostFsScanlines : sGdxPostFsCrt);
            GdxInitPostProgramOGL(&modePrg);
        }
        prg = &modePrg;
    }
    if (prg == nullptr || prg->program == 0) {
        return 0;
    }

    const FramebufferOGL& src = mFrameBuffers[srcFbId];

    if (mPostDownsampleFb < 0) {
        mPostDownsampleFb = CreateFramebuffer();
        // The post pass upscales with NEAREST for crisp texels; CreateFramebuffer's default
        // GL_LINEAR would blur the very pixels the shader is trying to make visible.
        glBindTexture(GL_TEXTURE_2D, mFrameBuffers[mPostDownsampleFb].clrbuf);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_2D, 0);
        if (mLastActiveTexture >= 0 && mLastActiveTexture < SHADER_MAX_TEXTURES) {
            mLastBoundTextures[mLastActiveTexture] = 0;
        }
    }
    if (mPostOutputFb < 0) {
        mPostOutputFb = CreateFramebuffer();
    }
#if defined(__APPLE__) || defined(USE_OPENGLES)
    if (mPostVao == 0) {
        glGenVertexArrays(1, &mPostVao);
    }
#endif

    // The downsample target stores bottom-up to match the D3D11 post convention: custom shaders
    // (e.g. satpixie-crt.glsl) sample with `uv.y = 1.0 - vUV.y` assuming a Y-flipped source.
    // The output target stays top-down because the built-in/custom fragment shaders flip on read.
    UpdateFramebufferParameters(mPostDownsampleFb, nativeW, nativeH, 1, false, true, false, false);
    UpdateFramebufferParameters(mPostOutputFb, outW, outH, 1, true, true, false, false);

    // Disabled for blit, as in ResolveMSAAColorBuffer.
    if (mLastScissorEnabled != 0) {
        mLastScissorEnabled = 0;
        glDisable(GL_SCISSOR_TEST);
    }

    // Step 1: GL_LINEAR downsample of the rendered frame to native resolution. The destination
    // is bottom-up, so a top-down source must be swapped; a bottom-up source copies straight.
    glBindFramebuffer(GL_READ_FRAMEBUFFER, src.fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, mFrameBuffers[mPostDownsampleFb].fbo);
    GLint dstY0 = 0, dstY1 = (GLint)nativeH;
    if (src.invertY) {
        std::swap(dstY0, dstY1);
    }
    glBlitFramebuffer(0, 0, src.width, src.height, 0, dstY0, (GLint)nativeW, dstY1, GL_COLOR_BUFFER_BIT, GL_LINEAR);

    // Step 2: fullscreen triangle through the post shader into the output target. All GL state
    // this touches is saved and restored, because the interpreter's caches (mLastLoadedShader,
    // mLastActiveTexture, ...) only stay valid while the GL state matches what they recorded.
    GLint prevProgram = 0;
    glGetIntegerv(GL_CURRENT_PROGRAM, &prevProgram);
    GLint prevViewport[4];
    glGetIntegerv(GL_VIEWPORT, prevViewport);
    GLint prevActiveTexture = 0;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &prevActiveTexture);
    glActiveTexture(GL_TEXTURE0);
    GLint prevBoundTexture = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevBoundTexture);
    const GLboolean blendWasEnabled = glIsEnabled(GL_BLEND);
#if defined(__APPLE__) || defined(USE_OPENGLES)
    GLint prevVao = 0;
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prevVao);
    glBindVertexArray(mPostVao);
#endif

    GdxLogPostShaderInfo(fmt::format("apply: custom='{}' mode={} {}x{} -> {}x{}",
                                      customStem != nullptr ? customStem : "", mode, nativeW, nativeH, outW, outH));

    glBindFramebuffer(GL_FRAMEBUFFER, mFrameBuffers[mPostOutputFb].fbo);
    glViewport(0, 0, (GLint)outW, (GLint)outH);
    if (blendWasEnabled) {
        glDisable(GL_BLEND);
    }
    glUseProgram(prg->program);
    glBindTexture(GL_TEXTURE_2D, mFrameBuffers[mPostDownsampleFb].clrbuf);
    glUniform2f(prg->srcSizeLocation, (float)nativeW, (float)nativeH);
    glUniform2f(prg->outSizeLocation, (float)outW, (float)outH);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    glUseProgram((GLuint)prevProgram);
    glViewport(prevViewport[0], prevViewport[1], prevViewport[2], prevViewport[3]);
    if (blendWasEnabled) {
        glEnable(GL_BLEND);
    }
    glBindTexture(GL_TEXTURE_2D, (GLuint)prevBoundTexture);
    glActiveTexture((GLenum)prevActiveTexture);
#if defined(__APPLE__) || defined(USE_OPENGLES)
    glBindVertexArray((GLuint)prevVao);
#endif

    glBindFramebuffer(GL_FRAMEBUFFER, mFrameBuffers[mCurrentFrameBuffer].fbo);
    if (mLastScissorEnabled != 1) {
        mLastScissorEnabled = 1;
        glEnable(GL_SCISSOR_TEST);
    }

    return (uintptr_t)mFrameBuffers[mPostOutputFb].clrbuf;
}

// Prefer a .slang pass (translated via glslang+SPIRV-Cross); fall back to the
// backend's native .glsl single-file contract so mixed chains work.
static std::filesystem::path GdxResolvePipelineShaderPathOGL(const std::filesystem::path& presetDir,
                                                           const std::string& stem) {
    // lexically_normal: pack presets carry ".." chains that can push the joined
    // path past MAX_PATH even when the real target is short.
    std::filesystem::path slangPath = (presetDir / (stem + ".slang")).lexically_normal();
    if (std::filesystem::exists(slangPath)) {
        return slangPath;
    }
    return (presetDir / (stem + ".glsl")).lexically_normal();
}

// Worker-thread half of the GL pipeline build: file reads and slang translation only. GL program
// compilation must stay on the render thread (no GL context is current anywhere else).
static void GdxBuildPostPipelineWorkerOGL(std::shared_ptr<GdxPostPipelineBuildOGL> build,
                                          GdxPostShaderPipeline pipeline) {
    const std::filesystem::path presetDir = pipeline.presetPath.parent_path();
    build->passes.resize(pipeline.passes.size());
    for (size_t i = 0; i < pipeline.passes.size(); ++i) {
        if (build->cancel.load()) {
            return;
        }
        GdxPostPassBuildOGL& pass = build->passes[i];
        const std::filesystem::path shaderPath = GdxResolvePipelineShaderPathOGL(presetDir, pipeline.passes[i].shader);
        pass.cacheKey = shaderPath.string();
        try {
            pass.mtime = static_cast<uint64_t>(std::filesystem::last_write_time(shaderPath).time_since_epoch().count());
        } catch (...) {
            SPDLOG_ERROR("Pipeline pass shader not found: {}", shaderPath.string());
            build->state = 2;
            return;
        }
        if (shaderPath.extension() == ".slang") {
            GdxSlangTranslation trans;
            try {
                trans = GdxTranslateSlangFile(shaderPath, GdxSlangTarget::Glsl330);
            } catch (const std::exception& e) {
                SPDLOG_ERROR("Slang translation threw for {}: {}", shaderPath.string(), e.what());
                build->state = 2;
                return;
            } catch (...) {
                SPDLOG_ERROR("Slang translation threw for {}", shaderPath.string());
                build->state = 2;
                return;
            }
            GdxDumpSlangTranslation(shaderPath, GdxSlangTarget::Glsl330, trans.source);
            if (trans.hasVertexStage) {
                GdxDumpSlangTranslation(shaderPath.parent_path() / (shaderPath.stem().string() + "-vs.slang"),
                                        GdxSlangTarget::Glsl330, trans.vertexSource);
            }
            if (!trans.ok) {
                SPDLOG_ERROR("Slang translation failed for {}: {}", shaderPath.string(), trans.error);
                build->state = 2;
                return;
            }
            pass.isSlang = true;
            pass.slangParameters = std::move(trans.parameters);
            pass.slangUsedBuiltins = std::move(trans.usedBuiltins);
            pass.vsSource = trans.hasVertexStage ? std::move(trans.vertexSource) : std::string(sGdxPostVsSource330);
            pass.fsSource = std::move(trans.source);
        } else {
            std::string userBody = GdxReadShaderFile(shaderPath);
            if (userBody.empty()) {
                SPDLOG_ERROR("Pipeline pass shader is empty or could not be read: {}", shaderPath.string());
                build->state = 2;
                return;
            }
            pass.vsSource = sGdxPostVsSource;
            pass.fsSource = GdxBuildCustomPostFsSource(userBody);
        }
        pass.ok = true;
    }
    build->state = 1;
}

// Render-thread half: compile the translated sources and resolve uniforms/samplers.
bool GdxInstallPostPipelineBuildOGL(GfxRenderingAPIOGL* self, GdxPostPipelineBuildOGL* build) {
    for (GdxPostPassBuildOGL& pass : build->passes) {
        GfxRenderingAPIOGL::PostShaderProgramOGL prg = {};
        prg.attempted = true;
        prg.mtime = pass.mtime;
        prg.isSlang = pass.isSlang;
        prg.slangParameters = std::move(pass.slangParameters);
        prg.slangUsedBuiltins = std::move(pass.slangUsedBuiltins);
        prg.program = GdxCompilePostProgram(pass.vsSource.c_str(), pass.fsSource.c_str());
        if (prg.program == 0) {
            SPDLOG_ERROR("Pipeline pass shader compile failed: {}", pass.cacheKey);
            self->mPostPipelineProgramCache[pass.cacheKey] = std::move(prg);
            return false;
        }
        self->GdxInitPostProgramOGL(&prg);
        self->mPostPipelineProgramCache[pass.cacheKey] = std::move(prg);
    }
    return true;
}

bool GdxCompilePipelinePassOGL(const std::filesystem::path& path, GfxRenderingAPIOGL* self,
                               GfxRenderingAPIOGL::PostShaderProgramOGL* outPrg) {
    outPrg->isSlang = false;
    outPrg->slangParameters.clear();
    outPrg->slangParameterLocations.clear();
    outPrg->slangUsedBuiltins = {};
    outPrg->slangSamplerLocations.clear();
    outPrg->slangExtraSizeLocations.clear();

    const bool isSlang = path.extension() == ".slang";
    if (isSlang) {
        // glslang/SPIRV-Cross throw on pathological inputs (Mega_Bezel-scale chains);
        // a failed pass must fall back to the unfiltered image, never crash the game.
        GdxSlangTranslation trans;
        try {
            trans = GdxTranslateSlangFile(path, GdxSlangTarget::Glsl330);
        } catch (const std::exception& e) {
            SPDLOG_ERROR("Slang translation threw for {}: {}", path.string(), e.what());
            return false;
        } catch (...) {
            SPDLOG_ERROR("Slang translation threw for {}", path.string());
            return false;
        }
        GdxDumpSlangTranslation(path, GdxSlangTarget::Glsl330, trans.source);
        if (trans.hasVertexStage) {
            GdxDumpSlangTranslation(path.parent_path() / (path.stem().string() + "-vs.slang"), GdxSlangTarget::Glsl330,
                                    trans.vertexSource);
        }
        if (!trans.ok) {
            SPDLOG_ERROR("Slang translation failed for {}: {}", path.string(), trans.error);
            return false;
        }
        outPrg->program = trans.hasVertexStage ? GdxCompilePostProgram(trans.vertexSource.c_str(), trans.source.c_str())
                                               : GdxCompilePostProgram(sGdxPostVsSource330, trans.source.c_str());
        if (outPrg->program == 0) {
            SPDLOG_ERROR("Translated slang compile failed: {}", path.string());
            return false;
        }
        outPrg->isSlang = true;
        outPrg->slangParameters = std::move(trans.parameters);
        outPrg->slangUsedBuiltins = std::move(trans.usedBuiltins);
        self->GdxInitPostProgramOGL(outPrg);
        return true;
    }

    std::string userBody = GdxReadShaderFile(path);
    if (userBody.empty()) {
        SPDLOG_ERROR("Pipeline pass shader is empty or could not be read: {}", path.string());
        return false;
    }
    std::string fsSource = GdxBuildCustomPostFsSource(userBody);
    outPrg->program = GdxCompilePostProgram(sGdxPostVsSource, fsSource.c_str());
    if (outPrg->program == 0) {
        SPDLOG_ERROR("Pipeline pass shader compile failed: {}", path.string());
        return false;
    }
    self->GdxInitPostProgramOGL(outPrg);
    return true;
}

static GLenum GdxPipelineWrapModeOGL(GdxPostPassDesc::WrapMode mode) {
    switch (mode) {
        case GdxPostPassDesc::WrapMode::ClampToBorder:
            return GL_CLAMP_TO_BORDER;
        case GdxPostPassDesc::WrapMode::Repeat:
            return GL_REPEAT;
        case GdxPostPassDesc::WrapMode::MirroredRepeat:
            return GL_MIRRORED_REPEAT;
        case GdxPostPassDesc::WrapMode::ClampToEdge:
        default:
            return GL_CLAMP_TO_EDGE;
    }
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

// Load the pipeline's LUT textures into OpenGL textures. Returns false and logs
// on failure; missing files are tolerated (the shader just samples black).
bool GdxLoadPipelineLutsOGL(const GdxPostShaderPipeline& pipeline, std::vector<GLuint>* outTextures,
                            std::vector<std::filesystem::path>* outPaths,
                            std::vector<std::pair<uint32_t, uint32_t>>* outSizes) {
    const std::filesystem::path presetDir = pipeline.presetPath.parent_path();

    outTextures->clear();
    outPaths->clear();
    outSizes->clear();
    outTextures->reserve(pipeline.textureOrder.size());
    outPaths->reserve(pipeline.textureOrder.size());
    outSizes->reserve(pipeline.textureOrder.size());

    for (const std::string& name : pipeline.textureOrder) {
        auto it = pipeline.textures.find(name);
        if (it == pipeline.textures.end()) {
            outTextures->push_back(0);
            outPaths->push_back(std::filesystem::path());
            outSizes->push_back({ 0, 0 });
            continue;
        }

        std::filesystem::path imagePath = (presetDir / it->second.path).lexically_normal();
        outPaths->push_back(imagePath);

        int w = 0, h = 0, channels = 0;
        stbi_uc* pixels = stbi_load(imagePath.string().c_str(), &w, &h, &channels, 4);
        if (pixels == nullptr) {
            SPDLOG_WARN("Could not load LUT texture '{}': {}", imagePath.string(), stbi_failure_reason());
            outTextures->push_back(0);
            outSizes->push_back({ 0, 0 });
            continue;
        }

        GLuint tex = 0;
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, it->second.linear ? GL_LINEAR : GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, it->second.linear ? GL_LINEAR : GL_NEAREST);
        const GLenum wrap = GdxPipelineWrapModeOGL(it->second.wrap);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap);
        glBindTexture(GL_TEXTURE_2D, 0);
        stbi_image_free(pixels);

        outTextures->push_back(tex);
        outSizes->push_back({ static_cast<uint32_t>(w), static_cast<uint32_t>(h) });
    }
    return true;
}

GdxSlangUsedBuiltins GdxUnionUsedBuiltins(const std::vector<GfxRenderingAPIOGL::PostShaderProgramOGL*>& programs) {
    GdxSlangUsedBuiltins result;
    for (const GfxRenderingAPIOGL::PostShaderProgramOGL* prg : programs) {
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

uintptr_t GfxRenderingAPIOGL::ApplyPostShaderChain(int srcFbId, const GdxPostShaderPipeline& pipeline,
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
            if (state == 2 || !GdxInstallPostPipelineBuildOGL(this, mPostPipelineBuild.get())) {
                mPostPipelineFailedKey = chainKey;
                mPostPipelineBuild.reset();
                return 0;
            }
            mPostPipelineBuild.reset();
            mPostPipelineReadyKey = chainKey;
        } else {
            bool allCached = true;
            for (size_t i = 0; i < pipeline.passes.size(); ++i) {
                const std::filesystem::path sp = GdxResolvePipelineShaderPathOGL(presetDir, pipeline.passes[i].shader);
                uint64_t mt = 0;
                try {
                    mt = static_cast<uint64_t>(std::filesystem::last_write_time(sp).time_since_epoch().count());
                } catch (...) {
                    allCached = false;
                    break;
                }
                auto it = mPostPipelineProgramCache.find(sp.string());
                if (it == mPostPipelineProgramCache.end() || it->second.mtime != mt || it->second.program == 0) {
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
                auto build = std::make_shared<GdxPostPipelineBuildOGL>();
                build->key = chainKey;
                std::thread(GdxBuildPostPipelineWorkerOGL, build, pipeline).detach();
                mPostPipelineBuild = std::move(build);
                return 0;
            }
        }
    }

    // Compile (or retrieve cached) programs for every pass. The cache key is the full shader path
    // so the same pass file reused across presets still reloads correctly when its mtime changes.
    std::vector<PostShaderProgramOGL*> programs;
    programs.reserve(pipeline.passes.size());
    for (size_t i = 0; i < pipeline.passes.size(); ++i) {
        const std::string& shaderStem = pipeline.passes[i].shader;
        const std::filesystem::path shaderPath = GdxResolvePipelineShaderPathOGL(presetDir, shaderStem);
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
            PostShaderProgramOGL prg = {};
            prg.attempted = true;
            prg.mtime = mtimeTicks;
            if (!GdxCompilePipelinePassOGL(shaderPath, this, &prg)) {
                mPostPipelineProgramCache[cacheKey] = std::move(prg);
                return 0;
            }
            mPostPipelineProgramCache[cacheKey] = std::move(prg);
            it = mPostPipelineProgramCache.find(cacheKey);
        }
        if (it->second.program == 0) {
            return 0;
        }
        programs.push_back(&it->second);
    }

    const GdxSlangUsedBuiltins used = GdxUnionUsedBuiltins(programs);
    const size_t historySize = used.maxOriginalHistory >= 0 ? static_cast<size_t>(used.maxOriginalHistory + 1) : 0;

    // Allocate N+1 framebuffers: [0] is the first-pass input (bottom-up, native size), [1..N-1]
    // are intermediate pass outputs (bottom-up), and [N] is the final pass output (top-down).
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
    for (const PostShaderProgramOGL* prg : programs) {
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
    bool lutsChanged = mPostPipelineLutTextures.size() != pipeline.textureOrder.size();
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
        for (GLuint tex : mPostPipelineLutTextures) {
            if (tex != 0) {
                glDeleteTextures(1, &tex);
            }
        }
        GdxLoadPipelineLutsOGL(pipeline, &mPostPipelineLutTextures, &mPostPipelineLutPaths, &mPostPipelineLutSizes);
    }

#if defined(__APPLE__) || defined(USE_OPENGLES)
    if (mPostVao == 0) {
        glGenVertexArrays(1, &mPostVao);
    }
#endif

    // Save GL state that the pass draw will touch.
    GLint prevProgram = 0;
    glGetIntegerv(GL_CURRENT_PROGRAM, &prevProgram);
    GLint prevViewport[4];
    glGetIntegerv(GL_VIEWPORT, prevViewport);
    GLint prevActiveTexture = 0;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &prevActiveTexture);
    glActiveTexture(GL_TEXTURE0);
    GLint prevBoundTexture = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevBoundTexture);
    const GLboolean blendWasEnabled = glIsEnabled(GL_BLEND);
#if defined(__APPLE__) || defined(USE_OPENGLES)
    GLint prevVao = 0;
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prevVao);
    glBindVertexArray(mPostVao);
#endif
    if (mLastScissorEnabled != 0) {
        mLastScissorEnabled = 0;
        glDisable(GL_SCISSOR_TEST);
    }

    // The pass chain runs top-down end to end, matching D3D11: translated slang passes sample
    // with the vUV varying directly, so a bottom-up first input renders every chain upside down
    // on GL. FB0 is top-down like the game FB; the blit flips only when the source is bottom-up.
    UpdateFramebufferParameters(mPostPipelineFbs[0], nativeW, nativeH, 1, true, true, false, false,
                                GdxFramebufferFormat::R8G8B8A8_UNORM);
    {
        const FramebufferOGL& src = mFrameBuffers[srcFbId];
        glBindFramebuffer(GL_READ_FRAMEBUFFER, src.fbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, mFrameBuffers[mPostPipelineFbs[0]].fbo);
        GLint dstY0 = 0, dstY1 = (GLint)nativeH;
        if (!src.invertY) {
            std::swap(dstY0, dstY1);
        }
        glBlitFramebuffer(0, 0, src.width, src.height, 0, dstY0, (GLint)nativeW, dstY1, GL_COLOR_BUFFER_BIT,
                          GL_LINEAR);
    }

    // Snapshot the current source frame into the history ring before the passes run.
    if (historySize > 0) {
        const int historyFb = mPostPipelineHistoryFbs[mPostPipelineHistoryIndex % historySize];
        UpdateFramebufferParameters(historyFb, nativeW, nativeH, 1, true, true, false, false,
                                    GdxFramebufferFormat::R8G8B8A8_UNORM);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, mFrameBuffers[mPostPipelineFbs[0]].fbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, mFrameBuffers[historyFb].fbo);
        glBlitFramebuffer(0, 0, (GLint)nativeW, (GLint)nativeH, 0, 0, (GLint)nativeW, (GLint)nativeH,
                          GL_COLOR_BUFFER_BIT, GL_LINEAR);
        ++mPostPipelineHistoryIndex;
    }

    auto bindSamplerOGL = [&](int unit, GLuint tex, bool linear, GdxPostPassDesc::WrapMode wrapS,
                              GdxPostPassDesc::WrapMode wrapT) {
        glActiveTexture(GL_TEXTURE0 + unit);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, linear ? GL_LINEAR : GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, linear ? GL_LINEAR : GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GdxPipelineWrapModeOGL(wrapS));
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GdxPipelineWrapModeOGL(wrapT));
    };

    // Run the pass chain. Each pass samples its input as bottom-up and writes to a bottom-up
    // output, except the final pass which writes top-down so DrawGame presents it correctly.
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
            // Primary source sampler.
            bindSamplerOGL(0, mFrameBuffers[srcFb].clrbuf, pass.filterLinear, pass.wrapS, pass.wrapT);

            // OriginalHistoryN: previous N source frames, oldest first.
            for (int n = 0; n <= programs[i]->slangUsedBuiltins.maxOriginalHistory; ++n) {
                const size_t idx = (mPostPipelineHistoryIndex - 1 - n + historySize) % historySize;
                const int histFb = mPostPipelineHistoryFbs[idx];
                bindSamplerOGL(GdxSlangTextureBindings::OriginalHistory(n), mFrameBuffers[histFb].clrbuf, true,
                               GdxPostPassDesc::WrapMode::ClampToEdge, GdxPostPassDesc::WrapMode::ClampToEdge);
            }

            // PassOutputN: same-frame output of pass N.
            for (int n : programs[i]->slangUsedBuiltins.passOutputIndices) {
                if (n >= 0 && n < (int)mPostPipelineFbs.size() - 1) {
                    const int outFb = mPostPipelineFbs[n + 1];
                    bindSamplerOGL(GdxSlangTextureBindings::PassOutput(n), mFrameBuffers[outFb].clrbuf, true,
                                   GdxPostPassDesc::WrapMode::ClampToEdge, GdxPostPassDesc::WrapMode::ClampToEdge);
                }
            }

            // PassFeedbackN: previous frame's output of pass N.
            for (int n : programs[i]->slangUsedBuiltins.passFeedbackIndices) {
                if (n >= 0 && n < (int)mPostPipelineFeedbackFbs.size() && mPostPipelineFeedbackFbs[n] >= 0) {
                    const int fbFb = mPostPipelineFeedbackFbs[n];
                    bindSamplerOGL(GdxSlangTextureBindings::PassFeedback(n), mFrameBuffers[fbFb].clrbuf, true,
                                   GdxPostPassDesc::WrapMode::ClampToEdge, GdxPostPassDesc::WrapMode::ClampToEdge);
                }
            }

            // UserN: LUT textures declared in the preset.
            for (int n : programs[i]->slangUsedBuiltins.userTextureIndices) {
                if (n >= 0 && n < (int)mPostPipelineLutTextures.size() && mPostPipelineLutTextures[n] != 0) {
                    auto it = pipeline.textures.find(pipeline.textureOrder[n]);
                    bool linear = true;
                    GdxPostPassDesc::WrapMode wrap = GdxPostPassDesc::WrapMode::ClampToEdge;
                    if (it != pipeline.textures.end()) {
                        linear = it->second.linear;
                        wrap = it->second.wrap;
                    }
                    bindSamplerOGL(GdxSlangTextureBindings::User(n), mPostPipelineLutTextures[n], linear, wrap, wrap);
                }
            }

            // Named samplers: a preset LUT when the name is a `textures` entry,
            // `<Alias>` for that pass's current-frame output, `<Alias>Feedback`
            // for its previous frame.
            for (size_t k = 0; k < programs[i]->slangUsedBuiltins.namedSamplers.size(); ++k) {
                const std::string& samplerName = programs[i]->slangUsedBuiltins.namedSamplers[k];
                auto texIt = pipeline.textures.find(samplerName);
                if (texIt != pipeline.textures.end()) {
                    const auto orderIt =
                        std::find(pipeline.textureOrder.begin(), pipeline.textureOrder.end(), samplerName);
                    const size_t lutIdx = static_cast<size_t>(orderIt - pipeline.textureOrder.begin());
                    if (orderIt != pipeline.textureOrder.end() && lutIdx < mPostPipelineLutTextures.size() &&
                        mPostPipelineLutTextures[lutIdx] != 0) {
                        bindSamplerOGL(GdxSlangTextureBindings::Named(static_cast<int>(k)),
                                       mPostPipelineLutTextures[lutIdx], texIt->second.linear, texIt->second.wrap,
                                       texIt->second.wrap);
                    }
                } else {
                    std::string alias = samplerName;
                    bool wantsFeedback = false;
                    if (alias.size() > 8 && alias.compare(alias.size() - 8, 8, "Feedback") == 0) {
                        wantsFeedback = true;
                        alias = alias.substr(0, alias.size() - 8);
                    }
                    const int passIdx = GdxFindPipelinePassByAlias(pipeline, alias);
                    int fb = -1;
                    if (passIdx >= 0) {
                        if (wantsFeedback) {
                            if (passIdx < (int)mPostPipelineFeedbackFbs.size()) {
                                fb = mPostPipelineFeedbackFbs[passIdx];
                            }
                        } else if (passIdx + 1 < (int)mPostPipelineFbs.size()) {
                            fb = mPostPipelineFbs[passIdx + 1];
                        }
                    }
                    if (fb >= 0) {
                        bindSamplerOGL(GdxSlangTextureBindings::Named(static_cast<int>(k)),
                                       mFrameBuffers[fb].clrbuf, true, GdxPostPassDesc::WrapMode::ClampToEdge,
                                       GdxPostPassDesc::WrapMode::ClampToEdge);
                    }
                }
            }

            glActiveTexture(GL_TEXTURE0);
        }

        if (!shouldSkip) {
            glBindFramebuffer(GL_FRAMEBUFFER, mFrameBuffers[dstFb].fbo);
            glViewport(0, 0, (GLint)dstW, (GLint)dstH);
            if (blendWasEnabled) {
                glDisable(GL_BLEND);
            }
            glUseProgram(programs[i]->program);
            glBindTexture(GL_TEXTURE_2D, mFrameBuffers[srcFb].clrbuf);
            if (programs[i]->isSlang) {
                // Slang sizes follow the vec4 convention: xy = size, zw = 1/size.
                glUniform4f(programs[i]->srcSizeLocation, (float)srcW, (float)srcH, 1.0f / (float)srcW,
                            1.0f / (float)srcH);
                glUniform4f(programs[i]->outSizeLocation, (float)dstW, (float)dstH, 1.0f / (float)dstW,
                            1.0f / (float)dstH);
            } else {
                glUniform2f(programs[i]->srcSizeLocation, (float)srcW, (float)srcH);
                glUniform2f(programs[i]->outSizeLocation, (float)dstW, (float)dstH);
            }
            if (programs[i]->isSlang) {
                if (programs[i]->slangFrameCountLocation >= 0) {
                    glUniform1ui(programs[i]->slangFrameCountLocation, mFrameCount);
                }
                if (programs[i]->slangMvpLocation >= 0) {
                    const float identity[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
                    glUniformMatrix4fv(programs[i]->slangMvpLocation, 1, GL_FALSE, identity);
                }
                // Resolve effective parameter value: CVar > .slangp override > default.
                const std::string presetStem = GdxPostShaderCvarStem(
                    std::filesystem::path(Ship::Context::GetAppDirectoryPath()) / "shaders", pipeline.presetPath);
                for (const GdxSlangParameter& p : programs[i]->slangParameters) {
                    auto locIt = programs[i]->slangParameterLocations.find(p.name);
                    if (locIt == programs[i]->slangParameterLocations.end() || locIt->second < 0) {
                        continue;
                    }
                    float value = p.defaultValue;
                    auto overrideIt = pipeline.parameterOverrides.find(p.name);
                    if (overrideIt != pipeline.parameterOverrides.end()) {
                        value = overrideIt->second;
                    }
                    const std::string cvarName = "gEnhancements.Graphics.PipelineParam." + presetStem + "." + p.name;
                    // The menu sliders write Float CVars; CVarGetString would return "" for them.
                    if (CVarGet(cvarName.c_str()) != nullptr) {
                        value = CVarGetFloat(cvarName.c_str(), p.defaultValue);
                    }
                    glUniform1f(locIt->second, value);
                }
                // `<Name>Size` vec4s, resolved against the same sources as the
                // D3D11 cbuffer fill. History and unrecognized names fall back to
                // the native source size.
                static const std::regex passOutSizeRe(R"(^Pass(?:Output|Feedback)(\d+)Size$)");
                static const std::regex userSizeRe(R"(^User(\d+)Size$)");
                for (const auto& sizeEntry : programs[i]->slangExtraSizeLocations) {
                    if (sizeEntry.second < 0) {
                        continue;
                    }
                    const std::string& sizeName = sizeEntry.first;
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
                        if (n >= 0 && n < (int)mPostPipelineLutSizes.size() &&
                            mPostPipelineLutSizes[n].first > 0) {
                            sizeW = mPostPipelineLutSizes[n].first;
                            sizeH = mPostPipelineLutSizes[n].second;
                        }
                    } else if (sizeName.size() > 4) {
                        const int passIdx =
                            GdxFindPipelinePassByAlias(pipeline, sizeName.substr(0, sizeName.size() - 4));
                        if (passIdx >= 0) {
                            sizeW = passOutSizes[passIdx].first;
                            sizeH = passOutSizes[passIdx].second;
                        }
                    }
                    glUniform4f(sizeEntry.second, (float)sizeW, (float)sizeH, 1.0f / (float)sizeW,
                                1.0f / (float)sizeH);
                }
            }
            glDrawArrays(GL_TRIANGLES, 0, 3);
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
            glBindFramebuffer(GL_READ_FRAMEBUFFER, mFrameBuffers[dstFb].fbo);
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, mFrameBuffers[fbFb].fbo);
            glBlitFramebuffer(0, 0, (GLint)dstW, (GLint)dstH, 0, 0, (GLint)dstW, (GLint)dstH, GL_COLOR_BUFFER_BIT,
                              GL_LINEAR);
        }
    }

    // Restore state so the interpreter's caches stay consistent with the GL context.
    glUseProgram((GLuint)prevProgram);
    glViewport(prevViewport[0], prevViewport[1], prevViewport[2], prevViewport[3]);
    if (blendWasEnabled) {
        glEnable(GL_BLEND);
    }
    glBindTexture(GL_TEXTURE_2D, (GLuint)prevBoundTexture);
    glActiveTexture((GLenum)prevActiveTexture);
#if defined(__APPLE__) || defined(USE_OPENGLES)
    glBindVertexArray((GLuint)prevVao);
#endif

    glBindFramebuffer(GL_FRAMEBUFFER, mFrameBuffers[mCurrentFrameBuffer].fbo);
    if (mLastScissorEnabled != 1) {
        mLastScissorEnabled = 1;
        glEnable(GL_SCISSOR_TEST);
    }

    GdxLogPostShaderInfo(fmt::format("pipeline '{}': {} pass(es), {}x{} -> {}x{}", pipeline.presetPath.filename().string(),
                                     pipeline.passes.size(), nativeW, nativeH, outW, outH));

    return (uintptr_t)mFrameBuffers[mPostPipelineFbs[pipeline.passes.size()]].clrbuf;
}

void GfxRenderingAPIOGL::CopyFramebuffer(int fb_dst_id, int fb_src_id, int srcX0, int srcY0, int srcX1, int srcY1,
                                         int dstX0, int dstY0, int dstX1, int dstY1) {
    if (fb_dst_id >= (int)mFrameBuffers.size() || fb_src_id >= (int)mFrameBuffers.size()) {
        return;
    }

    FramebufferOGL src = mFrameBuffers[fb_src_id];
    const FramebufferOGL& dst = mFrameBuffers[fb_dst_id];

    // Adjust y values for non-inverted source frame buffers because opengl uses bottom left for origin
    if (!src.invertY) {
        int temp = srcY1 - srcY0;
        srcY1 = src.height - srcY0;
        srcY0 = srcY1 - temp;
    }

    // Flip the y values
    if (src.invertY != dst.invertY) {
        std::swap(srcY0, srcY1);
    }

    // Disabled for blit
    if (mLastScissorEnabled != 0) {
        mLastScissorEnabled = 0;
        glDisable(GL_SCISSOR_TEST);
    }

    // For msaa enabled buffers we can't perform a scaled blit to a simple sample buffer
    // First do an unscaled blit to a msaa resolved buffer
    //
    // OR, not AND: glBlitFramebuffer rejects any scaled blit from a multisampled read
    // framebuffer, and a blit is scaled the moment one axis differs. With AND, a source matching
    // on exactly one axis skipped this pre-resolve and the blit below copied nothing.
    if ((src.height != dst.height || src.width != dst.width) && src.msaa_level > 1) {
        // Start with the main buffer (0) as the msaa resolved buffer
        int fb_resolve_id = 0;
        FramebufferOGL fb_resolve = mFrameBuffers[fb_resolve_id];

        // If the size doesn't match our source, then we need to use our separate color msaa resolved buffer (2)
        if (fb_resolve.height != src.height || fb_resolve.width != src.width) {
            if (mFrameBuffers.size() <= 2) {
                // Index 2 is the interpreter's mGameFbMsaaResolved, which exists for the life of
                // an initialised interpreter, so this never fires today. Widening the size test
                // above to OR makes the block reachable for sources it previously skipped, and an
                // unconditional mFrameBuffers[2] is one caller away from reading past the end.
                // Without a staging target there is nothing to pre-resolve into, so give up after
                // restoring the scissor state disabled just above.
                if (mLastScissorEnabled != 1) {
                    mLastScissorEnabled = 1;
                    glEnable(GL_SCISSOR_TEST);
                }
                return;
            }
            fb_resolve_id = 2;
            fb_resolve = mFrameBuffers[fb_resolve_id];
        }

        glBindFramebuffer(GL_READ_FRAMEBUFFER, src.fbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fb_resolve.fbo);

        GLint dstY0 = 0, dstY1 = (GLint)src.height;
        if (src.invertY != fb_resolve.invertY) {
            std::swap(dstY0, dstY1);
        }
        glBlitFramebuffer(0, 0, src.width, src.height, 0, dstY0, src.width, dstY1, GL_COLOR_BUFFER_BIT, GL_NEAREST);

        // Switch source buffer to the resolved sample
        fb_src_id = fb_resolve_id;
        src = fb_resolve;
    }

    glBindFramebuffer(GL_READ_FRAMEBUFFER, src.fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, dst.fbo);

    // The 0 buffer is a double buffer so we need to choose the back to avoid imgui elements
    if (fb_src_id == 0) {
        glReadBuffer(GL_BACK);
    } else {
        glReadBuffer(GL_COLOR_ATTACHMENT0);
    }

    glBlitFramebuffer(srcX0, srcY0, srcX1, srcY1, dstX0, dstY0, dstX1, dstY1, GL_COLOR_BUFFER_BIT, GL_NEAREST);

    glBindFramebuffer(GL_FRAMEBUFFER, mFrameBuffers[mCurrentFrameBuffer].fbo);

    // Read-buffer state is per framebuffer object and GL_BACK exists only on the default
    // framebuffer, so issuing this under a user FBO raises an error and changes nothing.
    if (mCurrentFrameBuffer == 0) {
        glReadBuffer(GL_BACK);
    }

    if (mLastScissorEnabled != 1) {
        mLastScissorEnabled = 1;
        glEnable(GL_SCISSOR_TEST);
    }
}

void GfxRenderingAPIOGL::ReadFramebufferToCPU(int fb_id, uint32_t width, uint32_t height, uint16_t* rgba16_buf) {
    if (fb_id >= (int)mFrameBuffers.size()) {
        return;
    }

    const FramebufferOGL& fb = mFrameBuffers[fb_id];

    // The requested output size is usually not this framebuffer's real size -- transition
    // captures ask for a fixed 320x240 while the source is resized to the window every frame --
    // so glReadPixels(0, 0, width, height) would read a top-left crop. Read the full framebuffer
    // at fb.width/fb.height and resample, as the DX11 path does.
    const uint32_t actualW = std::max<uint32_t>(fb.width, 1u);
    const uint32_t actualH = std::max<uint32_t>(fb.height, 1u);

    // Row-order contract: output row 0 is the top of the scene, matching the DX11 readback the
    // transition consumer is validated against. An invertY framebuffer already stores the image
    // top-down, because the interpreter negates vertex Y at render time, so only a conventional
    // bottom-up framebuffer needs the flip. Adding one unconditionally rendered the Linux
    // transition wipes upside-down.
    const bool flipY = !fb.invertY;

    // Read as RGBA8 (GL_UNSIGNED_BYTE) then convert to RGBA16 (5551).
    // GL_RGBA + GL_UNSIGNED_SHORT_5_5_5_1 writes 4 separate u16 components per pixel
    // (8 bytes) on some drivers (NVIDIA), not the packed 2 bytes the spec implies.
    // Reading as RGBA8 and converting matches the DX11 path's approach.
    glBindFramebuffer(GL_FRAMEBUFFER, fb.fbo);

    // Coverage bit, not alpha, matching the DX11 readback: host alpha is often 0 for fully
    // opaque pixels, and the game redraws captured frames through alpha-compare passes, which
    // would then discard every texel.
    if (actualW == width && actualH == height && !flipY) {
        // Sizes already match and no flip is needed, so read straight into the destination.
        std::vector<uint8_t> rgba8((size_t)width * height * 4);
        glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, rgba8.data());

        for (uint32_t i = 0; i < width * height; i++) {
            // Rounded 8->5-bit reduction, matching the DX11 readback. A truncating `v >> 3`
            // biases every channel down by up to half a step, which made GL captures visibly
            // darker than DX11 once redrawn as a transition wipe. The +4 form saturates on its
            // own (255 -> 31), so no mask is needed; uint32_t keeps the * 0x1F from overflowing.
            uint8_t r = (uint8_t)((((uint32_t)rgba8[i * 4 + 0] + 4) * 0x1F) / 0xFF);
            uint8_t g = (uint8_t)((((uint32_t)rgba8[i * 4 + 1] + 4) * 0x1F) / 0xFF);
            uint8_t b = (uint8_t)((((uint32_t)rgba8[i * 4 + 2] + 4) * 0x1F) / 0xFF);
            rgba16_buf[i] = (r << 11) | (g << 6) | (b << 1) | 1;
        }
    } else {
        // Box-filter average, not nearest-neighbor, matching the DX11 path: decimating a 6:1
        // transition capture shreds high-frequency art into disconnected dashes. No aspect crop
        // is needed, since the transition redraws the capture stretched back across the full
        // viewport (decomp ovl_i2/transition.c, G_EX_WIDESCREEN_STRETCH).
        std::vector<uint8_t> rgba8((size_t)actualW * actualH * 4);
        glReadPixels(0, 0, actualW, actualH, GL_RGBA, GL_UNSIGNED_BYTE, rgba8.data());

        for (uint32_t j = 0; j < height; j++) {
            uint32_t sy0 = j * actualH / height;
            uint32_t sy1 = (j + 1) * actualH / height;
            if (sy1 <= sy0) sy1 = sy0 + 1;
            if (sy1 > actualH) sy1 = actualH;
            for (uint32_t i = 0; i < width; i++) {
                uint32_t sx0 = i * actualW / width;
                uint32_t sx1 = (i + 1) * actualW / width;
                if (sx1 <= sx0) sx1 = sx0 + 1;
                if (sx1 > actualW) sx1 = actualW;

                uint32_t accR = 0, accG = 0, accB = 0, count = 0;
                for (uint32_t syLogical = sy0; syLogical < sy1; syLogical++) {
                    // Logical top-down row to glReadPixels' bottom-left physical order, per the
                    // row-order contract above.
                    uint32_t syPhys = flipY ? (actualH - 1 - syLogical) : syLogical;
                    const uint8_t* srcRow = rgba8.data() + (size_t)syPhys * actualW * 4;
                    for (uint32_t sx = sx0; sx < sx1; sx++) {
                        accR += srcRow[sx * 4 + 0];
                        accG += srcRow[sx * 4 + 1];
                        accB += srcRow[sx * 4 + 2];
                        ++count;
                    }
                }
                // Same rounded 8->5-bit reduction as the fast path, on the averaged channel.
                uint8_t r = count ? (uint8_t)((((accR / count) + 4) * 0x1F) / 0xFF) : 0;
                uint8_t g = count ? (uint8_t)((((accG / count) + 4) * 0x1F) / 0xFF) : 0;
                uint8_t b = count ? (uint8_t)((((accB / count) + 4) * 0x1F) / 0xFF) : 0;
                rgba16_buf[i + j * width] = (r << 11) | (g << 6) | (b << 1) | 1;
            }
        }
    }

    glBindFramebuffer(GL_FRAMEBUFFER, mFrameBuffers[mCurrentFrameBuffer].fbo);
}

std::unordered_map<std::pair<float, float>, uint16_t, hash_pair_ff>
GfxRenderingAPIOGL::GetPixelDepth(int fb_id, const std::set<std::pair<float, float>>& coordinates) {
    std::unordered_map<std::pair<float, float>, uint16_t, hash_pair_ff> res;

    FramebufferOGL& fb = mFrameBuffers[fb_id];

    // When looking up one value and the framebuffer is single-sampled, we can read pixels directly
    // Otherwise we need to blit first to a new buffer then read it
    if (coordinates.size() == 1 && fb.msaa_level <= 1) {
        uint32_t depth_stencil_value;
        glBindFramebuffer(GL_FRAMEBUFFER, fb.fbo);
        int x = coordinates.begin()->first;
        int y = coordinates.begin()->second;
#ifndef USE_OPENGLES // not supported on gles. Runs fine without it, but this may cause issues
        glReadPixels(x, fb.invertY ? fb.height - y : y, 1, 1, GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8,
                     &depth_stencil_value);
#endif
        res.emplace(*coordinates.begin(), (depth_stencil_value >> 18) << 2);
    } else {
        if (mPixelDepthRbSize < coordinates.size()) {
            // Resizing a renderbuffer seems broken with Intel's driver, so recreate one instead.
            glBindFramebuffer(GL_FRAMEBUFFER, mPixelDepthFb);
            glDeleteRenderbuffers(1, &mPixelDepthRb);
            glGenRenderbuffers(1, &mPixelDepthRb);
            glBindRenderbuffer(GL_RENDERBUFFER, mPixelDepthRb);
            glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, coordinates.size(), 1);
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, mPixelDepthRb);
            glBindRenderbuffer(GL_RENDERBUFFER, 0);

            mPixelDepthRbSize = coordinates.size();
        }

        glBindFramebuffer(GL_READ_FRAMEBUFFER, fb.fbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, mPixelDepthFb);

        // Through the cached flag, like ClearFramebuffer and CopyFramebuffer. A bare glDisable
        // left mLastScissorEnabled reading 1 with the test off, so every later draw ignored its
        // scissor rect while the cache believed the glEnable was redundant.
        if (mLastScissorEnabled != 0) {
            mLastScissorEnabled = 0;
            glDisable(GL_SCISSOR_TEST); // needed for the blit operation
        }

        {
            size_t i = 0;
            for (const auto& coord : coordinates) {
                int x = coord.first;
                int y = coord.second;
                if (fb.invertY) {
                    y = fb.height - y;
                }
                glBlitFramebuffer(x, y, x + 1, y + 1, i, 0, i + 1, 1, GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT,
                                  GL_NEAREST);
                ++i;
            }
        }

        glBindFramebuffer(GL_READ_FRAMEBUFFER, mPixelDepthFb);
        std::vector<uint32_t> depth_stencil_values(coordinates.size());
#ifndef USE_OPENGLES // not supported on gles. Runs fine without it, but this may cause issues
        glReadPixels(0, 0, coordinates.size(), 1, GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, depth_stencil_values.data());
#endif
        {
            size_t i = 0;
            for (const auto& coord : coordinates) {
                res.emplace(coord, (depth_stencil_values[i++] >> 18) << 2);
            }
        }
    }

    // Index, not GL name -- same as in ResolveMSAAColorBuffer above.
    glBindFramebuffer(GL_FRAMEBUFFER, mFrameBuffers[mCurrentFrameBuffer].fbo);

    // Restore what the blit path above disabled; a no-op for the direct-read path.
    if (mLastScissorEnabled != 1) {
        mLastScissorEnabled = 1;
        glEnable(GL_SCISSOR_TEST);
    }

    return res;
}

void GfxRenderingAPIOGL::SetTextureFilter(FilteringMode mode) {
    gfx_texture_cache_clear();
    mCurrentFilterMode = mode;
}

FilteringMode GfxRenderingAPIOGL::GetTextureFilter() {
    return mCurrentFilterMode;
}

void GfxRenderingAPIOGL::SetSrgbMode() {
    mSrgbMode = true;
}

ImTextureID GfxRenderingAPIOGL::GetTextureById(int id) {
    return reinterpret_cast<ImTextureID>(id);
}
} // namespace Fast
#endif

#pragma clang diagnostic pop
