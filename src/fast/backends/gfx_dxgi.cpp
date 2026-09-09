#if defined(ENABLE_DX11) || defined(ENABLE_DX12)

#include <stdint.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <windows.h>
#include <windowsx.h> // GET_X_LPARAM(), GET_Y_LPARAM()
#include <wrl/client.h>
#include <dxgi1_3.h>
#include <dxgi1_4.h>
#include <dxgi1_5.h>
#include <versionhelpers.h>

#include <shellscalingapi.h>

#ifndef _LANGUAGE_C
#define _LANGUAGE_C
#endif

#include "ship/config/ConsoleVariable.h"
#include "ship/config/Config.h"
#include "ship/Context.h"
#include "ship/window/FileDropMgr.h"

#include "libultraship/bridge/controllerbridge.h"

#include "fast/backends/gfx_window_manager_api.h"
#include "fast/backends/gfx_rendering_api.h"
#include "fast/backends/gfx_direct3d_common.h"
#include "fast/backends/gfx_screen_config.h"
#include "fast/interpreter.h"
#include "fast/Fast3dGui.h"

#define DECLARE_GFX_DXGI_FUNCTIONS
#include "fast/backends/gfx_dxgi.h"

#define WINCLASS_NAME L"N64GAME"
#define GFX_BACKEND_NAME "DXGI"

#define FRAME_INTERVAL_NS_NUMERATOR 1000000000
#define FRAME_INTERVAL_NS_DENOMINATOR (mTargetFps)

#define NANOSECOND_IN_SECOND 1000000000
#define _100NANOSECONDS_IN_SECOND 10000000

#ifndef HID_USAGE_PAGE_GENERIC
#define HID_USAGE_PAGE_GENERIC ((unsigned short)0x01)
#endif
#ifndef HID_USAGE_GENERIC_MOUSE
#define HID_USAGE_GENERIC_MOUSE ((unsigned short)0x02)
#endif
using QWORD = uint64_t; // For NEXTRAWINPUTBLOCK

#define ALLOW_BACKGROUND_INPUTS_BLOCK_ID 95237930

