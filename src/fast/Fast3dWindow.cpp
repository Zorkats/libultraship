#include "fast/Fast3dWindow.h"

#include "ship/Context.h"
#include "ship/config/Config.h"
#include "ship/controller/controldeck/ControlDeck.h"
#include "ship/config/ConsoleVariable.h"
#include "fast/interpreter.h"
#include "fast/backends/gfx_sdl.h"
#include "fast/backends/gfx_dxgi.h"
#include "fast/backends/gfx_opengl.h"
#include "fast/backends/gfx_metal.h"
#include "fast/backends/gfx_direct3d_common.h"
#include "fast/backends/gfx_direct3d11.h"
#include "fast/backends/gfx_window_manager_api.h"

#include "fast/Fast3dGui.h"

#include <fstream>

namespace Fast {

extern void GfxSetInstance(std::shared_ptr<Interpreter> gfx);

Fast3dWindow::Fast3dWindow(std::shared_ptr<Ship::Gui> gui, std::shared_ptr<FastMouseStateManager> mouseStateManager)
    : Ship::Window(gui, mouseStateManager) {
    mWindowManagerApi = nullptr;
    mRenderingApi = nullptr;
    mInterpreter = std::make_shared<Interpreter>();
    GfxSetInstance(mInterpreter);

#ifdef _WIN32
    AddAvailableWindowBackend(WindowBackend::FAST3D_DXGI_DX11);
#endif
#ifdef __APPLE__
    if (Metal_IsSupported()) {
        AddAvailableWindowBackend(WindowBackend::FAST3D_SDL_METAL);
    }
#endif
    AddAvailableWindowBackend(WindowBackend::FAST3D_SDL_OPENGL);
}

Fast3dWindow::Fast3dWindow(std::shared_ptr<Ship::Gui> gui)
    : Fast3dWindow(gui, std::make_shared<FastMouseStateManager>()) {
}

Fast3dWindow::Fast3dWindow(std::vector<std::shared_ptr<Ship::GuiWindow>> guiWindows)
    : Fast3dWindow(std::make_shared<Fast3dGui>(guiWindows)) {
}

Fast3dWindow::Fast3dWindow() : Fast3dWindow(std::vector<std::shared_ptr<Ship::GuiWindow>>()) {
}

Fast3dWindow::~Fast3dWindow() {
    SPDLOG_DEBUG("destruct fast3dwindow");
    mInterpreter->Destroy();
    delete mRenderingApi;
    delete mWindowManagerApi;
}

void Fast3dWindow::Init() {
    bool gameMode = false;

#ifdef __linux__
    std::ifstream osReleaseFile("/etc/os-release");
    if (osReleaseFile.is_open()) {
        std::string line;
        while (std::getline(osReleaseFile, line)) {
            if (line.find("VARIANT_ID") != std::string::npos) {
                if (line.find("steamdeck") != std::string::npos) {
                    gameMode = std::getenv("XDG_CURRENT_DESKTOP") != nullptr &&
                               std::string(std::getenv("XDG_CURRENT_DESKTOP")) == "gamescope";
                }
                break;
            }
        }
    }
#elif defined(__ANDROID__) || defined(__IOS__)
    gameMode = true;
#endif

    bool isFullscreen;
    uint32_t width, height;
    int32_t posX, posY;

    isFullscreen = Ship::Context::GetInstance()->GetConfig()->GetBool("Window.Fullscreen.Enabled", false) || gameMode;
    posX = Ship::Context::GetInstance()->GetConfig()->GetInt("Window.PositionX", 100);
    posY = Ship::Context::GetInstance()->GetConfig()->GetInt("Window.PositionY", 100);

    if (isFullscreen) {
        width = Ship::Context::GetInstance()->GetConfig()->GetInt("Window.Fullscreen.Width", gameMode ? 1280 : 1920);
        height = Ship::Context::GetInstance()->GetConfig()->GetInt("Window.Fullscreen.Height", gameMode ? 800 : 1080);
    } else {
        width = Ship::Context::GetInstance()->GetConfig()->GetInt("Window.Width", 640);
        height = Ship::Context::GetInstance()->GetConfig()->GetInt("Window.Height", 480);
    }
    Ship::Context::GetInstance()->GetWindow()->SetFullscreenScancode(
        Ship::Context::GetInstance()->GetConfig()->GetInt("Shortcuts.Fullscreen", Ship::KbScancode::LUS_KB_F11));
    Ship::Context::GetInstance()->GetWindow()->SetMouseCaptureScancode(
        Ship::Context::GetInstance()->GetConfig()->GetInt("Shortcuts.MouseCapture", Ship::KbScancode::LUS_KB_F2));

    InitWindowManager();
    mGfxDebugger = std::make_shared<GfxDebugger>();
    mInterpreter->SetGfxDebugger(mGfxDebugger);
    mInterpreter->Init(mWindowManagerApi, mRenderingApi, Ship::Context::GetInstance()->GetName().c_str(), isFullscreen,
                       width, height, posX, posY);
    mWindowManagerApi->SetFullscreenChangedCallback(OnFullscreenChanged);
    mWindowManagerApi->SetKeyboardCallbacks(KeyDown, KeyUp, AllKeysUp);
    mWindowManagerApi->SetMouseCallbacks(MouseButtonDown, MouseButtonUp);

    SetTextureFilter((FilteringMode)Ship::Context::GetInstance()->GetConsoleVariables()->GetInteger(
        CVAR_TEXTURE_FILTER, FILTER_THREE_POINT));
}

int32_t Fast3dWindow::GetTargetFps() {
    return mInterpreter->GetTargetFps();
}

void Fast3dWindow::SetTargetFps(int32_t fps) {
    mInterpreter->SetTargetFps(fps);
}

void Fast3dWindow::SetMaximumFrameLatency(int32_t latency) {
    mInterpreter->SetMaxFrameLatency(latency);
}

void Fast3dWindow::GetPixelDepthPrepare(float x, float y) {
    mInterpreter->GetPixelDepthPrepare(x, y);
}

uint16_t Fast3dWindow::GetPixelDepth(float x, float y) {
    return mInterpreter->GetPixelDepth(x, y);
}

void Fast3dWindow::InitWindowManager() {
    SetWindowBackend(GetSavedWindowBackend());

    switch (GetWindowBackend()) {
#ifdef ENABLE_DX11
        case WindowBackend::FAST3D_DXGI_DX11:
            mWindowManagerApi = new GfxWindowBackendDXGI();
            mRenderingApi = new GfxRenderingAPIDX11(static_cast<GfxWindowBackendDXGI*>(mWindowManagerApi));
            break;
#endif
#ifdef ENABLE_OPENGL
        case WindowBackend::FAST3D_SDL_OPENGL:
            mRenderingApi = new GfxRenderingAPIOGL();
            mWindowManagerApi = new GfxWindowBackendSDL2();
            break;
#endif
#ifdef __APPLE__
        case WindowBackend::FAST3D_SDL_METAL:
            mRenderingApi = new GfxRenderingAPIMetal();
            mWindowManagerApi = new GfxWindowBackendSDL2();
            break;
#endif
        default:
            SPDLOG_ERROR("Could not load the correct rendering backend");
            break;
    }
}

void Fast3dWindow::SetTextureFilter(FilteringMode filteringMode) {
    mInterpreter->GetCurrentRenderingAPI()->SetTextureFilter(filteringMode);
}

void Fast3dWindow::EnableSRGBMode() {
    mInterpreter->mRapi->SetSrgbMode();
}

void Fast3dWindow::SetRendererUCode(UcodeHandlers ucode) {
    gfx_set_target_ucode(ucode);
}

void Fast3dWindow::Close() {
    mWindowManagerApi->Close();
}

void Fast3dWindow::RunGuiOnly() {
    mInterpreter->RunGuiOnly();
}

void Fast3dWindow::StartFrame() {
    mInterpreter->StartFrame();
}

void Fast3dWindow::EndFrame() {
    mInterpreter->EndFrame();
}

bool Fast3dWindow::IsFrameReady() {
    return mWindowManagerApi->IsFrameReady();
}

/* Implemented in port/n64_gfx_bridge.cpp; a weak no-op is not available across this boundary, so
   the port always defines it. Called once per sub-frame render, before any blit or present. */
extern "C" void gdx_gfx_post_run_capture(void);
// port/gdx_course_edit_mouse.cpp — Course Edit tool keys / rebind capture / wheel accumulator.
extern "C" int gdx_course_edit_mouse_on_key(int lusScancode, int isDown);
extern "C" int gdx_course_edit_mouse_owns_button(int button);
extern "C" void gdx_course_edit_mouse_all_keys_up(void);

/* Sub-frame present bypass.
 *
 * The DXGI software limiter (gfx_dxgi.cpp IsFrameReady) paces a loop that presents once and then
 * waits. Interpolation presents M sub-frames back to back and asks permission for each, which the
 * limiter cannot express: inside a burst `last_end` and `desired` advance in lockstep, the
 * decision sits on the boundary and noise grants roughly every other present (measured 51.8%,
 * independent of VSync, pass spacing and the vsync interval estimate).
 *
 * The swapchain already paces correctly -- SwapBuffersEnd blocks until it can accept another
 * frame -- so only the limiter's verdict is overridden, and only while the interpolation loop is
 * driving. IsFrameReady is still called: it advances the internal schedule and the present
 * statistics that the stock single-present path depends on. */
static bool gGdxSubframePresent = false;

extern "C" void gdx_fast3d_set_subframe_present(int on) {
    gGdxSubframePresent = (on != 0);
}

// Declared rather than included: port/gdx_perf.h lives outside libultraship. The values must
// match GdxPerfSub, and gdx_perf.cpp static-asserts that they do.
extern "C" void gdx_perf_sub_begin(int id);
extern "C" void gdx_perf_sub_end(int id);
#define GDX_PERF_SUB_GUI_ID 3
#define GDX_PERF_SUB_SFRAME_ID 4
#define GDX_PERF_SUB_IRUN_ID 5
#define GDX_PERF_SUB_EFRAME_ID 6

bool Fast3dWindow::DrawAndRunGraphicsCommands(Gfx* commands, const std::unordered_map<Mtx*, MtxF>& mtxReplacements) {
    std::shared_ptr<Window> wnd = Ship::Context::GetInstance()->GetWindow();

    // Skip dropped frames
    const bool gdxFrameReady = wnd->IsFrameReady();
    if (!gdxFrameReady && !gGdxSubframePresent) {
        return false;
    }

    auto gui = wnd->GetGui();
    // Setup mouse state manager
    wnd->GetMouseStateManager()->StartFrame();
    // Setup of the backend frames and draw initial Window and GUI menus
    gdx_perf_sub_begin(GDX_PERF_SUB_GUI_ID);
    gui->StartDraw();
    gdx_perf_sub_end(GDX_PERF_SUB_GUI_ID);
    // Setup game framebuffers to match available window space
    gdx_perf_sub_begin(GDX_PERF_SUB_SFRAME_ID);
    mInterpreter->StartFrame();
    gdx_perf_sub_end(GDX_PERF_SUB_SFRAME_ID);
    // Execute the games gfx commands
    gdx_perf_sub_begin(GDX_PERF_SUB_IRUN_ID);
    mInterpreter->Run(commands, mtxReplacements);
    gdx_perf_sub_end(GDX_PERF_SUB_IRUN_ID);
    // The only sound point to capture a sub-frame's image. The swap chain is FLIP_DISCARD, so
    // once EndFrame below presents, the back buffer contents are undefined -- capturing after
    // DrawAndRunGraphicsCommands returns compares buffers D3D was free to discard, which once
    // "measured" a 44% difference between two passes fed identical matrices.
    gdx_gfx_post_run_capture();
    // Renders the game frame buffer to the final window and finishes the GUI
    gdx_perf_sub_begin(GDX_PERF_SUB_GUI_ID);
    gui->EndDraw();
    gdx_perf_sub_end(GDX_PERF_SUB_GUI_ID);
    // Finalize swap buffers
    gdx_perf_sub_begin(GDX_PERF_SUB_EFRAME_ID);
    mInterpreter->EndFrame();
    gdx_perf_sub_end(GDX_PERF_SUB_EFRAME_ID);

    return true;
}

void Fast3dWindow::HandleEvents() {
    mWindowManagerApi->HandleEvents();
}

void Fast3dWindow::SetCursorVisibility(bool visible) {
    mWindowManagerApi->SetCursorVisibility(visible);
}

uint32_t Fast3dWindow::GetWidth() {
    uint32_t width, height;
    int32_t posX, posY;
    mWindowManagerApi->GetDimensions(&width, &height, &posX, &posY);
    return width;
}

uint32_t Fast3dWindow::GetHeight() {
    uint32_t width, height;
    int32_t posX, posY;
    mWindowManagerApi->GetDimensions(&width, &height, &posX, &posY);
    return height;
}

float Fast3dWindow::GetAspectRatio() {
    return mInterpreter->mCurDimensions.aspect_ratio;
}

int32_t Fast3dWindow::GetPosX() {
    uint32_t width, height;
    int32_t posX, posY;
    mWindowManagerApi->GetDimensions(&width, &height, &posX, &posY);
    return posX;
}

int32_t Fast3dWindow::GetPosY() {
    uint32_t width, height;
    int32_t posX, posY;
    mWindowManagerApi->GetDimensions(&width, &height, &posX, &posY);
    return posY;
}

void Fast3dWindow::SetMousePos(Ship::Coords pos) {
    mWindowManagerApi->SetMousePos(pos.x, pos.y);
}

Ship::Coords Fast3dWindow::GetMousePos() {
    int32_t x, y;
    mWindowManagerApi->GetMousePos(&x, &y);
    return { x, y };
}

Ship::Coords Fast3dWindow::GetMouseDelta() {
    int32_t x, y;
    mWindowManagerApi->GetMouseDelta(&x, &y);
    return { x, y };
}

Ship::CoordsF Fast3dWindow::GetMouseWheel() {
    float x, y;
    mWindowManagerApi->GetMouseWheel(&x, &y);
    return { x, y };
}

bool Fast3dWindow::GetMouseState(Ship::MouseBtn btn) {
    return mWindowManagerApi->GetMouseState(static_cast<uint32_t>(btn));
}

void Fast3dWindow::SetMouseCapture(bool capture) {
    mWindowManagerApi->SetMouseCapture(capture);
}

bool Fast3dWindow::IsMouseCaptured() {
    return mWindowManagerApi->IsMouseCaptured();
}

void Fast3dWindow::SetMouseGrab(bool grab) {
    mWindowManagerApi->SetMouseGrab(grab);
}

uint32_t Fast3dWindow::GetCurrentRefreshRate() {
    uint32_t refreshRate;
    mWindowManagerApi->GetActiveWindowRefreshRate(&refreshRate);
    return refreshRate;
}

bool Fast3dWindow::SupportsWindowedFullscreen() {
#ifdef __APPLE__
    return false;
#endif

    if (GetWindowBackend() == WindowBackend::FAST3D_SDL_OPENGL) {
        return true;
    }

    return false;
}

bool Fast3dWindow::CanDisableVerticalSync() {
    return mWindowManagerApi->CanDisableVsync();
}

void Fast3dWindow::SetResolutionMultiplier(float multiplier) {
    mInterpreter->SetResolutionMultiplier(multiplier);
}

void Fast3dWindow::SetMsaaLevel(uint32_t value) {
    mInterpreter->SetMsaaLevel(value);
}

void Fast3dWindow::SetFullscreen(bool isFullscreen) {
    // Save current window position before fullscreening
    SaveWindowToConfig();
    mWindowManagerApi->SetFullscreen(isFullscreen);
}

bool Fast3dWindow::IsFullscreen() {
    return mWindowManagerApi->IsFullscreen();
}

bool Fast3dWindow::IsRunning() {
    return mWindowManagerApi->IsRunning();
}

uintptr_t Fast3dWindow::GetGfxFrameBuffer() {
    return mInterpreter->mGfxFrameBuffer;
}

const char* Fast3dWindow::GetKeyName(int32_t scancode) {
    return mWindowManagerApi->GetKeyName(scancode);
}

// The window backend's message pump also runs synchronously inside DestroyWindow during
// ~Fast3dWindow, when Context::GetInstance() is already empty, so every handler below must
// tolerate a dead singleton. A queued key/mouse message there was the Windows exit crash.
bool Fast3dWindow::KeyUp(int32_t scancode) {
    auto ctx = Ship::Context::GetInstance();
    if (ctx == nullptr) {
        return false;
    }
    if (scancode == ctx->GetWindow()->GetFullscreenScancode()) {
        ctx->GetWindow()->ToggleFullscreen();
    }

    if (scancode == ctx->GetWindow()->GetMouseCaptureScancode()) {
        ctx->GetWindow()->GetMouseStateManager()->ToggleMouseCaptureOverride();
    }

    // G-Diffuser Course Edit tool keys; backend-agnostic (LUS KbScancode space on both backends).
    int consumed = gdx_course_edit_mouse_on_key(scancode, 0);

    ctx->GetWindow()->SetLastScancode(-1);
    // A key pressed before the editor took ownership must still release its mapped action.
    bool processed = ctx->GetControlDeck()->ProcessKeyboardEvent(Ship::KbEventType::LUS_KB_EVENT_KEY_UP,
                                                                static_cast<Ship::KbScancode>(scancode));
    return consumed || processed;
}

bool Fast3dWindow::KeyDown(int32_t scancode) {
    auto ctx = Ship::Context::GetInstance();
    if (ctx == nullptr) {
        return false;
    }
    if (scancode == ctx->GetWindow()->GetFullscreenScancode() ||
        scancode == ctx->GetWindow()->GetMouseCaptureScancode()) {
        ctx->GetWindow()->SetLastScancode(scancode);
        return ctx->GetControlDeck()->ProcessKeyboardEvent(Ship::KbEventType::LUS_KB_EVENT_KEY_DOWN,
                                                           static_cast<Ship::KbScancode>(scancode));
    }

    // G-Diffuser Course Edit tool keys and menu rebind capture; see KeyUp above.
    int consumed = gdx_course_edit_mouse_on_key(scancode, 1);
    if (consumed) {
        ctx->GetWindow()->SetLastScancode(scancode);
        return true;
    }

    bool isProcessed = ctx->GetControlDeck()->ProcessKeyboardEvent(Ship::KbEventType::LUS_KB_EVENT_KEY_DOWN,
                                                                   static_cast<Ship::KbScancode>(scancode));
    ctx->GetWindow()->SetLastScancode(scancode);

    return isProcessed;
}

void Fast3dWindow::AllKeysUp() {
    auto ctx = Ship::Context::GetInstance();
    if (ctx == nullptr) {
        return;
    }
    gdx_course_edit_mouse_all_keys_up();
    ctx->GetControlDeck()->ProcessKeyboardEvent(Ship::KbEventType::LUS_KB_EVENT_ALL_KEYS_UP,
                                                Ship::KbScancode::LUS_KB_UNKNOWN);
}

bool Fast3dWindow::MouseButtonUp(int button) {
    auto ctx = Ship::Context::GetInstance();
    if (ctx == nullptr) {
        return false;
    }
    return ctx->GetControlDeck()->ProcessMouseButtonEvent(false, static_cast<Ship::MouseBtn>(button));
}

bool Fast3dWindow::MouseButtonDown(int button) {
    auto ctx = Ship::Context::GetInstance();
    if (ctx == nullptr) {
        return false;
    }
    if (gdx_course_edit_mouse_owns_button(button)) {
        return true;
    }
    bool isProcessed = ctx->GetControlDeck()->ProcessMouseButtonEvent(true, static_cast<Ship::MouseBtn>(button));
    return isProcessed;
}

void Fast3dWindow::OnFullscreenChanged(bool isNowFullscreen) {
    std::shared_ptr<Window> wnd = Ship::Context::GetInstance()->GetWindow();

    // Re-save fullscreen enabled after
    Ship::Context::GetInstance()->GetConfig()->SetBool("Window.Fullscreen.Enabled", isNowFullscreen);
}

std::weak_ptr<Interpreter> Fast3dWindow::GetInterpreterWeak() const {
    return mInterpreter;
}

std::string Fast3dWindow::GetWindowBackendName() {
    switch (GetWindowBackend()) {
        case WindowBackend::FAST3D_DXGI_DX11:
            return "DirectX 11";
        case WindowBackend::FAST3D_SDL_OPENGL:
            return "OpenGL";
        case WindowBackend::FAST3D_SDL_METAL:
            return "Metal";
        default:
            return "";
    }
}

void Fast3dWindow::SetCurrentDimensions(uint32_t width, uint32_t height) {
    SetCurrentDimensions(width, height, GetPosX(), GetPosY());
}

void Fast3dWindow::SetCurrentDimensions(uint32_t width, uint32_t height, int32_t posX, int32_t posY) {
    mWindowManagerApi->SetDimensions(width, height, posX, posY);
    SaveWindowToConfig();
}

void Fast3dWindow::SetCurrentDimensions(bool isFullscreen, uint32_t width, uint32_t height) {
    SetCurrentDimensions(isFullscreen, width, height, GetPosX(), GetPosY());
}

void Fast3dWindow::SetCurrentDimensions(bool isFullscreen, uint32_t width, uint32_t height, int32_t posX,
                                        int32_t posY) {
    auto config = Ship::Context::GetInstance()->GetConfig();
    if (!isFullscreen) {
        config->SetInt("Window.Width", static_cast<int32_t>(width));
        config->SetInt("Window.Height", static_cast<int32_t>(height));
        config->SetInt("Window.PositionX", posX);
        config->SetInt("Window.PositionY", posY);
    } else {
        config->SetInt("Window.Fullscreen.Width", static_cast<int32_t>(width));
        config->SetInt("Window.Fullscreen.Height", static_cast<int32_t>(height));
    }
    mWindowManagerApi->SetFullscreen(isFullscreen);
    mWindowManagerApi->SetDimensions(width, height, posX, posY);
    SaveWindowToConfig();
}

Ship::WindowRect Fast3dWindow::GetPrimaryMonitorRect() {
    return mWindowManagerApi->GetPrimaryMonitorRect();
}

std::shared_ptr<GfxDebugger> Fast3dWindow::GetGfxDebugger() const {
    return mGfxDebugger;
}

} // namespace Fast
