#include <stdio.h>
#include <math.h>

#include <algorithm>

#if defined(ENABLE_OPENGL) || defined(__APPLE__)

#ifdef __MINGW32__
#define FOR_WINDOWS 1
#else
#define FOR_WINDOWS 0
#endif

#include "ship/Context.h"
#include "ship/config/ConsoleVariable.h"
#include "ship/controller/controldeck/ControlDeck.h"
#include "ship/window/FileDropMgr.h"
#include "fast/backends/gfx_sdl.h"

#ifdef __OpenBSD__
#include <sys/sysctl.h>
#include <sys/time.h>
#endif

#if FOR_WINDOWS
#include <GL/glew.h>
#include "SDL.h"
#define GL_GLEXT_PROTOTYPES 1
#include "SDL_opengl.h"
#elif __APPLE__
#include <SDL.h>
#include "fast/backends/gfx_metal.h"
#include "ship/utils/macUtils.h"
#else
#include <SDL2/SDL.h>
#define GL_GLEXT_PROTOTYPES 1
#include <SDL2/SDL_opengles2.h>
#endif

#include "ship/window/gui/Gui.h"
#include "fast/Fast3dGui.h"
#include <imgui.h> // touchscreen -> ImGui mouse feed (ImGui's SDL2 backend ignores SDL_FINGER events)

#ifdef _WIN32
#include <WTypesbase.h>
#include <Windows.h>
#include <SDL_syswm.h>
#endif

#define GFX_BACKEND_NAME "SDL"
#define _100NANOSECONDS_IN_SECOND 10000000

// Temporary instrumentation for the touchscreen bug in the enhancement menu: taps highlight a
// widget but never activate it on real SDL2 under Wayland, while the same code works against
// sdl2-compat. Two guesses at the cause were wrong, so this traces what actually arrives instead.
// Set GDX_DIAG_TOUCH=1 to enable; the check is one cached read, and nothing prints otherwise.
//
//   GDX_DIAG_TOUCH=1 ./G-Diffuser baserom.us.rev0.z64 > touch.log 2>&1
//
// Logging the ImGui state alongside each event is the point: it shows whether a press ever
// reaches a widget (active=1) or is discarded because the pointer sat somewhere else.
static bool GdxTouchDiagEnabled() {
    static const bool enabled = []() {
        const char* value = SDL_getenv("GDX_DIAG_TOUCH");
        return value != nullptr && value[0] != '\0' && value[0] != '0';
    }();
    return enabled;
}

static void GdxTracePointer(const char* what, float x, float y, int which, const char* note = "") {
    // The context check matters: input events can arrive before Gui::Init has created one.
    if (!GdxTouchDiagEnabled() || ImGui::GetCurrentContext() == nullptr) {
        return;
    }
    const ImGuiIO& io = ImGui::GetIO();
    SDL_Log("[touch] %-16s (%7.1f,%7.1f) which=%-6d | io.pos=(%7.1f,%7.1f) down=%d hovered=%d active=%d %s",
            what, x, y, which, io.MousePos.x, io.MousePos.y, io.MouseDown[0] ? 1 : 0,
            ImGui::IsAnyItemHovered() ? 1 : 0, ImGui::IsAnyItemActive() ? 1 : 0, note);
}

#ifdef _WIN32
LONG_PTR SDL_WndProc;
#endif