namespace Fast {

void GfxWindowBackendDXGI::LoadDxgi() {
    dxgi_module = LoadLibraryW(L"dxgi.dll");
    *(FARPROC*)&CreateDXGIFactory1 = GetProcAddress(dxgi_module, "CreateDXGIFactory1");
    *(FARPROC*)&CreateDXGIFactory2 = GetProcAddress(dxgi_module, "CreateDXGIFactory2");
}

void GfxWindowBackendDXGI::ApplyMaxFrameLatency(bool first) {
    DXGI_SWAP_CHAIN_DESC swap_desc = {};
    swap_chain->GetDesc(&swap_desc);

    ComPtr<IDXGISwapChain2> swap_chain2;
    if ((swap_desc.Flags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT) &&
        swap_chain->QueryInterface(__uuidof(IDXGISwapChain2), &swap_chain2) == S_OK) {
        ThrowIfFailed(swap_chain2->SetMaximumFrameLatency(mMaxFrameLatency));
        if (first) {
            mWaitableObject = swap_chain2->GetFrameLatencyWaitableObject();
            WaitForSingleObject(mWaitableObject, INFINITE);
        }
    } else {
        ComPtr<IDXGIDevice1> device1;
        ThrowIfFailed(swap_chain->GetDevice(__uuidof(IDXGIDevice1), &device1));
        ThrowIfFailed(device1->SetMaximumFrameLatency(mMaxFrameLatency));
    }
    mAppliedMaxFrameLatency = mMaxFrameLatency;
}

static std::vector<std::tuple<HMONITOR, RECT, BOOL>> GetMonitorList() {
    std::vector<std::tuple<HMONITOR, RECT, BOOL>> monitors;
    EnumDisplayMonitors(
        nullptr, nullptr,
        [](HMONITOR hmon, HDC hdc, LPRECT rc, LPARAM lp) {
            UNREFERENCED_PARAMETER(hdc);
            UNREFERENCED_PARAMETER(rc);

            bool isPrimary;
            MONITORINFOEX mi = {};
            mi.cbSize = sizeof(MONITORINFOEX);
            GetMonitorInfo(hmon, &mi);
            auto monitors = (std::vector<std::tuple<HMONITOR, RECT, BOOL>>*)lp;
            if (mi.dwFlags == MONITORINFOF_PRIMARY) {
                isPrimary = TRUE;
            } else {
                isPrimary = FALSE;
            }
            monitors->push_back({ hmon, mi.rcMonitor, isPrimary });
            return TRUE;
        },
        (LPARAM)&monitors);
    return monitors;
}

// Uses coordinates to get a Monitor handle from a list
static bool GetMonitorAtCoords(std::vector<std::tuple<HMONITOR, RECT, BOOL>> MonitorList, int x, int y, UINT cx,
                               UINT cy, std::tuple<HMONITOR, RECT, BOOL>& MonitorInfo) {
    RECT wr = { x, y, (x + cx), (y + cy) };
    std::tuple<HMONITOR, RECT, BOOL> primary;
    for (std::tuple<HMONITOR, RECT, BOOL> i : MonitorList) {
        if (PtInRect(&get<1>(i), POINT((x + (cx / 2)), (y + (cy / 2))))) {
            MonitorInfo = i;
            return true;
        }
        if (get<2>(i)) {
            primary = i;
        }
    }
    RECT intersection;
    LONG area;
    LONG lastArea = 0;
    std::tuple<HMONITOR, RECT, BOOL> biggest;
    for (std::tuple<HMONITOR, RECT, BOOL> i : MonitorList) {
        if (IntersectRect(&intersection, &std::get<1>(i), &wr)) {
            area = (intersection.right - intersection.left) * (intersection.bottom - intersection.top);
            if (area > lastArea) {
                lastArea = area;
                biggest = i;
            }
        }
    }
    if (lastArea > 0) {
        MonitorInfo = biggest;
        return true;
    }
    MonitorInfo = primary; // Fallback to primary, when out of bounds.
    return false;
}

void GfxWindowBackendDXGI::ToggleBorderlessWindowFullScreen(bool enable, bool call_callback) {
    // Windows 7 + flip mode + waitable object can't go to exclusive fullscreen,
    // so do borderless instead. If DWM is enabled, this means we get one monitor
    // sync interval of latency extra. On Win 10 however (maybe Win 8 too), due to
    // "fullscreen optimizations" the latency is eliminated.

    if (enable == mFullScreen) {
        return;
    }

    if (!enable) {
        // Set in window mode with the last saved position and size
        SetWindowLongPtr(h_wnd, GWL_STYLE, WS_VISIBLE | WS_OVERLAPPEDWINDOW);

        if (mLastMaximizedState) {
            SetWindowPos(h_wnd, NULL, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE);
            ShowWindow(h_wnd, SW_MAXIMIZE);
        } else {
            std::tuple<HMONITOR, RECT, BOOL> Monitor;
            auto conf = Ship::Context::GetInstance()->GetConfig();
            current_width = conf->GetInt("Window.Width", 640);
            current_height = conf->GetInt("Window.Height", 480);
            mPosX = conf->GetInt("Window.PositionX", 100);
            mPosY = conf->GetInt("Window.PositionY", 100);
            if (!GetMonitorAtCoords(monitor_list, mPosX, mPosY, current_width, current_height,
                                    Monitor)) { // Fallback to default when out of bounds.
                mPosX = 100;
                mPosY = 100;
            }
            RECT wr = { mPosX, mPosY, mPosX + static_cast<int32_t>(current_width),
                        mPosY + static_cast<int32_t>(current_height) };
            AdjustWindowRect(&wr, WS_OVERLAPPEDWINDOW, FALSE);
            SetWindowPos(h_wnd, NULL, wr.left, wr.top, wr.right - wr.left, wr.bottom - wr.top, SWP_FRAMECHANGED);
            ShowWindow(h_wnd, SW_RESTORE);
        }

        mFullScreen = false;
    } else {
        // Save if window is maximized or not
        WINDOWPLACEMENT window_placement;
        window_placement.length = sizeof(WINDOWPLACEMENT);
        GetWindowPlacement(h_wnd, &window_placement);
        mLastMaximizedState = window_placement.showCmd == SW_SHOWMAXIMIZED;

        // We already know on what monitor we are (gets it on init or move)
        // Get info from that monitor
        RECT r = get<1>(mMonitor);

        // Set borderless full screen to that monitor
        SetWindowLongPtr(h_wnd, GWL_STYLE, WS_VISIBLE | WS_POPUP);
        // OTRTODO: This should be setting the resolution from config.
        current_width = r.right - r.left;
        current_height = r.bottom - r.top;
        SetWindowPos(h_wnd, HWND_TOP, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_FRAMECHANGED);
        mFullScreen = true;
    }

    if (mOnFullscreenChanged != nullptr && call_callback) {
        mOnFullscreenChanged(enable);
    }
}

void GfxWindowBackendDXGI::OnKeydown(WPARAM wParam, LPARAM lParam) {
    int key = ((lParam >> 16) & 0x1ff);
    if (mOnKeyDown != nullptr) {
        mOnKeyDown(key);
    }
}
void GfxWindowBackendDXGI::OnKeyup(WPARAM wParam, LPARAM lParam) {
    int key = ((lParam >> 16) & 0x1ff);
    if (mOnKeyUp != nullptr) {
        mOnKeyUp(key);
    }
}

void GfxWindowBackendDXGI::OnMouseButtonDown(int btn) {
    if (!(btn >= 0 && btn < 5)) {
        return;
    }
    mMousePressed[btn] = true;
    if (mOnMouseButtonDown != nullptr) {
        mOnMouseButtonDown(btn);
    }
}
void GfxWindowBackendDXGI::OnMouseButtonUp(int btn) {
    mMousePressed[btn] = false;
    if (mOnMouseButtonUp != nullptr) {
        mOnMouseButtonUp(btn);
    }
}

static double HzToPeriod(double Frequency) {
    if (Frequency == 0)
        Frequency = 60; // Default to 60, to prevent devision by zero
    double period = (double)1000 / Frequency;
    if (period == 0)
        period = 16.666666; // In case we go too low, use 16 ms (60 Hz) to prevent division by zero later
    return period;
}

static void GetMonitorHzPeriod(HMONITOR hMonitor, double& Frequency, double& Period) {
    DEVMODE dm = {};
    dm.dmSize = sizeof(DEVMODE);
    if (hMonitor != NULL) {
        MONITORINFOEX minfoex = {};
        minfoex.cbSize = sizeof(MONITORINFOEX);

        if (GetMonitorInfo(hMonitor, (LPMONITORINFOEX)&minfoex)) {
            if (EnumDisplaySettings(minfoex.szDevice, ENUM_CURRENT_SETTINGS, &dm)) {
                Frequency = dm.dmDisplayFrequency;
                Period = HzToPeriod(Frequency);
            }
        }
    }
}

static void GetMonitorHzPeriod(std::tuple<HMONITOR, RECT, BOOL> Monitor, double& Frequency, double& Period) {
    HMONITOR hMonitor = get<0>(Monitor);
    DEVMODE dm = {};
    dm.dmSize = sizeof(DEVMODE);
    if (hMonitor != NULL) {
        MONITORINFOEX minfoex = {};
        minfoex.cbSize = sizeof(MONITORINFOEX);

        if (GetMonitorInfo(hMonitor, (LPMONITORINFOEX)&minfoex)) {
            if (EnumDisplaySettings(minfoex.szDevice, ENUM_CURRENT_SETTINGS, &dm)) {
                Frequency = dm.dmDisplayFrequency;
                Period = HzToPeriod(Frequency);
            }
        }
    }
}

void GfxWindowBackendDXGI::Close() {
    mIsRunning = false;
}

void GfxWindowBackendDXGI::ApplyMouseCaptureClip() {
    RECT rect;
    rect.left = mPosX + (current_width / 2) - 1;
    rect.top = mPosY + (current_height / 2) - 1;
    rect.right = rect.left + 2;
    rect.bottom = rect.top + 2;
    ClipCursor(&rect);
}

void GfxWindowBackendDXGI::ApplyMouseGrabClip() {
    RECT rect;
    rect.left = mPosX;
    rect.top = mPosY;
    rect.right = mPosX + current_width;
    rect.bottom = mPosY + current_height;
    ClipCursor(&rect);
}

void GfxWindowBackendDXGI::UpdateMousePrevPos() {
    if (!mHasMousePosition && mIsMouseHovered && !mIsMouseCaptured) {
        mHasMousePosition = true;

        int32_t x, y;
        GetMousePos(&x, &y);
        mPrevMouseCursorPos.x = x;
        mPrevMouseCursorPos.y = y;
    }
}

void GfxWindowBackendDXGI::HandleRawInputBuffered() {
    static UINT offset = -1;
    if (offset == -1) {
        offset = sizeof(RAWINPUTHEADER);

        BOOL isWow64;
        if (IsWow64Process(GetCurrentProcess(), &isWow64) && isWow64) {
            offset += 8;
        }
    }

    static BYTE* buf = NULL;
    static size_t bufsize;
    static const UINT RAWINPUT_BUFFER_SIZE_INCREMENT = 48 * 4; // 4 64-bit raw mouse packets

    while (true) {
        RAWINPUT* input = (RAWINPUT*)buf;
        UINT size = bufsize;
        UINT count = GetRawInputBuffer(input, &size, sizeof(RAWINPUTHEADER));

        if (!buf || (count == -1 && GetLastError() == ERROR_INSUFFICIENT_BUFFER)) {
            // realloc
            BYTE* newbuf = (BYTE*)std::realloc(buf, bufsize + RAWINPUT_BUFFER_SIZE_INCREMENT);
            if (!newbuf) {
                break;
            }
            buf = newbuf;
            bufsize += RAWINPUT_BUFFER_SIZE_INCREMENT;
            input = (RAWINPUT*)newbuf;
        } else if (count == -1) {
            // unhandled error
            DWORD err = GetLastError();
            fprintf(stderr, "Error: %lu\n", err);
        } else if (count == 0) {
            // there are no events
            break;
        } else {
            if (mIsMouseCaptured && mInFocus) {
                while (count--) {
                    if (input->header.dwType == RIM_TYPEMOUSE) {
                        RAWMOUSE* rawmouse = (RAWMOUSE*)((BYTE*)input + offset);
                        mRawMouseDeltaBuf.x += rawmouse->lLastX;
                        mRawMouseDeltaBuf.y += rawmouse->lLastY;
                    }
                    input = NEXTRAWINPUTBLOCK(input);
                }
            }
        }
    }
}

// port/gdx_course_edit_mouse.cpp — Course Edit wheel zoom accumulator.
extern "C" void gdx_course_edit_mouse_on_wheel(int detents);

static LRESULT CALLBACK gfx_dxgi_wnd_proc(HWND h_wnd, UINT message, WPARAM w_param, LPARAM l_param) {

    char fileName[256];
    Fast::WindowEvent event_impl;
    event_impl.Win32 = { h_wnd, static_cast<int>(message), static_cast<int>(w_param), static_cast<int>(l_param) };
    auto ctx = Ship::Context::GetInstance();
    if (ctx && ctx->GetWindow() && ctx->GetWindow()->GetGui()) {
        auto fast3dGui = std::dynamic_pointer_cast<Fast::Fast3dGui>(ctx->GetWindow()->GetGui());
        if (fast3dGui) {
            fast3dGui->HandleWindowEvents(event_impl);
        } else {
            static bool sWarnedOnce = false;
            if (!sWarnedOnce) {
                SPDLOG_ERROR("gfx_dxgi: Gui is not a Fast3dGui; cannot dispatch window event");
                sWarnedOnce = true;
            }
        }
    }
    std::tuple<HMONITOR, RECT, BOOL> newMonitor;
    GfxWindowBackendDXGI* self = reinterpret_cast<GfxWindowBackendDXGI*>(GetWindowLongPtr(h_wnd, GWLP_USERDATA));
    switch (message) {
        case WM_CREATE: {
            LPCREATESTRUCT lpcs = reinterpret_cast<LPCREATESTRUCT>(l_param);
            self = static_cast<GfxWindowBackendDXGI*>(lpcs->lpCreateParams);
            SetWindowLongPtr(h_wnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            break;
        }
        case WM_SIZE:
            self->current_width = LOWORD(l_param);
            self->current_height = HIWORD(l_param);
            GetMonitorAtCoords(self->monitor_list, self->mPosX, self->mPosY, self->current_width, self->current_height,
                               newMonitor);
            if (get<0>(newMonitor) != std::get<0>(self->mMonitor)) {
                self->mMonitor = newMonitor;
                GetMonitorHzPeriod(self->mMonitor, self->mDetectedHz, self->mDisplayPeriod);
            }
            break;
        case WM_MOVE:
            self->mPosX = GET_X_LPARAM(l_param);
            self->mPosY = GET_Y_LPARAM(l_param);
            GetMonitorAtCoords(self->monitor_list, self->mPosX, self->mPosY, self->current_width, self->current_height,
                               newMonitor);
            if (get<0>(newMonitor) != std::get<0>(self->mMonitor)) {
                self->mMonitor = newMonitor;
                GetMonitorHzPeriod(self->mMonitor, self->mDetectedHz, self->mDisplayPeriod);
            }
            break;
        case WM_CLOSE:
            self->Close();
            break;
        case WM_DPICHANGED: {
            RECT* const prcNewWindow = (RECT*)l_param;
            SetWindowPos(h_wnd, NULL, prcNewWindow->left, prcNewWindow->top, prcNewWindow->right - prcNewWindow->left,
                         prcNewWindow->bottom - prcNewWindow->top, SWP_NOZORDER | SWP_NOACTIVATE);
            self->mPosX = prcNewWindow->left;
            self->mPosY = prcNewWindow->top;
            self->current_width = prcNewWindow->right - prcNewWindow->left;
            self->current_height = prcNewWindow->bottom - prcNewWindow->top;
            break;
        }
        case WM_ENDSESSION:
            // This hopefully gives the game a chance to shut down, before windows kills it.
            if (w_param == TRUE) {
                self->Close();
            }
            break;
        case WM_ACTIVATEAPP:
            if (self->mOnAllKeysUp != nullptr) {
                self->mOnAllKeysUp();
            }
            break;
        case WM_KEYDOWN:
            self->OnKeydown(w_param, l_param);
            break;
        case WM_KEYUP:
            self->OnKeyup(w_param, l_param);
            break;
        case WM_LBUTTONDOWN:
            self->OnMouseButtonDown(0);
            break;
        case WM_LBUTTONUP:
            self->OnMouseButtonUp(0);
            break;
        case WM_MBUTTONDOWN:
            self->OnMouseButtonDown(1);
            break;
        case WM_MBUTTONUP:
            self->OnMouseButtonUp(1);
            break;
        case WM_RBUTTONDOWN:
            self->OnMouseButtonDown(2);
            break;
        case WM_RBUTTONUP:
            self->OnMouseButtonUp(2);
            break;
        case WM_XBUTTONDOWN: {
            int btn = 2 + GET_XBUTTON_WPARAM(w_param);
            self->OnMouseButtonDown(btn);
            break;
        }
        case WM_XBUTTONUP: {
            int btn = 2 + GET_XBUTTON_WPARAM(w_param);
            self->OnMouseButtonUp(btn);
            break;
        }
        case WM_MOUSEHWHEEL:
            self->mMouseWheel[0] = GET_WHEEL_DELTA_WPARAM(w_param) / WHEEL_DELTA;
            break;
        case WM_MOUSEWHEEL: {
            const int detents = GET_WHEEL_DELTA_WPARAM(w_param) / WHEEL_DELTA;
            self->mMouseWheel[1] = detents;
            // G-Diffuser Course Edit wheel zoom accumulator (port/gdx_course_edit_mouse.cpp).
            gdx_course_edit_mouse_on_wheel(detents);
            break;
        }
        case WM_INPUT: {
            // At this point the top most message should already be off the queue.
            // So we don't need to get it all, if mouse isn't captured.
            if (self->mIsMouseCaptured && self->mInFocus) {
                uint32_t size = sizeof(RAWINPUT);
                RAWINPUT raw[sizeof(RAWINPUT)];
                GetRawInputData((HRAWINPUT)l_param, RID_INPUT, raw, &size, sizeof(RAWINPUTHEADER));

                if (raw->header.dwType == RIM_TYPEMOUSE) {
                    self->mRawMouseDeltaBuf.x += raw->data.mouse.lLastX;
                    self->mRawMouseDeltaBuf.y += raw->data.mouse.lLastY;
                }
            }
            // The rest still needs to use that, to get them off the queue.
            self->HandleRawInputBuffered();
            break;
        }
        case WM_MOUSEMOVE:
            if (!self->mIsMouseHovered) {
                self->mIsMouseHovered = true;
                self->UpdateMousePrevPos();
            }
            break;
        case WM_MOUSELEAVE:
            self->mIsMouseHovered = false;
            self->mHasMousePosition = false;
            break;
        case WM_DROPFILES:
            DragQueryFileA((HDROP)w_param, 0, fileName, 256);
            Ship::Context::GetInstance()->GetFileDropMgr()->SetDroppedFile(fileName);
            break;
        case WM_DISPLAYCHANGE:
            self->monitor_list = GetMonitorList();
            GetMonitorAtCoords(self->monitor_list, self->mPosX, self->mPosY, self->current_width, self->current_height,
                               self->mMonitor);
            GetMonitorHzPeriod(self->mMonitor, self->mDetectedHz, self->mDisplayPeriod);
            break;
        case WM_SETFOCUS:
            self->mInFocus = true;
            ControllerUnblockGameInput(ALLOW_BACKGROUND_INPUTS_BLOCK_ID);
            if (self->mIsMouseCaptured) {
                self->ApplyMouseCaptureClip();
            } else if (self->mIsMouseGrabbed) {
                self->ApplyMouseGrabClip();
            }
            break;
        case WM_KILLFOCUS:
            if (auto ctx = Ship::Context::GetInstance(); ctx && ctx->GetConsoleVariables()) {
                if (!ctx->GetConsoleVariables()->GetInteger(CVAR_ALLOW_BACKGROUND_INPUTS, 1)) {
                    ControllerBlockGameInput(ALLOW_BACKGROUND_INPUTS_BLOCK_ID);
                }
            }
            if (self->mIsMouseGrabbed) {
                ClipCursor(nullptr);
            }
            self->mInFocus = false;
            break;
        default:
            return DefWindowProcW(h_wnd, message, w_param, l_param);
    }
    return 0;
}

static BOOL CALLBACK WIN_ResourceNameCallback(HMODULE hModule, LPCTSTR lpType, LPTSTR lpName, LONG_PTR lParam) {
    WNDCLASSEX* wcex = (WNDCLASSEX*)lParam;

    (void)lpType; /* We already know that the resource type is RT_GROUP_ICON. */

    /* We leave hIconSm as NULL as it will allow Windows to automatically
       choose the appropriate small icon size to suit the current DPI. */
    wcex->hIcon = LoadIcon(hModule, lpName);

    /* Do not bother enumerating any more. */
    return FALSE;
}

static uint64_t qpc_init, qpc_freq;

GfxWindowBackendDXGI::~GfxWindowBackendDXGI() {
}

void GfxWindowBackendDXGI::Init(const char* game_name, const char* gfx_api_name, bool start_in_fullscreen,
                                uint32_t width, uint32_t height, int32_t posX, int32_t posY) {
    LARGE_INTEGER lqpc_init, lqpc_freq;
    QueryPerformanceCounter(&lqpc_init);
    QueryPerformanceFrequency(&lqpc_freq);
    qpc_init = lqpc_init.QuadPart;
    qpc_freq = lqpc_freq.QuadPart;

    mTargetFps = 60;
    // Sized for a burst of presents, not one per tick. ApplyMaxFrameLatency consumes one token
    // at creation and never returns it, so the usable depth is this minus one -- at the old value
    // of 2, the second present of a burst blocked until the first retired on a vblank.
    //
    // Frame interpolation presents 2-3 sub-frames per 60 Hz tick on the thread that also runs the
    // simulation, so those blocks overran the tick, and one host-loop iteration advances the sim
    // exactly once with no catch-up: the game clock itself ran slow (measured 8.6 Hz on a 144 Hz
    // panel). A usable depth of 3 covers the largest burst the accumulator produces at 144 Hz.
    //
    // Cost is up to 3 frames of present latency instead of 1, which is the right trade: input lag
    // is a smoothness property, a slow game clock is a correctness fault. The non-interpolated
    // path presents once per tick and never queues deep enough to reach this limit.
    mMaxFrameLatency = 4;

    // Use high-resolution mTimer by default on Windows 10 (so that NtSetTimerResolution (...) hacks are not needed)
    mTimer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    // Fallback to low resolution mTimer if unsupported by the OS
    if (mTimer == nullptr) {
        mTimer = CreateWaitableTimer(nullptr, FALSE, nullptr);
    }

    // Prepare window title

    char title[512];
    wchar_t w_title[512];
    int len = snprintf(title, sizeof(title), "%s (%s)", game_name, gfx_api_name);
    mbstowcs(w_title, title, len + 1);

    // Create window
    WNDCLASSEXW wcex;

    wcex.cbSize = sizeof(WNDCLASSEX);

    wcex.style = CS_HREDRAW | CS_VREDRAW;
    wcex.lpfnWndProc = gfx_dxgi_wnd_proc;

    wcex.cbClsExtra = 0;
    wcex.cbWndExtra = 0;
    wcex.hInstance = GetModuleHandle(nullptr);
    wcex.hIcon = nullptr;
    wcex.hIconSm = nullptr;
    wcex.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wcex.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wcex.lpszMenuName = nullptr;
    wcex.lpszClassName = WINCLASS_NAME;
    wcex.hIconSm = nullptr;

    EnumResourceNames(wcex.hInstance, RT_GROUP_ICON, WIN_ResourceNameCallback, (LONG_PTR)&wcex);

    ATOM winclass = RegisterClassExW(&wcex);

    RECT wr = { 0, 0, width, height };
    AdjustWindowRect(&wr, WS_OVERLAPPEDWINDOW, FALSE);
    current_width = wr.right - wr.left;
    current_height = wr.bottom - wr.top;
    monitor_list = GetMonitorList();
    posX = posX;
    posY = posY;
    if (!GetMonitorAtCoords(monitor_list, posX, posY, current_width, current_height, mMonitor)) {
        posX = 100;
        posY = 100;
    }

    h_wnd = CreateWindowW(WINCLASS_NAME, w_title, WS_OVERLAPPEDWINDOW, posX + wr.left, posY + wr.top, current_width,
                          current_height, nullptr, nullptr, nullptr, this);

    LoadDxgi();

    ShowWindow(h_wnd, SW_SHOW);
    UpdateWindow(h_wnd);

    // Get refresh rate
    GetMonitorHzPeriod(mMonitor, mDetectedHz, mDisplayPeriod);

    if (start_in_fullscreen) {
        ToggleBorderlessWindowFullScreen(true, false);
    }

    DragAcceptFiles(h_wnd, TRUE);

    // Mouse init
    mRawInputDevice[0].usUsagePage = HID_USAGE_PAGE_GENERIC;
    mRawInputDevice[0].usUsage = HID_USAGE_GENERIC_MOUSE;
    mRawInputDevice[0].dwFlags = RIDEV_INPUTSINK;
    mRawInputDevice[0].hwndTarget = h_wnd;
    RegisterRawInputDevices(mRawInputDevice, 1, sizeof(mRawInputDevice[0]));
}

void GfxWindowBackendDXGI::SetFullscreenChangedCallback(void (*onFullscreenChanged)(bool is_now_fullscreen)) {
    mOnFullscreenChanged = onFullscreenChanged;
}

void GfxWindowBackendDXGI::SetCursorVisibility(bool visible) {
    // https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-showcursor
    // https://devblogs.microsoft.com/oldnewthing/20091217-00/?p=15643
    // ShowCursor uses a counter, not a boolean value, and increments or decrements that value when called
    // This means we need to keep calling it until we get the value we want

    //
    //  NOTE:  If you continue calling until you "get the value you want" and there is no mouse attached,
    //  it will lock the software up.  Windows always returns -1 if there is no mouse!
    //

    const int _MAX_TRIES = 15; // Prevent spinning infinitely if no mouse is plugged in

    int cursorVisibilityTries = 0;
    int cursorVisibilityCounter;
    if (visible) {
        do {
            cursorVisibilityCounter = ShowCursor(true);
        } while (cursorVisibilityCounter < 0 && ++cursorVisibilityTries < _MAX_TRIES);
    } else {
        do {
            cursorVisibilityCounter = ShowCursor(false);
        } while (cursorVisibilityCounter >= 0);
    }
}

void GfxWindowBackendDXGI::SetMousePos(int32_t x, int32_t y) {
    SetCursorPos(x, y);
}

void GfxWindowBackendDXGI::GetMousePos(int32_t* x, int32_t* y) {
    POINT p;
    GetCursorPos(&p);
    ScreenToClient(h_wnd, &p);
    *x = p.x;
    *y = p.y;
}

void GfxWindowBackendDXGI::GetMouseDelta(int32_t* x, int32_t* y) {
    if (mIsMouseCaptured) {
        *x = mRawMouseDeltaBuf.x;
        *y = mRawMouseDeltaBuf.y;
        mRawMouseDeltaBuf.x = 0;
        mRawMouseDeltaBuf.y = 0;
    } else if (mHasMousePosition) {
        int32_t current_x, current_y;
        GetMousePos(&current_x, &current_y);
        *x = current_x - mPrevMouseCursorPos.x;
        *y = current_y - mPrevMouseCursorPos.y;
        mPrevMouseCursorPos.x = current_x;
        mPrevMouseCursorPos.y = current_y;
    } else {
        *x = 0;
        *y = 0;
    }
}

void GfxWindowBackendDXGI::GetMouseWheel(float* x, float* y) {
    *x = mMouseWheel[0];
    *y = mMouseWheel[1];
    mMouseWheel[0] = 0;
    mMouseWheel[1] = 0;
}

bool GfxWindowBackendDXGI::GetMouseState(uint32_t btn) {
    return mMousePressed[btn];
}

void GfxWindowBackendDXGI::SetMouseCapture(bool capture) {
    mIsMouseCaptured = capture;
    if (capture) {
        ApplyMouseCaptureClip();
        SetCursorVisibility(false);
        SetCapture(h_wnd);
        mHasMousePosition = false;
    } else {
        // Restore the window grab if one was requested; otherwise release the cursor.
        if (mIsMouseGrabbed) {
            ApplyMouseGrabClip();
        } else {
            ClipCursor(nullptr);
        }
        SetCursorVisibility(true);
        ReleaseCapture();
        UpdateMousePrevPos();
    }
}

bool GfxWindowBackendDXGI::IsMouseCaptured() {
    return mIsMouseCaptured;
}

void GfxWindowBackendDXGI::SetMouseGrab(bool grab) {
    if (mIsMouseGrabbed == grab) {
        return;
    }
    mIsMouseGrabbed = grab;
    if (mIsMouseCaptured) {
        // Relative-mode capture owns the cursor; it will restore the grab on release.
        return;
    }
    if (grab) {
        ApplyMouseGrabClip();
    } else {
        ClipCursor(nullptr);
    }
}

void GfxWindowBackendDXGI::SetFullscreen(bool enable) {
    ToggleBorderlessWindowFullScreen(enable, true);
}

void GfxWindowBackendDXGI::GetActiveWindowRefreshRate(uint32_t* refresh_rate) {
    *refresh_rate = (uint32_t)roundf(mDetectedHz);
}

void GfxWindowBackendDXGI::SetKeyboardCallbacks(bool (*onKeyDown)(int scancode), bool (*onKeyUp)(int scancode),
                                                void (*onnAllKeysUp)()) {
    mOnKeyDown = onKeyDown;
    mOnKeyUp = onKeyUp;
    mOnAllKeysUp = onnAllKeysUp;
}

void GfxWindowBackendDXGI::SetMouseCallbacks(bool (*on_btn_down)(int btn), bool (*on_btn_up)(int btn)) {
    mOnMouseButtonDown = on_btn_down;
    mOnMouseButtonUp = on_btn_up;
}

void GfxWindowBackendDXGI::GetDimensions(uint32_t* width, uint32_t* height, int32_t* posX, int32_t* posY) {
    *width = current_width;
    *height = current_height;
    *posX = mPosX;
    *posY = mPosY;
}

void GfxWindowBackendDXGI::SetDimensions(uint32_t width, uint32_t height, int32_t posX, int32_t posY) {
    current_width = width;
    current_height = height;
    mPosX = posX;
    mPosY = posY;
    if (h_wnd) {
        RECT wr = { mPosX, mPosY, mPosX + static_cast<int32_t>(current_width),
                    mPosY + static_cast<int32_t>(current_height) };
        if (!mFullScreen) {
            AdjustWindowRect(&wr, WS_OVERLAPPEDWINDOW, FALSE);
            SetWindowPos(h_wnd, nullptr, wr.left, wr.top, wr.right - wr.left, wr.bottom - wr.top, SWP_FRAMECHANGED);
        }
    }
}

Ship::WindowRect GfxWindowBackendDXGI::GetPrimaryMonitorRect() {
    auto monitors = GetMonitorList();
    for (const auto& [hmon, rect, isPrimary] : monitors) {
        if (isPrimary) {
            return { rect.left, rect.top, rect.right, rect.bottom };
        }
    }
    return { 0, 0, 0, 0 };
}

void GfxWindowBackendDXGI::HandleEvents() {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) {
            mIsRunning = false;
            break;
        }
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
}

static uint64_t qpc_to_ns(uint64_t qpc) {
    return qpc / qpc_freq * NANOSECOND_IN_SECOND + qpc % qpc_freq * NANOSECOND_IN_SECOND / qpc_freq;
}

static uint64_t qpc_to_100ns(uint64_t qpc) {
    return qpc / qpc_freq * _100NANOSECONDS_IN_SECOND + qpc % qpc_freq * _100NANOSECONDS_IN_SECOND / qpc_freq;
}

// Diagnostic for the one place that refuses a present. The decision is
// `vsyncs_to_wait = (desired_present_time - last_present_end) / vsync_interval`, refused when
// non-positive, so the fields worth reporting are the schedule's drift from real time and the
// number of presents DXGI still has queued, which pushes last_end forward.
//
// Gated on GDX_DIAG_LIMITER and emitted once per 300 calls; disabled cost is one bool test.
namespace {
struct GdxLimiterDiag {
    unsigned long long calls = 0, accepts = 0, dropLate = 0, dropRound = 0, noStats = 0, resync = 0;
    unsigned long long intervalRejects = 0; // estimates overruled by the panel's known interval
    double lastWait = 0.0;      // vsyncs_to_wait at the most recent refusal (pre-rounding)
    long long lastDesiredNs = 0; // where the internal schedule wanted this present
    long long lastEndNs = 0;     // when the previous present is expected to retire
    unsigned lastQueued = 0;     // vsyncs still queued in the swapchain
    unsigned long long lastInterval = 0; // measured vsync interval, ns
};
GdxLimiterDiag gLimiterDiag;

bool GdxLimiterDiagOn() {
    static const bool on = [] {
        const char* e = getenv("GDX_DIAG_LIMITER");
        return e != nullptr && e[0] != '\0' && !(e[0] == '0' && e[1] == '\0');
    }();
    return on;
}

void GdxLimiterDiagEmit() {
    SPDLOG_WARN("[limiter-diag] calls={} accept={} drop_late={} drop_round={} resync={} nostats={} "
                "int_reject={} "
                "| last refusal: wait={:.3f} delta_ms={:.3f} desired={} last_end={} queued={} vsync_int_ns={}",
                gLimiterDiag.calls, gLimiterDiag.accepts, gLimiterDiag.dropLate, gLimiterDiag.dropRound,
                gLimiterDiag.resync, gLimiterDiag.noStats, gLimiterDiag.intervalRejects, gLimiterDiag.lastWait,
                (double)(gLimiterDiag.lastDesiredNs - gLimiterDiag.lastEndNs) / 1.0e6,
                gLimiterDiag.lastDesiredNs, gLimiterDiag.lastEndNs, gLimiterDiag.lastQueued,
                gLimiterDiag.lastInterval);
}
} // namespace

bool GfxWindowBackendDXGI::IsFrameReady() {
    const bool gdxDiag = GdxLimiterDiagOn();
    if (gdxDiag && (++gLimiterDiag.calls % 300ull) == 0ull) {
        GdxLimiterDiagEmit();
    }
    DXGI_FRAME_STATISTICS stats;
    if (swap_chain->GetFrameStatistics(&stats) == S_OK &&
        (stats.SyncRefreshCount != 0 || stats.SyncQPCTime.QuadPart != 0ULL)) {
        {
            LARGE_INTEGER t0;
            QueryPerformanceCounter(&t0);
            // printf("Get frame stats: %llu\n", (unsigned long long)(t0.QuadPart - qpc_init));
        }
        // printf("stats: %u %u %u %u %u %.6f\n", mPendingFrameStats.rbegin()->first,
        // mPendingFrameStats.rbegin()->second, stats.PresentCount, stats.PresentRefreshCount,
        // stats.SyncRefreshCount, (double)(stats.SyncQPCTime.QuadPart - qpc_init) / qpc_freq);
        if (mFrameStats.empty() || mFrameStats.rbegin()->second.PresentCount != stats.PresentCount) {
            mFrameStats.insert(std::make_pair(stats.PresentCount, stats));
        }
        if (mFrameStats.size() > 3) {
            mFrameStats.erase(mFrameStats.begin());
        }
    }
    if (!mFrameStats.empty()) {
        while (!mPendingFrameStats.empty() && mPendingFrameStats.begin()->first < mFrameStats.rbegin()->first) {
            mPendingFrameStats.erase(mPendingFrameStats.begin());
        }
    }
    while (mPendingFrameStats.size() > 40) {
        // Just make sure the list doesn't grow too large if GetFrameStatistics fails.
        mPendingFrameStats.erase(mPendingFrameStats.begin());

        // These are not that useful anymore
        mFrameStats.clear();
    }

    mFrameTimeStamp += FRAME_INTERVAL_NS_NUMERATOR;

    if (mFrameStats.size() >= 2) {
        DXGI_FRAME_STATISTICS* first = &mFrameStats.begin()->second;
        DXGI_FRAME_STATISTICS* last = &mFrameStats.rbegin()->second;
        uint64_t sync_qpc_diff = last->SyncQPCTime.QuadPart - first->SyncQPCTime.QuadPart;
        UINT sync_vsync_diff = last->SyncRefreshCount - first->SyncRefreshCount;
        UINT present_vsync_diff = last->PresentRefreshCount - first->PresentRefreshCount;
        UINT present_diff = last->PresentCount - first->PresentCount;

        if (sync_vsync_diff == 0) {
            sync_vsync_diff = 1;
        }

        double estimated_vsync_interval = (double)sync_qpc_diff / (double)sync_vsync_diff;
        uint64_t estimated_vsync_interval_ns = qpc_to_ns(estimated_vsync_interval);
        // printf("Estimated vsync_interval: %d\n", (int)estimated_vsync_interval_ns);
        if (estimated_vsync_interval_ns < 2000 || estimated_vsync_interval_ns > 1000000000) {
            // Unreasonable, maybe a monitor change
            estimated_vsync_interval_ns = 16666666;
            estimated_vsync_interval = (double)estimated_vsync_interval_ns * qpc_freq / 1000000000;
        }
        // Anchor the estimate to the panel's actual refresh interval. Deleting this block
        // reproduced a deterministic race-entry crash (3/3), so GDX_NO_VSYNC_CLAMP disables the
        // clamp's effect for A/B testing while leaving the code in place.
        static const bool sNoClamp = [] {
            const char* e = getenv("GDX_NO_VSYNC_CLAMP");
            return e != nullptr && e[0] != 0 && !(e[0] == 0x30 && e[1] == 0);
        }();
        if (!sNoClamp && mDetectedHz >= 20.0f) {
            const uint64_t expected_ns = (uint64_t)(1000000000.0 / (double)mDetectedHz);
            if (estimated_vsync_interval_ns < (expected_ns * 3) / 4 ||
                estimated_vsync_interval_ns > (expected_ns * 5) / 4) {
                estimated_vsync_interval_ns = expected_ns;
                estimated_vsync_interval = (double)estimated_vsync_interval_ns * qpc_freq / 1000000000;
                if (gdxDiag) {
                    ++gLimiterDiag.intervalRejects;
                }
            }
        }

        UINT queued_vsyncs = 0;
        bool is_first = true;
        for (const std::pair<UINT, UINT>& p : mPendingFrameStats) {
            /*if (is_first && mZeroLatency) {
                is_first = false;
                continue;
            }*/
            queued_vsyncs += p.second;
        }

        uint64_t last_frame_present_end_qpc =
            (last->SyncQPCTime.QuadPart - qpc_init) + estimated_vsync_interval * queued_vsyncs;
        uint64_t last_end_ns = qpc_to_ns(last_frame_present_end_qpc);

        double vsyncs_to_wait = (double)(int64_t)(mFrameTimeStamp / FRAME_INTERVAL_NS_DENOMINATOR - last_end_ns) /
                                estimated_vsync_interval_ns;
        // printf("ts: %llu, last_end_ns: %llu, Init v: %f\n", mFrameTimeStamp / 3, last_end_ns,
        // vsyncs_to_wait);

        if (vsyncs_to_wait <= 0) {
            // Too late
            if (gdxDiag) {
                gLimiterDiag.lastWait = vsyncs_to_wait;
                gLimiterDiag.lastDesiredNs = (long long)(mFrameTimeStamp / FRAME_INTERVAL_NS_DENOMINATOR);
                gLimiterDiag.lastEndNs = (long long)last_end_ns;
                gLimiterDiag.lastQueued = queued_vsyncs;
                gLimiterDiag.lastInterval = estimated_vsync_interval_ns;
            }

            if ((int64_t)(mFrameTimeStamp / FRAME_INTERVAL_NS_DENOMINATOR - last_end_ns) < -66666666) {
                // The application must have been paused or similar
                if (gdxDiag) {
                    ++gLimiterDiag.resync;
                }
                vsyncs_to_wait = round(((double)FRAME_INTERVAL_NS_NUMERATOR / FRAME_INTERVAL_NS_DENOMINATOR) /
                                       estimated_vsync_interval_ns);
                if (vsyncs_to_wait < 1) {
                    vsyncs_to_wait = 1;
                }
                mFrameTimeStamp =
                    FRAME_INTERVAL_NS_DENOMINATOR * (last_end_ns + vsyncs_to_wait * estimated_vsync_interval_ns);
            } else {
                // Drop frame
                // printf("Dropping frame\n");
                if (gdxDiag) {
                    ++gLimiterDiag.dropLate;
                }
                mDroppedFrame = true;
                return false;
            }
        }
        double orig_wait = vsyncs_to_wait;
        if (floor(vsyncs_to_wait) != vsyncs_to_wait) {
            uint64_t left = last_end_ns + floor(vsyncs_to_wait) * estimated_vsync_interval_ns;
            uint64_t right = last_end_ns + ceil(vsyncs_to_wait) * estimated_vsync_interval_ns;
            uint64_t adjusted_desired_time =
                mFrameTimeStamp / FRAME_INTERVAL_NS_DENOMINATOR +
                (last_end_ns + (FRAME_INTERVAL_NS_NUMERATOR / FRAME_INTERVAL_NS_DENOMINATOR) >
                         mFrameTimeStamp / FRAME_INTERVAL_NS_DENOMINATOR
                     ? 2000000
                     : -2000000);
            int64_t diff_left = adjusted_desired_time - left;
            int64_t diff_right = right - adjusted_desired_time;
            if (diff_left < 0) {
                diff_left = -diff_left;
            }
            if (diff_right < 0) {
                diff_right = -diff_right;
            }
            if (diff_left < diff_right) {
                vsyncs_to_wait = floor(vsyncs_to_wait);
            } else {
                vsyncs_to_wait = ceil(vsyncs_to_wait);
            }
            if (vsyncs_to_wait == 0) {
                // printf("vsyncs_to_wait became 0 so dropping frame\n");
                if (gdxDiag) {
                    ++gLimiterDiag.dropRound;
                    gLimiterDiag.lastWait = orig_wait;
                    gLimiterDiag.lastDesiredNs = (long long)(mFrameTimeStamp / FRAME_INTERVAL_NS_DENOMINATOR);
                    gLimiterDiag.lastEndNs = (long long)last_end_ns;
                    gLimiterDiag.lastQueued = queued_vsyncs;
                    gLimiterDiag.lastInterval = estimated_vsync_interval_ns;
                }
                mDroppedFrame = true;
                return false;
            }
        }
    } else if (gdxDiag) {
        // Counted apart from ordinary accepts: with fewer than two samples the pacing block is
        // skipped entirely, so a large count here means the limiter never engaged at all.
        ++gLimiterDiag.noStats;
    }
    if (gdxDiag) {
        ++gLimiterDiag.accepts;
    }
    return true;
}

/* Window-level capture: dumps the swapchain backbuffer to numbered 24bpp BMPs just before
   Present. The other capture path (GDX_CAPTURE_FRAMES) reads the 320x240 aspect-blind N64 frame
   mirror, which cannot show window-level artifacts such as wipe judder.

   GDX_CAPTURE_WINDOW=<start>:<count> captures <count> presented frames from present index
   <start>, writing gdxwin_<index>.bmp next to the executable. Off when unset. */
extern "C" int gGameMode; // decomp global; GET_MODE = gGameMode & 0x1F

static void GdxMaybeCaptureWindowFrame(IDXGISwapChain1* swapChain, IUnknown* swapChainDevice) {
    // Parsed once; unset or malformed leaves the facility off at a cost of one bool test.
    static int sState = 0; // 0 = unparsed, 1 = active, -1 = disabled
    static unsigned int sStart = 0;
    static unsigned int sCount = 0;
    static unsigned int sPresentIndex = 0;

    if (sState == -1) {
        return;
    }
    if (sState == 0) {
        const char* env = getenv("GDX_CAPTURE_WINDOW");
        unsigned int start = 0, count = 0;
        if (env != nullptr && sscanf(env, "%u:%u", &start, &count) == 2 && count > 0) {
            sStart = start;
            sCount = count;
            sState = 1;
        } else {
            sState = -1;
            return;
        }
    }

    const unsigned int index = sPresentIndex++;

    /* Log the present index at every game-mode change so a capture window can be aimed at a
       specific transition without guessing the present number. */
    {
        static int sLastMode = -1;
        static int sModeLogs = 0;
        const int mode = gGameMode & 0x1F;
        if (mode != sLastMode && sModeLogs < 32) {
            sLastMode = mode;
            ++sModeLogs;
            FILE* mf = fopen("gdxwin-modes.txt", "a");
            if (mf != nullptr) {
                fprintf(mf, "present=%u gameMode=0x%X\n", index, mode);
                fclose(mf);
            }
        }
    }

    if (index < sStart || index >= sStart + sCount) {
        return;
    }
    if (swapChain == nullptr || swapChainDevice == nullptr) {
        return;
    }

    // The swapchain device is a D3D11 device on DX11 and a command queue on DX12. Only DX11 is
    // handled, so a failed QueryInterface just skips.
    ComPtr<ID3D11Device> device;
    if (FAILED(swapChainDevice->QueryInterface(__uuidof(ID3D11Device), (void**)device.GetAddressOf()))) {
        return;
    }
    ComPtr<ID3D11DeviceContext> ctx;
    device->GetImmediateContext(ctx.GetAddressOf());
    if (ctx == nullptr) {
        return;
    }

    ComPtr<ID3D11Texture2D> backbuffer;
    if (FAILED(swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)backbuffer.GetAddressOf()))) {
        return;
    }
    D3D11_TEXTURE2D_DESC desc;
    backbuffer->GetDesc(&desc);

    D3D11_TEXTURE2D_DESC stagingDesc = desc;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.BindFlags = 0;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    stagingDesc.MiscFlags = 0;
    stagingDesc.SampleDesc.Count = 1;
    stagingDesc.SampleDesc.Quality = 0;

    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(device->CreateTexture2D(&stagingDesc, nullptr, staging.GetAddressOf()))) {
        return;
    }
    // Backbuffers are single-sampled; a straight CopyResource is valid.
    ctx->CopyResource(staging.Get(), backbuffer.Get());

    D3D11_MAPPED_SUBRESOURCE mapped;
    if (FAILED(ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
        return;
    }

    const unsigned int width = desc.Width;
    const unsigned int height = desc.Height;
    // Flip-model backbuffers are usually B8G8R8A8; R8G8B8A8 is handled too.
    const bool isBgra = (desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM ||
                         desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB);

    const unsigned int rowBytes = (width * 3u + 3u) & ~3u;
    const unsigned int imageBytes = rowBytes * height;
    const unsigned int fileBytes = 14u + 40u + imageBytes;
    unsigned char header[54] = { 0 };
    header[0] = 'B';
    header[1] = 'M';
    header[2] = (unsigned char)(fileBytes);
    header[3] = (unsigned char)(fileBytes >> 8);
    header[4] = (unsigned char)(fileBytes >> 16);
    header[5] = (unsigned char)(fileBytes >> 24);
    header[10] = 54; // pixel data offset
    header[14] = 40; // BITMAPINFOHEADER size
    header[18] = (unsigned char)(width);
    header[19] = (unsigned char)(width >> 8);
    header[20] = (unsigned char)(width >> 16);
    header[21] = (unsigned char)(width >> 24);
    header[22] = (unsigned char)(height);
    header[23] = (unsigned char)(height >> 8);
    header[24] = (unsigned char)(height >> 16);
    header[25] = (unsigned char)(height >> 24);
    header[26] = 1;  // planes
    header[28] = 24; // bpp

    char path[64];
    snprintf(path, sizeof(path), "gdxwin_%05u.bmp", index);
    FILE* f = fopen(path, "wb");
    if (f == nullptr) {
        ctx->Unmap(staging.Get(), 0);
        return;
    }
    fwrite(header, 1, sizeof(header), f);

    std::vector<unsigned char> row(rowBytes, 0);
    for (unsigned int y = 0; y < height; y++) {
        // BMP rows are bottom-up; the backbuffer is top-down.
        const unsigned char* src =
            (const unsigned char*)mapped.pData + (size_t)(height - 1 - y) * mapped.RowPitch;
        for (unsigned int x = 0; x < width; x++) {
            const unsigned char c0 = src[x * 4 + 0];
            const unsigned char c1 = src[x * 4 + 1];
            const unsigned char c2 = src[x * 4 + 2];
            unsigned char r, g, b;
            if (isBgra) {
                b = c0;
                g = c1;
                r = c2;
            } else {
                r = c0;
                g = c1;
                b = c2;
            }
            row[x * 3 + 0] = b;
            row[x * 3 + 1] = g;
            row[x * 3 + 2] = r;
        }
        fwrite(row.data(), 1, rowBytes, f);
    }
    fclose(f);
    ctx->Unmap(staging.Get(), 0);
}

