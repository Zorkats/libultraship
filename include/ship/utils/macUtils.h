#pragma once

#include <SDL.h>
#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Toggles the native macOS fullscreen mode for an SDL window.
 * @param window Pointer to the SDL_Window to toggle.
 */
void toggleNativeMacOSFullscreen(SDL_Window* window);

/**
 * @brief Checks whether a native macOS fullscreen transition is active for an SDL window.
 * @param window Pointer to the SDL_Window to query.
 * @return true if native macOS fullscreen is currently active, false otherwise.
 */
bool isNativeMacOSFullscreenActive(SDL_Window* window);

/**
 * @brief Sets a CAMetalLayer's opaque flag. A non-opaque layer keeps macOS from scanning it out
 * directly to the display when it covers the whole screen (full screen / borderless).
 * @param metalLayer The CAMetalLayer (as void*, e.g. from SDL_RenderGetMetalLayer).
 */
void setMetalLayerOpaque(void* metalLayer, bool opaque);

#ifdef __cplusplus
}
#endif