namespace Fast {
const SDL_Scancode lus_to_sdl_table[] = {
    SDL_SCANCODE_UNKNOWN,
    SDL_SCANCODE_ESCAPE,
    SDL_SCANCODE_1,
    SDL_SCANCODE_2,
    SDL_SCANCODE_3,
    SDL_SCANCODE_4,
    SDL_SCANCODE_5,
    SDL_SCANCODE_6, /* 0 */
    SDL_SCANCODE_7,
    SDL_SCANCODE_8,
    SDL_SCANCODE_9,
    SDL_SCANCODE_0,
    SDL_SCANCODE_MINUS,
    SDL_SCANCODE_EQUALS,
    SDL_SCANCODE_BACKSPACE,
    SDL_SCANCODE_TAB, /* 0 */

    SDL_SCANCODE_Q,
    SDL_SCANCODE_W,
    SDL_SCANCODE_E,
    SDL_SCANCODE_R,
    SDL_SCANCODE_T,
    SDL_SCANCODE_Y,
    SDL_SCANCODE_U,
    SDL_SCANCODE_I, /* 1 */
    SDL_SCANCODE_O,
    SDL_SCANCODE_P,
    SDL_SCANCODE_LEFTBRACKET,
    SDL_SCANCODE_RIGHTBRACKET,
    SDL_SCANCODE_RETURN,
    SDL_SCANCODE_LCTRL,
    SDL_SCANCODE_A,
    SDL_SCANCODE_S, /* 1 */

    SDL_SCANCODE_D,
    SDL_SCANCODE_F,
    SDL_SCANCODE_G,
    SDL_SCANCODE_H,
    SDL_SCANCODE_J,
    SDL_SCANCODE_K,
    SDL_SCANCODE_L,
    SDL_SCANCODE_SEMICOLON, /* 2 */
    SDL_SCANCODE_APOSTROPHE,
    SDL_SCANCODE_GRAVE,
    SDL_SCANCODE_LSHIFT,
    SDL_SCANCODE_BACKSLASH,
    SDL_SCANCODE_Z,
    SDL_SCANCODE_X,
    SDL_SCANCODE_C,
    SDL_SCANCODE_V, /* 2 */

    SDL_SCANCODE_B,
    SDL_SCANCODE_N,
    SDL_SCANCODE_M,
    SDL_SCANCODE_COMMA,
    SDL_SCANCODE_PERIOD,
    SDL_SCANCODE_SLASH,
    SDL_SCANCODE_RSHIFT,
    SDL_SCANCODE_PRINTSCREEN, /* 3 */
    SDL_SCANCODE_LALT,
    SDL_SCANCODE_SPACE,
    SDL_SCANCODE_CAPSLOCK,
    SDL_SCANCODE_F1,
    SDL_SCANCODE_F2,
    SDL_SCANCODE_F3,
    SDL_SCANCODE_F4,
    SDL_SCANCODE_F5, /* 3 */

    SDL_SCANCODE_F6,
    SDL_SCANCODE_F7,
    SDL_SCANCODE_F8,
    SDL_SCANCODE_F9,
    SDL_SCANCODE_F10,
    SDL_SCANCODE_NUMLOCKCLEAR,
    SDL_SCANCODE_SCROLLLOCK,
    SDL_SCANCODE_HOME, /* 4 */
    SDL_SCANCODE_UP,
    SDL_SCANCODE_PAGEUP,
    SDL_SCANCODE_KP_MINUS,
    SDL_SCANCODE_LEFT,
    SDL_SCANCODE_KP_5,
    SDL_SCANCODE_RIGHT,
    SDL_SCANCODE_KP_PLUS,
    SDL_SCANCODE_END, /* 4 */

    SDL_SCANCODE_DOWN,
    SDL_SCANCODE_PAGEDOWN,
    SDL_SCANCODE_INSERT,
    SDL_SCANCODE_DELETE,
    SDL_SCANCODE_UNKNOWN,
    SDL_SCANCODE_UNKNOWN,
    SDL_SCANCODE_NONUSBACKSLASH,
    SDL_SCANCODE_F11, /* 5 */
    SDL_SCANCODE_F12,
    SDL_SCANCODE_PAUSE,
    SDL_SCANCODE_UNKNOWN,
    SDL_SCANCODE_LGUI,
    SDL_SCANCODE_RGUI,
    SDL_SCANCODE_APPLICATION,
    SDL_SCANCODE_UNKNOWN,
    SDL_SCANCODE_UNKNOWN, /* 5 */

    SDL_SCANCODE_UNKNOWN,
    SDL_SCANCODE_UNKNOWN,
    SDL_SCANCODE_UNKNOWN,
    SDL_SCANCODE_UNKNOWN,
    SDL_SCANCODE_F13,
    SDL_SCANCODE_F14,
    SDL_SCANCODE_F15,
    SDL_SCANCODE_F16, /* 6 */
    SDL_SCANCODE_F17,
    SDL_SCANCODE_F18,
    SDL_SCANCODE_F19,
    SDL_SCANCODE_UNKNOWN,
    SDL_SCANCODE_UNKNOWN,
    SDL_SCANCODE_UNKNOWN,
    SDL_SCANCODE_UNKNOWN,
    SDL_SCANCODE_UNKNOWN, /* 6 */

    SDL_SCANCODE_INTERNATIONAL2,
    SDL_SCANCODE_UNKNOWN,
    SDL_SCANCODE_UNKNOWN,
    SDL_SCANCODE_INTERNATIONAL1,
    SDL_SCANCODE_UNKNOWN,
    SDL_SCANCODE_UNKNOWN,
    SDL_SCANCODE_UNKNOWN,
    SDL_SCANCODE_UNKNOWN, /* 7 */
    SDL_SCANCODE_UNKNOWN,
    SDL_SCANCODE_INTERNATIONAL4,
    SDL_SCANCODE_UNKNOWN,
    SDL_SCANCODE_INTERNATIONAL5,
    SDL_SCANCODE_UNKNOWN,
    SDL_SCANCODE_INTERNATIONAL3,
    SDL_SCANCODE_UNKNOWN,
    SDL_SCANCODE_UNKNOWN /* 7 */
};

const SDL_Scancode scancode_rmapping_extended[][2] = {
    { SDL_SCANCODE_KP_ENTER, SDL_SCANCODE_RETURN },
    { SDL_SCANCODE_RALT, SDL_SCANCODE_LALT },
    { SDL_SCANCODE_RCTRL, SDL_SCANCODE_LCTRL },
    { SDL_SCANCODE_KP_DIVIDE, SDL_SCANCODE_SLASH },
    //{SDL_SCANCODE_KP_PLUS, SDL_SCANCODE_CAPSLOCK}
};

const SDL_Scancode scancode_rmapping_nonextended[][2] = { { SDL_SCANCODE_KP_7, SDL_SCANCODE_HOME },
                                                          { SDL_SCANCODE_KP_8, SDL_SCANCODE_UP },
                                                          { SDL_SCANCODE_KP_9, SDL_SCANCODE_PAGEUP },
                                                          { SDL_SCANCODE_KP_4, SDL_SCANCODE_LEFT },
                                                          { SDL_SCANCODE_KP_6, SDL_SCANCODE_RIGHT },
                                                          { SDL_SCANCODE_KP_1, SDL_SCANCODE_END },
                                                          { SDL_SCANCODE_KP_2, SDL_SCANCODE_DOWN },
                                                          { SDL_SCANCODE_KP_3, SDL_SCANCODE_PAGEDOWN },
                                                          { SDL_SCANCODE_KP_0, SDL_SCANCODE_INSERT },
                                                          { SDL_SCANCODE_KP_PERIOD, SDL_SCANCODE_DELETE },
                                                          { SDL_SCANCODE_KP_MULTIPLY, SDL_SCANCODE_PRINTSCREEN } };

GfxWindowBackendSDL2::~GfxWindowBackendSDL2() {
}

void GfxWindowBackendSDL2::SetFullscreenImpl(bool on, bool call_callback) {
    if (mFullScreen == on) {
        return;
    }

    int display_in_use = SDL_GetWindowDisplayIndex(mWnd);
    if (display_in_use < 0) {
        SPDLOG_WARN("Can't detect on which monitor we are. Probably out of display area?");
        SPDLOG_WARN(SDL_GetError());
    }

    if (on) {
        // OTRTODO: Get mode from config.
        SDL_DisplayMode mode;
        if (SDL_GetDesktopDisplayMode(display_in_use, &mode) >= 0) {
            SDL_SetWindowDisplayMode(mWnd, &mode);
        } else {
            SPDLOG_ERROR(SDL_GetError());
        }
    }

#if defined(__APPLE__)
    // Implement fullscreening with native macOS APIs
    if (on != isNativeMacOSFullscreenActive(mWnd)) {
        toggleNativeMacOSFullscreen(mWnd);
    }
    mFullScreen = on;
#else
    if (SDL_SetWindowFullscreen(
            mWnd, on ? (Ship::Context::GetInstance()->GetConsoleVariables()->GetInteger(CVAR_SDL_WINDOWED_FULLSCREEN, 0)
                            ? SDL_WINDOW_FULLSCREEN_DESKTOP
                            : SDL_WINDOW_FULLSCREEN)
                     : 0) >= 0) {
        mFullScreen = on;
    } else {
        SPDLOG_ERROR("Failed to switch from or to fullscreen mode.");
        SPDLOG_ERROR(SDL_GetError());
    }
#endif

    if (!on) {
        auto conf = Ship::Context::GetInstance()->GetConfig();
        mWindowWidth = conf->GetInt("Window.Width", 640);
        mWindowHeight = conf->GetInt("Window.Height", 480);
        int32_t posX = conf->GetInt("Window.PositionX", 100);
        int32_t posY = conf->GetInt("Window.PositionY", 100);
        if (display_in_use < 0) { // Fallback to default if out of bounds
            posX = 100;
            posY = 100;
        }
        SDL_SetWindowPosition(mWnd, posX, posY);
        SDL_SetWindowSize(mWnd, mWindowWidth, mWindowHeight);
    }

    if (mOnFullscreenChanged != nullptr && call_callback) {
        mOnFullscreenChanged(on);
    }
}

void GfxWindowBackendSDL2::GetActiveWindowRefreshRate(uint32_t* refresh_rate) {
    int display_in_use = SDL_GetWindowDisplayIndex(mWnd);

    SDL_DisplayMode mode;
    SDL_GetCurrentDisplayMode(display_in_use, &mode);
    *refresh_rate = mode.refresh_rate != 0 ? mode.refresh_rate : 60;
}

static uint64_t previous_time;
#ifdef _WIN32
static HANDLE mTimer;
#endif

// True when SDL_GL_SwapWindow already blocks on the hardware refresh, so the software deadline
// wait in SyncFramerateWithTime must be skipped rather than beat against it. File-scope because
// SwapBuffersBegin has the member access to compute it and SyncFramerateWithTime is const.
static bool sVsyncPaced = false;
// Re-queried on a display-index change, and unconditionally once a second: a same-display Hz
// change (OS refresh setting, VRR renegotiation) never moves the index. The timestamp reuses
// previous_time from the wall-clock pacer, so the check itself costs no extra syscall.
static int sCachedDisplayIndex = -1;
static double sCachedRefreshHz = 0.0;
static uint64_t sLastRefreshRequeryTime100ns = 0;
// 1 second expressed in the 100ns ticks previous_time/qpc_to_100ns use.
static constexpr uint64_t kRefreshRequeryInterval100ns = 10000000ull;

#define FRAME_INTERVAL_US_NUMERATOR 1000000
#define FRAME_INTERVAL_US_DENOMINATOR (mTargetFps)

void GfxWindowBackendSDL2::Close() {
    mIsRunning = false;
}

#ifdef _WIN32
static LRESULT CALLBACK gfx_sdl_wnd_proc(HWND h_wnd, UINT message, WPARAM w_param, LPARAM l_param) {
    switch (message) {
        case WM_GETDPISCALEDSIZE:
            // Something is wrong with SDLs original implementation of WM_GETDPISCALEDSIZE, so pass it to the default
            // system window procedure instead.
            return DefWindowProc(h_wnd, message, w_param, l_param);
        case WM_ENDSESSION: {
            GfxWindowBackendSDL2* self =
                reinterpret_cast<GfxWindowBackendSDL2*>(GetWindowLongPtr(h_wnd, GWLP_USERDATA));
            // Apparently SDL2 does not handle this
            if (w_param == TRUE) {
                self->Close();
            }
            break;
        }
        default:
            // Pass anything else to SDLs original window procedure.
            return CallWindowProc((WNDPROC)SDL_WndProc, h_wnd, message, w_param, l_param);
    }
    return 0;
};
#endif

void GfxWindowBackendSDL2::Init(const char* gameName, const char* gfxApiName, bool startFullScreen, uint32_t width,
                                uint32_t height, int32_t posX, int32_t posY) {
    mWindowWidth = width;
    mWindowHeight = height;

#if SDL_VERSION_ATLEAST(2, 24, 0)
    /* fix DPI scaling issues on Windows */
    SDL_SetHint(SDL_HINT_WINDOWS_DPI_AWARENESS, "permonitorv2");
#endif

    SDL_Init(SDL_INIT_VIDEO);

    SDL_EventState(SDL_DROPFILE, SDL_ENABLE);

#if defined(__APPLE__)
    bool use_opengl = strcmp(gfxApiName, "OpenGL") == 0;
#else
    constexpr bool use_opengl = true;
#endif

    if (use_opengl) {
        SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
        SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
        SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    } else {
        SDL_SetHint(SDL_HINT_RENDER_DRIVER, "metal");
    }

#if defined(__APPLE__)
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG); // Always required on Mac
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
#endif

#ifdef _WIN32
    // Use high-resolution mTimer by default on Windows 10 (so that NtSetTimerResolution (...) hacks are not needed)
    mTimer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    // Fallback to low resolution mTimer if unsupported by the OS
    if (mTimer == nullptr) {
        mTimer = CreateWaitableTimer(nullptr, false, nullptr);
    }
#endif

#ifdef __OpenBSD__
    int sysctlname[2] = { CTL_KERN, KERN_CLOCKRATE };
    struct clockinfo clockinfo;
    size_t clockinfo_size = sizeof(struct clockinfo);
    if (sysctl(sysctlname, 2, &clockinfo, &clockinfo_size, NULL, 0) != -1) {
        mBsdTick = clockinfo.tick;
    }
#endif