void GfxWindowBackendDXGI::SwapBuffersBegin() {
    // mLengthInVsyncFrames (now mVsyncEnabled) was used as present interval. Present interval >1 (aka fractional
    // V-Sync) breaks VRR and introduces even more input lag than capping via normal V-Sync does. Get the present
    // interval the user wants instead (V-Sync toggle).
    mVsyncEnabled = Ship::Context::GetInstance()->GetConsoleVariables()->GetInteger(CVAR_VSYNC_ENABLED, 1) ? 1 : 0;

    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);

    // With vsync on and the target within tolerance of the panel's refresh rate, Present()
    // below already blocks on the hardware refresh; running this software deadline wait too
    // gives two independently-clocked pacers beating against each other, which shows up as
    // judder and shimmer on high-frequency detail. The tolerance absorbs NTSC/VRR drift.
    double vsyncTolerance = mDetectedHz > 0.0 ? std::max(1.0, mDetectedHz * 0.015) : 1.0;
    bool vsyncPaced = mVsyncEnabled != 0 && mDetectedHz > 0.0 &&
                      fabs((double)mTargetFps - mDetectedHz) <= vsyncTolerance;
    static bool sVsyncPacedLogged = false;
    static bool sWasVsyncPaced = false;
    if (vsyncPaced != sWasVsyncPaced || !sVsyncPacedLogged) {
        sWasVsyncPaced = vsyncPaced;
        sVsyncPacedLogged = true;
        // WARN, not INFO: Release builds initialise the logger at spdlog::level::warn (Context.h
        // InitLogging default), and this line carries the two values that decide whether the
        // software limiter drops a present.
        SPDLOG_WARN("[pacer] vsync-paced: {} software wait target={:.2f} refresh={:.2f}",
                    vsyncPaced ? "skipping" : "resuming", (double)mTargetFps, mDetectedHz);
    }

    if (!vsyncPaced) {
        int64_t next = qpc_to_100ns(mPreviousPresentTime.QuadPart) +
                       FRAME_INTERVAL_NS_NUMERATOR / (FRAME_INTERVAL_NS_DENOMINATOR * 100);
        int64_t left = next - qpc_to_100ns(t.QuadPart) - 15000UL;
        if (left > 0) {
            LARGE_INTEGER li;
            li.QuadPart = -left;
            SetWaitableTimer(mTimer, &li, 0, nullptr, nullptr, false);
            WaitForSingleObject(mTimer, INFINITE);
        }

        QueryPerformanceCounter(&t);
        t.QuadPart = qpc_to_100ns(t.QuadPart);
        while (t.QuadPart < next) {
            YieldProcessor();
            QueryPerformanceCounter(&t);
            t.QuadPart = qpc_to_100ns(t.QuadPart);
        }
        QueryPerformanceCounter(&t);
    }
    // On the skip path t is still the entry sample. Keeping mPreviousPresentTime near-now
    // rather than stale means the software wait resumes from a sane deadline, instead of a
    // catch-up burst, if the target fps later drifts off the refresh rate.
    mPreviousPresentTime = t;
    // Before Present, so the dumped frame is exactly what is about to be shown.
    GdxMaybeCaptureWindowFrame(swap_chain.Get(), mSwapChainDevice.Get());
    if (mTearingSupport && !mVsyncEnabled) {
        // 512: DXGI_PRESENT_ALLOW_TEARING - allows for true V-Sync off with flip model
        ThrowIfFailed(swap_chain->Present(mVsyncEnabled, DXGI_PRESENT_ALLOW_TEARING));
    } else {
        ThrowIfFailed(swap_chain->Present(mVsyncEnabled, 0));
    }

    UINT this_present_id;
    if (swap_chain->GetLastPresentCount(&this_present_id) == S_OK) {
        mPendingFrameStats.insert(std::make_pair(this_present_id, mVsyncEnabled));
    }
    mDroppedFrame = false;
}

