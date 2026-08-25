#pragma once
#include <SDL2/SDL.h>
#include "ship/window/gui/Gui.h"
#include "fast/WindowEvent.h"
#include "fast/resource/type/Texture.h"
#include "ship/window/gui/resource/GuiTexture.h"

// Fixes issue #926: HandleWindowEvents is only ever called from Fast3D backend code
// (gfx_sdl2.cpp, gfx_dxgi.cpp) and must not be a virtual method on Ship::Gui.
// The WindowEvent type has been moved to the Fast namespace so that ship code does
// not depend on any Fast3D or platform-specific types.

namespace Fast {
class Interpreter;

/**
 * @brief Backend-specific data required to initialise the ImGui rendering context.
 *
 * The active union member is selected based on the graphics backend:
 * - Dx11   for DirectX 11 (Windows).
 * - Opengl for OpenGL (Linux/macOS via SDL).
 * - Metal  for Metal (macOS via SDL).
 * - Gx2    for the GX2 API (Wii U / Café OS).
 */
typedef struct {
    union {
        struct {
            void* Window;        ///< HWND
            void* DeviceContext; ///< ID3D11DeviceContext*
            void* Device;        ///< ID3D11Device*
        } Dx11;
        struct {
            void* Window;  ///< SDL_Window*
            void* Context; ///< SDL_GLContext
        } Opengl;
        struct {
            void* Window;           ///< SDL_Window*
            SDL_Renderer* Renderer; ///< SDL_Renderer* (for Metal layer)
        } Metal;
        struct {
            uint32_t Width;  ///< Framebuffer width in pixels.
            uint32_t Height; ///< Framebuffer height in pixels.
        } Gx2;
    };
} GuiWindowInitData;

/**
 * @brief Concrete Gui subclass for the Fast3D rendering backend.
 *
 * Overrides the virtual ImGui backend methods defined by Ship::Gui with
 * implementations that dispatch to the appropriate platform/renderer backend
 * (SDL+OpenGL, SDL+Metal, or DXGI+DX11) based on the active WindowBackend.
 *
 * Also owns the Fast3D-specific rendering resources: texture cache and the
 * Interpreter weak reference used for viewport and resolution calculations.
 */
class Fast3dGui : public Ship::Gui {
  public:
    Fast3dGui();
    Fast3dGui(std::vector<std::shared_ptr<Ship::GuiWindow>> guiWindows);
    ~Fast3dGui() override = default;

    bool SupportsViewports() override;

    /**
     * @brief Initialises the ImGui context with the given backend-specific handles.
     * @param windowImpl Backend-specific handles required by the ImGui backend.
     */
    void Init(GuiWindowInitData windowImpl);

    /**
     * @brief Forwards a platform window event to the active ImGui backend.
     *
     * Only Fast3D backends (gfx_sdl2, gfx_dxgi) construct and dispatch WindowEvents.
     * Callers retrieve the Gui via Window::GetGui() and dynamic_cast to Fast3dGui.
     * @param event Platform event (SDL or Win32) wrapped in a Fast::WindowEvent.
     */
    void HandleWindowEvents(Fast::WindowEvent event);

    /**
     * @brief Loads an image from an archive path and caches it under the given name.
     * @param name Path/texture name used to reference the texture in GetTextureByName().
     * @param path Virtual resource path of the source image.
     * @param tint RGBA tint multiplied over the image (use ImVec4(1,1,1,1) for no tint).
     */
    void LoadGuiTexture(const std::string& name, const std::string& path, const ImVec4& tint);

    /**
     * @brief Returns true if a texture with the given name is already cached.
     * @param name Texture cache key.
     */
    bool HasTextureByName(const std::string& name);

    /**
     * @brief Uploads a Fast::Texture object to the GPU and caches it under @p name.
     * @param name Texture cache key.
     * @param tex  Source texture data.
     * @param tint RGBA tint.
     */
    void LoadGuiTexture(const std::string& name, const Fast::Texture& tex, const ImVec4& tint);