    char title[512];
    int len = snprintf(title, sizeof(title), "%s (%s)", gameName, gfxApiName);

#ifdef __IOS__
    Uint32 flags = SDL_WINDOW_BORDERLESS | SDL_WINDOW_SHOWN;
#else
    Uint32 flags = SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
#endif

    if (use_opengl) {
        flags = flags | SDL_WINDOW_OPENGL;
    } else {
        flags = flags | SDL_WINDOW_METAL;
    }

    mWnd = SDL_CreateWindow(title, posX, posY, mWindowWidth, mWindowHeight, flags);
#ifdef _WIN32
    // Get Windows window handle and use it to subclass the window procedure.
    // Needed to circumvent SDLs DPI scaling problems under windows (original does only scale *sometimes*).
    SDL_SysWMinfo wmInfo;
    SDL_VERSION(&wmInfo.version);
    SDL_GetWindowWMInfo(mWnd, &wmInfo);
    HWND hwnd = wmInfo.info.win.window;
    SDL_WndProc = SetWindowLongPtr(hwnd, GWLP_WNDPROC, (LONG_PTR)gfx_sdl_wnd_proc);
    SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
#endif
    Fast::GuiWindowInitData window_impl;

    int display_in_use = SDL_GetWindowDisplayIndex(mWnd);
    if (display_in_use < 0) { // Fallback to default if out of bounds
        posX = 100;
        posY = 100;
    }