void GfxWindowBackendDXGI::SwapBuffersEnd() {
    LARGE_INTEGER t0, t1, t2;
    QueryPerformanceCounter(&t0);
    QueryPerformanceCounter(&t1);

    if (mAppliedMaxFrameLatency > mMaxFrameLatency) {
        // If latency is decreased, you have to wait the same amout of times as the old latency was set to
        int times_to_wait = mAppliedMaxFrameLatency;
        int latency = mMaxFrameLatency;
        mMaxFrameLatency = 1;
        ApplyMaxFrameLatency(false);
        if (mWaitableObject != nullptr) {
            while (times_to_wait > 0) {
                WaitForSingleObject(mWaitableObject, INFINITE);
                times_to_wait--;
            }
        }
        mMaxFrameLatency = latency;
        ApplyMaxFrameLatency(false);

        return; // Make sure we don't wait a second time on the waitable object, since that would hang the program
    } else if (mAppliedMaxFrameLatency != mMaxFrameLatency) {
        ApplyMaxFrameLatency(false);
    }

    if (!mDroppedFrame) {
        if (mWaitableObject != nullptr) {
            WaitForSingleObject(mWaitableObject, INFINITE);
        }
        // else TODO: maybe sleep until some estimated time the frame will be shown to reduce lag
    }

    DXGI_FRAME_STATISTICS stats;
    swap_chain->GetFrameStatistics(&stats);

    QueryPerformanceCounter(&t2);

    mZeroLatency = !mPendingFrameStats.empty() && mPendingFrameStats.rbegin()->first == stats.PresentCount;

    // printf(L"done %I64u gpu:%d wait:%d freed:%I64u frame:%u %u monitor:%u t:%I64u\n", (unsigned long
    // long)(t0.QuadPart - qpc_init), (int)(t1.QuadPart - t0.QuadPart), (int)(t2.QuadPart - t0.QuadPart), (unsigned
    // long long)(t2.QuadPart - qpc_init), mPendingFrameStats.rbegin()->first, stats.PresentCount,
    // stats.SyncRefreshCount, (unsigned long long)(stats.SyncQPCTime.QuadPart - qpc_init));
}