    /**
     * @brief Removes the texture with the given name from the cache and frees GPU resources.
     * @param name Texture cache key to remove.
     */
    void UnloadTexture(const std::string& name);

    /**
     * @brief Returns the ImGui texture handle for the given cache key.
     * @param name Texture cache key.
     * @return ImTextureID suitable for ImGui::Image(), or nullptr if not found.
     */
    ImTextureID GetTextureByName(const std::string& name);

    /**
     * @brief Returns the pixel dimensions of the cached texture.
     * @param name Texture cache key.
     * @return ImVec2 with the texture's width and height, or (0, 0) if not found.
     */
    ImVec2 GetTextureSize(const std::string& name);

    /**
     * @brief Loads a raw image file from the filesystem (not the archive) and caches it.
     * @param name Cache key.
     * @param path Absolute filesystem path to the image file (e.g. a PNG).
     */
    void LoadTextureFromRawImage(const std::string& name, const std::string& path);

    /**
     * @brief Uploads a pre-loaded GuiTexture resource to the GPU and caches it.
     * @param name    Cache key.
     * @param texture Loaded GuiTexture resource.
     */
    void LoadTextureFromResource(const std::string& name, std::shared_ptr<Ship::GuiTexture> texture);

    void RefreshImGuiGamepads() override;

    /**
     * @brief Returns the client-space rect the game framebuffer was blitted into by the last
     * DrawGame() call.
     *
     * Host-side mouse code (G-Diffuser's Course Edit absolute drive) needs to invert the blit to
     * map a window mouse position back to 320x240 game space; storing the rect here beats
     * re-deriving the pillarbox/letterbox branch above outside the class.
     *
     * @param x,y,w,h Out parameters in window client coordinates.
     * @return false until the first successful blit (no framebuffer yet).
     */
    bool GetGameBlitRect(float* x, float* y, float* w, float* h);

  protected:
    void ImGuiWMInit() override;
    void ImGuiWMShutdown() override;
    void ImGuiBackendInit() override;
    void ImGuiBackendShutdown() override;
    void ImGuiBackendNewFrame() override;
    void ImGuiWMNewFrame() override;
    void ImGuiRenderDrawData(ImDrawData* data) override;
    void DrawFloatingWindows() override;
    void CalculateGameViewport() override;
    void DrawGame() override;

    /**
     * @brief Returns the ImTextureID for a texture identified by its integer ID.
     * @param id Internal texture registry ID.
     * @return ImTextureID for use with ImGui::Image().
     */
    ImTextureID GetTextureById(int32_t id);

    std::weak_ptr<Interpreter> mInterpreter; ///< Weak reference to the Fast3D scripting interpreter.
    GuiWindowInitData mImpl;                 ///< Backend-specific window/context handles passed to Init().
    /// WindowBackend captured at Init, while the Context singleton is still alive. The ImGui
    /// shutdown methods run from ~Context and must switch on this rather than re-fetch the
    /// singleton, which is already empty by then. -1 = Init never ran.
    int32_t mCachedBackend = -1;

  private:
    /** @brief Applies any pending resolution or MSAA changes to the render target. */
    void ApplyResolutionChanges();

    /**
     * @brief Returns the integer scaling factor applied to the game viewport.
     * @return Scaling multiplier (1 = native, 2 = 2×, etc.).
     */
    int16_t GetIntegerScaleFactor();

    std::unordered_map<std::string, Ship::GuiTextureMetadata> mGuiTextures; ///< Cached GPU texture registry.

    ImVec2 mGameBlitPos{ 0, 0 };  ///< Client-space origin of the last game-framebuffer blit.
    ImVec2 mGameBlitSize{ 0, 0 }; ///< Client-space size of the last game-framebuffer blit.
    bool mGameBlitValid = false;  ///< False until DrawGame() has blitted a framebuffer once.
};
} // namespace Fast