    if (use_opengl) {
        SDL_GL_GetDrawableSize(mWnd, &mWindowWidth, &mWindowHeight);

        if (startFullScreen) {
            SetFullscreenImpl(true, false);
        }

        mCtx = SDL_GL_CreateContext(mWnd);

        SDL_GL_MakeCurrent(mWnd, mCtx);
        SDL_GL_SetSwapInterval(mVsyncEnabled ? 1 : 0);

        window_impl.Opengl = { mWnd, mCtx };
    } else {
        uint32_t flags = SDL_RENDERER_ACCELERATED;
        if (mVsyncEnabled) {
            flags |= SDL_RENDERER_PRESENTVSYNC;
        }
        mRenderer = SDL_CreateRenderer(mWnd, -1, flags);
        if (mRenderer == nullptr) {
            SPDLOG_ERROR("Error creating renderer: {}", SDL_GetError());
            return;
        }

        if (startFullScreen) {
            SetFullscreenImpl(true, false);
        }

        SDL_GetRendererOutputSize(mRenderer, &mWindowWidth, &mWindowHeight);
        window_impl.Metal = { mWnd, mRenderer };
    }

    std::dynamic_pointer_cast<Fast::Fast3dGui>(Ship::Context::GetInstance()->GetWindow()->GetGui())->Init(window_impl);

    for (size_t i = 0; i < std::size(lus_to_sdl_table); i++) {
        mSdlToLusTable[lus_to_sdl_table[i]] = i;
    }

    for (size_t i = 0; i < std::size(scancode_rmapping_extended); i++) {
        mSdlToLusTable[scancode_rmapping_extended[i][0]] = mSdlToLusTable[scancode_rmapping_extended[i][1]] + 0x100;
    }

    for (size_t i = 0; i < std::size(scancode_rmapping_nonextended); i++) {
        mSdlToLusTable[scancode_rmapping_nonextended[i][0]] = mSdlToLusTable[scancode_rmapping_nonextended[i][1]];
        mSdlToLusTable[scancode_rmapping_nonextended[i][1]] += 0x100;
    }
}

void GfxWindowBackendSDL2::SetFullscreenChangedCallback(void (*onFullscreenChanged)(bool is_now_fullscreen)) {
    mOnFullscreenChanged = onFullscreenChanged;
}

void GfxWindowBackendSDL2::SetFullscreen(bool enable) {
    SetFullscreenImpl(enable, true);
}