double GfxWindowBackendDXGI::GetTime() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (double)(t.QuadPart - qpc_init) / qpc_freq;
}

int GfxWindowBackendDXGI::GetTargetFps() {
    return mTargetFps;
}

void GfxWindowBackendDXGI::SetTargetFps(int fps) {
    uint32_t old_fps = mTargetFps;
    uint64_t t0 = mFrameTimeStamp / old_fps;
    uint32_t t1 = mFrameTimeStamp % old_fps;
    mTargetFps = fps;
    mFrameTimeStamp = t0 * mTargetFps + t1 * mTargetFps / old_fps;
}

void GfxWindowBackendDXGI::SetMaxFrameLatency(int latency) {
    mMaxFrameLatency = latency;
}

void GfxWindowBackendDXGI::CreateFactoryAndDevice(bool debug, int d3d_version, class GfxRenderingAPIDX11* self,
                                                  bool (*createFunc)(class GfxRenderingAPIDX11* self,
                                                                     bool SoftwareRenderer)) {
    if (CreateDXGIFactory2 != nullptr) {
        ThrowIfFailed(CreateDXGIFactory2(debug ? DXGI_CREATE_FACTORY_DEBUG : 0, __uuidof(IDXGIFactory2), &mFactory));
    } else {
        ThrowIfFailed(CreateDXGIFactory1(__uuidof(IDXGIFactory2), &mFactory));
    }

    ComPtr<IDXGIFactory4> factory4;
    if (mFactory->QueryInterface(__uuidof(IDXGIFactory4), &factory4) == S_OK) {
        mDXGI11_4 = true;

        ComPtr<IDXGIFactory5> factory;
        HRESULT hr = mFactory.As(&factory);
        BOOL allowTearing = FALSE;
        if (SUCCEEDED(hr)) {
            hr = factory->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allowTearing, sizeof(allowTearing));
        }

        mTearingSupport = SUCCEEDED(hr) && allowTearing;
    }
    // Try preferred hardware adapter and then try software adapter (WARP), if that fails.
    // Maybe we can try a different renderer (like OpenGL) here first before software?
    if (!createFunc(self, false) && !createFunc(self, true)) {
        SPDLOG_CRITICAL("Creating D3D renderer failed. Exiting");
        MessageBoxA(self->mWindowBackend->GetWindowHandle(), "Creating D3D renderer failed. Exiting", "Error",
                    MB_OK | MB_ICONERROR);
        throw;
    }
}