void GfxWindowBackendSDL2::SetCursorVisibility(bool visible) {
    if (visible) {
        SDL_ShowCursor(SDL_ENABLE);
    } else {
        SDL_ShowCursor(SDL_DISABLE);
    }
}

void GfxWindowBackendSDL2::SetMousePos(int32_t x, int32_t y) {
    SDL_WarpMouseInWindow(mWnd, x, y);
}

void GfxWindowBackendSDL2::GetMousePos(int32_t* x, int32_t* y) {
    SDL_GetMouseState(x, y);
}

void GfxWindowBackendSDL2::GetMouseDelta(int32_t* x, int32_t* y) {
    SDL_GetRelativeMouseState(x, y);
}

void GfxWindowBackendSDL2::GetMouseWheel(float* x, float* y) {
    *x = mMouseWheelX;
    *y = mMouseWheelY;
    mMouseWheelX = 0.0f;
    mMouseWheelY = 0.0f;
}

bool GfxWindowBackendSDL2::GetMouseState(uint32_t btn) {
    return SDL_GetMouseState(nullptr, nullptr) & (1 << btn);
}

void GfxWindowBackendSDL2::SetMouseCapture(bool capture) {
    SDL_SetRelativeMouseMode(static_cast<SDL_bool>(capture));
    // TODO: Manually setting a clipping rect here because
    // https://wiki.libsdl.org/SDL2/SDL_HINT_MOUSE_RELATIVE_MODE_CENTER isn't working as epxected.
    // Revisit on SDL3
    auto mouse = SDL_GetWindowMouseRect(mWnd);
    if (capture) {
        int w, h;
        SDL_GetWindowSize(mWnd, &w, &h);
        mCursorClip = { (w / 2) - 1, (h / 2) - 1, 2, 2 };
    }
    SDL_SetWindowMouseRect(mWnd, capture ? &mCursorClip : NULL);
    if (!capture) {
        // Restore the window grab if one was requested; relative mode release would free it.
        SDL_SetWindowMouseGrab(mWnd, mIsMouseGrabbed ? SDL_TRUE : SDL_FALSE);
    }
}

bool GfxWindowBackendSDL2::IsMouseCaptured() {
    return (SDL_GetRelativeMouseMode() == SDL_TRUE);
}

void GfxWindowBackendSDL2::SetMouseGrab(bool grab) {
    if (mIsMouseGrabbed == grab) {
        return;
    }
    mIsMouseGrabbed = grab;
    if (SDL_GetRelativeMouseMode() == SDL_TRUE) {
        // Relative-mode capture owns the cursor; it will restore the grab on release.
        return;
    }
    SDL_SetWindowMouseGrab(mWnd, grab ? SDL_TRUE : SDL_FALSE);
}

void GfxWindowBackendSDL2::SetKeyboardCallbacks(bool (*onKeyDown)(int scancode), bool (*onKeyUp)(int scancode),
                                                void (*onAllKeysUp)()) {
    mOnKeyDown = onKeyDown;
    mOnKeyUp = onKeyUp;
    mOnAllKeysUp = onAllKeysUp;
}

void GfxWindowBackendSDL2::SetMouseCallbacks(bool (*onMouseButtonDown)(int btn), bool (*onMouseButtonUp)(int btn)) {
    mOnMouseButtonDown = onMouseButtonDown;
    mOnMouseButtonUp = onMouseButtonUp;
}

void GfxWindowBackendSDL2::GetDimensions(uint32_t* width, uint32_t* height, int32_t* posX, int32_t* posY) {
#ifdef __APPLE__
    SDL_GetWindowSize(mWnd, static_cast<int*>((void*)width), static_cast<int*>((void*)height));
#else
    SDL_GL_GetDrawableSize(mWnd, static_cast<int*>((void*)width), static_cast<int*>((void*)height));
#endif
    SDL_GetWindowPosition(mWnd, static_cast<int*>(posX), static_cast<int*>(posY));
}

void GfxWindowBackendSDL2::SetDimensions(uint32_t width, uint32_t height, int32_t posX, int32_t posY) {
    mWindowWidth = width;
    mWindowHeight = height;
    if (mWnd) {
        SDL_SetWindowPosition(mWnd, posX, posY);
        SDL_SetWindowSize(mWnd, mWindowWidth, mWindowHeight);
    }
}

Ship::WindowRect GfxWindowBackendSDL2::GetPrimaryMonitorRect() {
    SDL_DisplayMode mode;
    int display_in_use = mWnd ? SDL_GetWindowDisplayIndex(mWnd) : 0;
    if (display_in_use < 0) {
        SPDLOG_WARN("Can't detect on which monitor we are. Probably out of display area? ({})", SDL_GetError());
        display_in_use = 0;
    }
    if (SDL_GetDesktopDisplayMode(display_in_use, &mode) >= 0) {
        return { 0, 0, mode.w, mode.h };
    }
    SPDLOG_ERROR("Failed to get SDL Desktop Display Mode: ({})", SDL_GetError());
    return { 0, 0, 0, 0 };
}

int GfxWindowBackendSDL2::TranslateScancode(int scancode) const {
    if (scancode < 512) {
        return mSdlToLusTable[scancode];
    }
    return 0;
}

int GfxWindowBackendSDL2::UntranslateScancode(int translatedScancode) const {
    for (int i = 0; i < 512; i++) {
        if (mSdlToLusTable[i] == translatedScancode) {
            return i;
        }
    }

    return 0;
}

void GfxWindowBackendSDL2::OnKeydown(int scancode) const {
    int key = TranslateScancode(scancode);
    if (mOnKeyDown != nullptr) {
        mOnKeyDown(key);
    }
}

void GfxWindowBackendSDL2::OnKeyup(int scancode) const {
    int key = TranslateScancode(scancode);
    if (mOnKeyUp != nullptr) {
        mOnKeyUp(key);
    }
}

void GfxWindowBackendSDL2::OnMouseButtonDown(int btn) const {
    if (!(btn >= 0 && btn < 5)) {
        return;
    }
    if (mOnMouseButtonDown != nullptr) {
        mOnMouseButtonDown(btn);
    }
}

void GfxWindowBackendSDL2::OnMouseButtonUp(int btn) const {
    if (mOnMouseButtonUp != nullptr) {
        mOnMouseButtonUp(btn);
    }
}

void GfxWindowBackendSDL2::HandleSingleEvent(SDL_Event& event) {
    Fast::WindowEvent event_impl;
    event_impl.Sdl = { &event };
    auto gui = Ship::Context::GetInstance()->GetWindow()->GetGui();
    auto fast3dGui = std::dynamic_pointer_cast<Fast::Fast3dGui>(gui);
    if (fast3dGui) {
        fast3dGui->HandleWindowEvents(event_impl);
    } else {
        static bool sWarnedOnce = false;
        if (!sWarnedOnce) {
            SPDLOG_ERROR("gfx_sdl2: Gui is not a Fast3dGui; cannot dispatch window event");
            sWarnedOnce = true;
        }
    }
    switch (event.type) {
#ifndef TARGET_WEB
        // Scancodes are broken in Emscripten SDL2: https://bugzilla.libsdl.org/show_bug.cgi?id=3259
        case SDL_KEYDOWN:
            OnKeydown(event.key.keysym.scancode);
            break;
        case SDL_KEYUP:
            OnKeyup(event.key.keysym.scancode);
            break;
        case SDL_MOUSEMOTION:
            // Nothing to do here, but tracing it answers whether SDL is still synthesising
            // pointer motion from touch behind the finger feed: which == SDL_TOUCH_MOUSEID.
            GdxTracePointer("MOUSEMOTION", static_cast<float>(event.motion.x),
                            static_cast<float>(event.motion.y), static_cast<int>(event.motion.which));
            break;
        case SDL_MOUSEBUTTONDOWN:
            GdxTracePointer("MOUSEBUTTONDOWN", static_cast<float>(event.button.x),
                            static_cast<float>(event.button.y), static_cast<int>(event.button.which));
            if (event.button.which != SDL_TOUCH_MOUSEID && mTouchGesture == TouchGesture::Pending) {
                // The platform is emulating a pointer from the touch we were still holding a press
                // for. It owns this gesture: it will click and release coherently on its own, and
                // anything from us would only land the press somewhere its release is not.
                mTouchGesture = TouchGesture::OwnedByPlatform;
            }
            OnMouseButtonDown(event.button.button - 1);
            break;
        case SDL_MOUSEBUTTONUP:
            GdxTracePointer("MOUSEBUTTONUP", static_cast<float>(event.button.x),
                            static_cast<float>(event.button.y), static_cast<int>(event.button.which));
            if (event.button.which != SDL_TOUCH_MOUSEID && mTouchGesture != TouchGesture::None) {
                if (mTouchGesture == TouchGesture::OwnedByFinger && ImGui::GetCurrentContext() != nullptr) {
                    // Both channels reported one tap. Lift our press, or it stays held and swallows
                    // every later click, since ImGui already sees the button down.
                    ImGui::GetIO().AddMouseButtonEvent(0, false);
                }
                // Ends the gesture whoever owned it: platforms that emulate a pointer often stop
                // sending FINGERUP, and without this the next touch is discarded as a second finger.
                mTouchGesture = TouchGesture::None;
            }
            OnMouseButtonUp(event.button.button - 1);
            break;
        case SDL_MOUSEWHEEL:
            mMouseWheelX = event.wheel.x;
            mMouseWheelY = event.wheel.y;
            break;
        // ImGui's SDL2 backend does not translate SDL_FINGER events, so the primary finger is fed
        // in as a left-mouse pointer -- but the press is held back one event first.
        //
        // Some platforms emulate a pointer from the same touch. There the sequence is FINGERDOWN,
        // then pointer motion to a slightly different place, then a full click there, and pressing
        // on FINGERDOWN puts our press at the raw finger position while their release arrives tens
        // of pixels away, so the widget never activates and taps appear to need several tries.
        // Which behaviour a machine shows is not fixed: the same binary on the same device has
        // produced 58 FINGERUPs and 2 pointer clicks in one session, and 1 and 42 in the next.
        // So the decision is per gesture rather than per run, and one event of delay is enough to
        // make it: a pointer click means the platform owns this touch, anything else means we do.
        // Holding the press rather than suppressing it keeps finger drags working, since the press
        // is replayed at the point the finger landed. GDX_DIAG_TOUCH=1 traces the whole sequence.
        case SDL_FINGERDOWN:
        case SDL_FINGERUP:
        case SDL_FINGERMOTION: {
            // tfinger coordinates are normalized. Scale them by the window rather than by
            // mWindowWidth/mWindowHeight, which hold the drawable size: ImGui's SDL2 backend
            // takes DisplaySize from SDL_GetWindowSize and carries the drawable ratio separately
            // in DisplayFramebufferScale, so the two only agree where nothing is scaled.
            int windowW = 0, windowH = 0;
            SDL_GetWindowSize(mWnd, &windowW, &windowH);
            const float fingerX = event.tfinger.x * static_cast<float>(windowW);
            const float fingerY = event.tfinger.y * static_cast<float>(windowH);
            const char* traced = (event.type == SDL_FINGERDOWN)   ? "FINGERDOWN"
                                 : (event.type == SDL_FINGERUP)   ? "FINGERUP"
                                                                  : "FINGERMOTION";
            const char* skipped = "";
            if (event.type == SDL_FINGERDOWN) {
                if (mTouchGesture != TouchGesture::None) {
                    skipped = "| dropped: a gesture is already in progress";
                }
            } else if (mTouchGesture == TouchGesture::None ||
                       event.tfinger.fingerId != mPrimaryFingerId) {
                skipped = "| dropped: not the pointer finger";
            } else if (mTouchGesture == TouchGesture::OwnedByPlatform) {
                skipped = "| not fed: the platform is driving this gesture";
            }
            // Traced before it is acted on, so a dropped event still appears: silently discarding
            // them is what left the first reading of this trace ambiguous.
            GdxTracePointer(traced, fingerX, fingerY, static_cast<int>(event.tfinger.fingerId), skipped);
            if (skipped[0] != '\0') {
                if (event.type == SDL_FINGERUP && event.tfinger.fingerId == mPrimaryFingerId) {
                    mTouchGesture = TouchGesture::None; // a platform-driven gesture ends here too
                }
                break;
            }

            ImGuiIO& io = ImGui::GetIO();
            if (event.type == SDL_FINGERDOWN) {
                mPrimaryFingerId = event.tfinger.fingerId;
                mPendingFingerX = fingerX;
                mPendingFingerY = fingerY;
                mTouchGesture = TouchGesture::Pending;
                // Position only. The press waits for the next event, which is what says whether
                // the platform is going to deliver its own click for this same touch.
                io.AddMousePosEvent(fingerX, fingerY);
                break;
            }
            if (mTouchGesture == TouchGesture::Pending) {
                // Nothing claimed the gesture, so it is ours. Press where the finger landed rather
                // than where it is now, so a drag starts from the point that was touched.
                io.AddMousePosEvent(mPendingFingerX, mPendingFingerY);
                io.AddMouseButtonEvent(0, true);
                mTouchGesture = TouchGesture::OwnedByFinger;
            }
            io.AddMousePosEvent(fingerX, fingerY);
            if (event.type == SDL_FINGERUP) {
                io.AddMouseButtonEvent(0, false);
                mTouchGesture = TouchGesture::None;
            }
            break;
        }
#endif
        case SDL_WINDOWEVENT:
            switch (event.window.event) {
                case SDL_WINDOWEVENT_SIZE_CHANGED:
#ifdef __APPLE__
                    SDL_GetWindowSize(mWnd, &mWindowWidth, &mWindowHeight);
#else
                    SDL_GL_GetDrawableSize(mWnd, &mWindowWidth, &mWindowHeight);
#endif
                    break;
                case SDL_WINDOWEVENT_FOCUS_GAINED:
                    if (mIsMouseGrabbed && (SDL_GetRelativeMouseMode() != SDL_TRUE)) {
                        SDL_SetWindowMouseGrab(mWnd, SDL_TRUE);
                    }
                    break;
                case SDL_WINDOWEVENT_FOCUS_LOST:
                    if (mIsMouseGrabbed) {
                        SDL_SetWindowMouseGrab(mWnd, SDL_FALSE);
                    }
                    break;
                case SDL_WINDOWEVENT_CLOSE:
                    if (event.window.windowID == SDL_GetWindowID(mWnd)) {
                        // We listen specifically for main window close because closing main window
                        // on macOS does not trigger SDL_Quit.
                        Close();
                    }
                    break;
            }
            break;
        case SDL_DROPFILE:
            Ship::Context::GetInstance()->GetFileDropMgr()->SetDroppedFile(event.drop.file);
            break;
        case SDL_QUIT:
            Close();
            break;
    }
}