void GfxWindowBackendDXGI::CreateSwapChain(IUnknown* mDevice, std::function<void()>&& before_destroy_fn) {
    bool win8 = IsWindows8OrGreater();            // DXGI_SCALING_NONE is only supported on Win8 and beyond
    bool dxgi_13 = CreateDXGIFactory2 != nullptr; // DXGI 1.3 introduced waitable object

    DXGI_SWAP_CHAIN_DESC1 swap_chain_desc = {};
    // 4, not 3: the flip models count the buffer on screen, so BufferCount N caps the queue at
    // N-1 un-retired presents, and interpolation submits bursts of up to 3 per tick. At 3 the
    // third present had nowhere to go no matter what SetMaximumFrameLatency allowed -- see the
    // mMaxFrameLatency comment in Init for why that made the game clock run slow.
    swap_chain_desc.BufferCount = 4;
    swap_chain_desc.Width = 0;
    swap_chain_desc.Height = 0;
    swap_chain_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swap_chain_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swap_chain_desc.Scaling = win8 ? DXGI_SCALING_NONE : DXGI_SCALING_STRETCH;
    swap_chain_desc.SwapEffect =
        mDXGI11_4 ? DXGI_SWAP_EFFECT_FLIP_DISCARD : // Introduced in DXGI 1.4 and Windows 10
            DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL; // Apparently flip sequential was also backported to Win 7 Platform Update
    swap_chain_desc.Flags = dxgi_13 ? DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT : 0;
    if (mTearingSupport) {
        swap_chain_desc.Flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING; // Now we can use DXGI_PRESENT_ALLOW_TEARING
    }
    swap_chain_desc.SampleDesc.Count = 1;

    ThrowIfFailed(mFactory->CreateSwapChainForHwnd(mDevice, h_wnd, &swap_chain_desc, nullptr, nullptr, &swap_chain));
    ThrowIfFailed(mFactory->MakeWindowAssociation(h_wnd, DXGI_MWA_NO_ALT_ENTER));

    ApplyMaxFrameLatency(true);

    ThrowIfFailed(swap_chain->GetDesc1(&swap_chain_desc));

    mSwapChainDevice = mDevice;
    mBeforeDestroySwapChainFn = std::move(before_destroy_fn);
}