void GfxWindowBackendSDL2::HandleEvents() {
    SDL_Event event;
    SDL_PumpEvents();
    while (SDL_PeepEvents(&event, 1, SDL_GETEVENT, SDL_FIRSTEVENT, SDL_CONTROLLERDEVICEADDED - 1) > 0) {
        HandleSingleEvent(event);
    }
    while (SDL_PeepEvents(&event, 1, SDL_GETEVENT, SDL_CONTROLLERDEVICEREMOVED + 1, SDL_LASTEVENT) > 0) {
        HandleSingleEvent(event);
    }

    // resync fullscreen state
#ifdef __APPLE__
    auto nextFullscreenState = isNativeMacOSFullscreenActive(mWnd);
    if (mFullScreen != nextFullscreenState) {
        mFullScreen = nextFullscreenState;
        if (mOnFullscreenChanged != nullptr) {
            mOnFullscreenChanged(mFullScreen);
        }
    }
#endif
}

bool GfxWindowBackendSDL2::IsFrameReady() {
    return true;
}

static uint64_t qpc_to_100ns(uint64_t qpc) {
    const uint64_t qpc_freq = SDL_GetPerformanceFrequency();
    return qpc / qpc_freq * _100NANOSECONDS_IN_SECOND + qpc % qpc_freq * _100NANOSECONDS_IN_SECOND / qpc_freq;
}

void GfxWindowBackendSDL2::SyncFramerateWithTime() const {
    uint64_t t = qpc_to_100ns(SDL_GetPerformanceCounter());

    if (!sVsyncPaced) {
        const int64_t next = previous_time + 10 * FRAME_INTERVAL_US_NUMERATOR / FRAME_INTERVAL_US_DENOMINATOR;
        int64_t left = next - t;
#ifdef _WIN32
        // We want to exit a bit early, so we can busy-wait the rest to never miss the deadline
        left -= 15000UL;
#elif defined(__APPLE__)
        // Use macOS scheduler interval on macOS. Don't trust sysctl on macOS
        left -= 10000UL;
#elif defined(__OpenBSD__)
        left -= mBsdTick * 10;
#endif
        if (left > 0) {
#ifndef _WIN32
            const timespec spec = { 0, left * 100 };
            nanosleep(&spec, nullptr);
#else
            // The accuracy of this mTimer seems to usually be within +- 1.0 ms
            LARGE_INTEGER li;
            li.QuadPart = -left;
            SetWaitableTimer(mTimer, &li, 0, nullptr, nullptr, false);
            WaitForSingleObject(mTimer, INFINITE);
#endif
        }

        t = qpc_to_100ns(SDL_GetPerformanceCounter());
#ifdef _WIN32
        while (t < next) {
            YieldProcessor(); // TODO: Find a way for other compilers, OSes and architectures
            t = qpc_to_100ns(SDL_GetPerformanceCounter());
        }
#endif
        if (left > 0 && t - next < 10000) {
            // In case it takes some time for the application to wake up after sleep,
            // or inaccurate mTimer,
            // don't let that slow down the framerate.
            t = next;
        }
    }
    // On the skip path t is still the entry sample. Keeping previous_time near-now rather than
    // stale means the software wait resumes from a sane deadline, instead of a catch-up burst,
    // if the target fps later drifts off the refresh rate.
    previous_time = t;
}

void GfxWindowBackendSDL2::SwapBuffersBegin() {
    bool nextVsyncEnabled = Ship::Context::GetInstance()->GetConsoleVariables()->GetInteger(CVAR_VSYNC_ENABLED, 1);

    if (mVsyncEnabled != nextVsyncEnabled) {
        mVsyncEnabled = nextVsyncEnabled;
        SDL_GL_SetSwapInterval(mVsyncEnabled ? 1 : 0);
        SDL_RenderSetVSync(mRenderer, mVsyncEnabled ? 1 : 0);
    }

    // Two independently-clocked pacers beat against each other (judder, shimmer on
    // high-frequency detail), so when SDL_GL_SwapWindow below is already blocking on the
    // hardware refresh the software wait stands down. The tolerance absorbs NTSC/VRR drift.
    {
        int displayIndex = SDL_GetWindowDisplayIndex(mWnd);
        const bool displayChanged = displayIndex != sCachedDisplayIndex;
        const bool refreshStale = (previous_time - sLastRefreshRequeryTime100ns) >= kRefreshRequeryInterval100ns;
        if (displayChanged || refreshStale) {
            sCachedDisplayIndex = displayIndex;
            sLastRefreshRequeryTime100ns = previous_time;
            SDL_DisplayMode mode;
            if (SDL_GetCurrentDisplayMode(displayIndex, &mode) == 0 && mode.refresh_rate != 0) {
                sCachedRefreshHz = (double)mode.refresh_rate;
            } else {
                sCachedRefreshHz = 60.0;
            }
        }

        double tolerance = std::max(1.0, sCachedRefreshHz * 0.015);
        bool vsyncPaced = mVsyncEnabled != 0 && sCachedRefreshHz > 0.0 &&
                          fabs((double)mTargetFps - sCachedRefreshHz) <= tolerance;
        static bool sVsyncPacedLogged = false;
        if (vsyncPaced != sVsyncPaced || !sVsyncPacedLogged) {
            sVsyncPaced = vsyncPaced;
            sVsyncPacedLogged = true;
            SPDLOG_INFO("[pacer] vsync-paced: {} software wait target={:.2f} refresh={:.2f}",
                        vsyncPaced ? "skipping" : "resuming", (double)mTargetFps, sCachedRefreshHz);
        }
    }

    SyncFramerateWithTime();
    SDL_GL_SwapWindow(mWnd);
}

void GfxWindowBackendSDL2::SwapBuffersEnd() {
}

double GfxWindowBackendSDL2::GetTime() {
    return 0.0;
}

int GfxWindowBackendSDL2::GetTargetFps() {
    return mTargetFps;
}

void GfxWindowBackendSDL2::SetTargetFps(int fps) {
    mTargetFps = fps;
}

void GfxWindowBackendSDL2::SetMaxFrameLatency(int latency) {
    // Not supported by SDL :(
}

const char* GfxWindowBackendSDL2::GetKeyName(int scancode) {
    return SDL_GetScancodeName((SDL_Scancode)UntranslateScancode(scancode));
}

bool GfxWindowBackendSDL2::CanDisableVsync() {
    return true;
}

bool GfxWindowBackendSDL2::IsRunning() {
    return mIsRunning;
}

void GfxWindowBackendSDL2::Destroy() {
    // TODO: destroy _any_ resources used by SDL
    SDL_GL_DeleteContext(mCtx);
    SDL_DestroyWindow(mWnd);
    SDL_DestroyRenderer(mRenderer);
    SDL_Quit();
}

bool GfxWindowBackendSDL2::IsFullscreen() {
    return mFullScreen;
}
} // namespace Fast
#endif