bool GfxWindowBackendDXGI::IsRunning() {
    return mIsRunning;
}

HWND GfxWindowBackendDXGI::GetWindowHandle() {
    return h_wnd;
}

IDXGISwapChain1* GfxWindowBackendDXGI::GetSwapChain() {
    return swap_chain.Get();
}

const char* GfxWindowBackendDXGI::GetKeyName(int scancode) {
    static char text[64];
    GetKeyNameTextA(scancode << 16, text, 64);
    return text;
}

bool GfxWindowBackendDXGI::CanDisableVsync() {
    return mTearingSupport;
}

void GfxWindowBackendDXGI::Destroy() {
    // During messy teardowns, it doesn't take a huge tree to eat through the stack.
    // Iteratively clear frame-stat containers to avoid MSVC's recursive _Erase_tree
    // stack overflow on deep std::set/std::map trees during destruction.
    while (!mPendingFrameStats.empty()) {
        mPendingFrameStats.erase(mPendingFrameStats.begin());
    }
    while (!mFrameStats.empty()) {
        mFrameStats.erase(mFrameStats.begin());
    }
}

bool GfxWindowBackendDXGI::IsFullscreen() {
    return mFullScreen;
}

} // namespace Fast

void ThrowIfFailed(HRESULT res) {
    if (FAILED(res)) {
        fprintf(stderr, "Error: 0x%08X\n", res);
        throw Ship::HResultException(res);
    }
}

void ThrowIfFailed(HRESULT res, HWND h_wnd, const char* message) {
    if (FAILED(res)) {
        char full_message[256];
        snprintf(full_message, sizeof(full_message), "%s\n\nHRESULT: 0x%08X", message, res);
        MessageBoxA(h_wnd, full_message, "Error", MB_OK | MB_ICONERROR);
        throw Ship::HResultException(res, message);
    }
}

#endif
