/**
 * @file window.h
 * @brief GLFW window wrapper configured for Vulkan surface creation, and the
 * GLFW backend for coopa::input::Input.
 *
 * Handles GLFW initialization, window creation, and cleanup. Designed to
 * be constructed first and passed to Instance, Surface, and Renderer.
 *
 * Window owns a coopa::input::Input and feeds it from GLFW's callbacks; it
 * also implements coopa::input::IInputBackend (via the private nested
 * GlfwInputBackend) so Input's cursor/clipboard control calls have somewhere
 * to go. Window itself exposes NO input query methods of its own -- callers
 * reach input() and talk to Input exclusively, so gfxcoopa never needs a
 * second, GLFW-shaped input API alongside libcoopa's. See coopa/input/README.md.
 */

#ifndef COOPA_GFX_PRESENTATION_WINDOW_H
#define COOPA_GFX_PRESENTATION_WINDOW_H

#define GLFW_INCLUDE_NONE // Prevent GLFW from including its own OpenGL/Vulkan headers
#include <GLFW/glfw3.h>
#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>
#include <stdexcept>
#include <vector>

#include <coopa/input/input.h>

#include <gfxcoopa/detail/glfw_keys.h>

#ifdef __APPLE__
#include <gfxcoopa/util/volk_init.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <cstring>
#endif

namespace coopa {
namespace gfx {
namespace presentation {

/**
 * @class Window
 * @brief RAII wrapper around a GLFW window configured for Vulkan rendering.
 *
 * Initializes GLFW, creates a non-resizable (by default) Vulkan-compatible
 * window, and handles cleanup on destruction. Exposes the underlying
 * GLFWwindow* handle for use with the Vulkan surface and ImGui, and owns the
 * coopa::input::Input that every keyboard/mouse query goes through.
 */
class Window {
public:
    /**
     * @brief Initializes GLFW and creates a Vulkan-compatible window.
     *
     * GLFW_CLIENT_API is set to GLFW_NO_API so no implicit OpenGL context
     * is created. The window is non-resizable unless `resizable` is true.
     *
     * A `visible = false` window is not mapped by the window system, so it
     * neither appears on screen nor takes input focus -- but it still owns a
     * real, correctly-sized surface that Vulkan presents to normally, which is
     * what makes it the right shape for an automated render test: the frames
     * are genuine, and nothing interrupts whoever is using the desktop. It
     * stays unmapped until show() is called, which is also how an app maps a
     * window only once it has a frame to show in it (see
     * gfx::app::ContextConfig::show_when_ready).
     *
     * @param title Window title bar string.
     * @param width Window width in pixels.
     * @param height Window height in pixels.
     * @param resizable Allow the user to resize the window.
     * @param visible False creates the window without mapping it -- see above.
     * @throws std::runtime_error if GLFW init or window creation fails.
     */
    Window(const std::string& title, uint32_t width, uint32_t height,
           bool resizable = false, bool visible = true)
        : width_(width), height_(height), visible_(visible)
    {
        // glfwInit() itself is safe to call repeatedly (GLFW documents it as
        // idempotent), but glfwTerminate() is NOT idempotent-safe when
        // multiple Windows coexist -- it tears down ALL GLFW state
        // unconditionally, including any other live Window's handle. The
        // destructor below only calls it once the LAST live Window is
        // destroyed, via this counter, so two Windows can safely coexist in
        // one process (e.g. a test fixture's Window alongside a
        // gfx::app::Context's own -- verified this crashes without the
        // refcount: the second Window's teardown otherwise invalidates the
        // first, which then segfaults on its next GLFW call).
#ifdef __APPLE__
        // macOS: glfwInitVulkanLoader() must run before glfwInit(), so the
        // loader is resolved here rather than first in Instance -- see
        // util::ensure_volk_initialized().
        util::ensure_volk_initialized();
#endif
        if (!glfwInit()) {
            throw std::runtime_error("[gfxcoopa] glfwInit() failed.");
        }
        ++live_window_count_;

        // No OpenGL context — Vulkan provides its own surface.
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
        glfwWindowHint(GLFW_RESIZABLE,  resizable ? GLFW_TRUE : GLFW_FALSE);
        glfwWindowHint(GLFW_VISIBLE,    visible   ? GLFW_TRUE : GLFW_FALSE);

        window_ = glfwCreateWindow(
            static_cast<int>(width_),
            static_cast<int>(height_),
            title.c_str(), nullptr, nullptr);

        if (!window_) {
            if (--live_window_count_ == 0) glfwTerminate();
            throw std::runtime_error("[gfxcoopa] glfwCreateWindow() failed.");
        }

        input_backend_.owner = this;
        input_.set_backend(&input_backend_);

        // Store a back-pointer so resize and input callbacks can update our members.
        glfwSetWindowUserPointer(window_, this);
        glfwSetFramebufferSizeCallback(window_, framebuffer_resize_callback);
        glfwSetScrollCallback(window_, scroll_callback);
        glfwSetCharCallback(window_, char_callback);
        glfwSetKeyCallback(window_, key_callback);
        glfwSetMouseButtonCallback(window_, mouse_button_callback);
        glfwSetCursorPosCallback(window_, cursor_pos_callback);
        glfwSetCursorEnterCallback(window_, cursor_enter_callback);
        glfwSetWindowFocusCallback(window_, window_focus_callback);

        // The position callback only fires on motion -- seed a baseline now
        // so cursor_position() is correct even before the mouse ever moves.
        // This is the Input's first-ever push_cursor_position(), so it
        // reports a zero delta rather than jumping from (0, 0).
        double init_x = 0.0, init_y = 0.0;
        glfwGetCursorPos(window_, &init_x, &init_y);
#ifdef __APPLE__
        to_framebuffer_coords_(window_, init_x, init_y);
#endif
        input_.push_cursor_position(init_x, init_y);
    }

    /**
     * @brief Destroys the GLFW window and terminates GLFW.
     */
    ~Window() {
        if (cursor_) {
            glfwDestroyCursor(cursor_);
        }
        if (window_) {
            glfwDestroyWindow(window_);
        }
        // Only the last live Window tears GLFW down globally -- see the
        // constructor's comment on live_window_count_.
        if (--live_window_count_ == 0) {
            glfwTerminate();
        }
    }

    /// @brief Non-copyable.
    Window(const Window&) = delete;
    /// @brief Non-copyable.
    Window& operator=(const Window&) = delete;

    // --- Query ---

    /**
     * @brief Returns true if the user has requested the window to close.
     * @return True when GLFW_CLOSE was signalled.
     */
    bool should_close() const { return glfwWindowShouldClose(window_); }

    /**
     * @brief Signals the window to close.
     * @param value Set to true to request the window to close.
     */
    void set_should_close(bool value = true) {
        glfwSetWindowShouldClose(window_, value ? GLFW_TRUE : GLFW_FALSE);
    }

    /**
     * @brief This window's keyboard/mouse state. The sole input entry point --
     * Window itself has no is_key_pressed()/cursor_position()/etc. of its own.
     */
    coopa::input::Input& input() { return input_; }
    /// @brief Const overload of input().
    const coopa::input::Input& input() const { return input_; }

    /**
     * @brief Advances Input to a new frame, clearing last frame's edges/events/deltas.
     *
     * Call once per frame before poll_events(). `dt` should be the PREVIOUS
     * frame's duration (the only one known at this point) -- see
     * coopa::input::Input::begin_frame()'s doc on held-time accounting.
     */
    void new_frame(float dt = 0.0f) { input_.begin_frame(dt); }

    /**
     * @brief Polls pending OS events (keyboard, mouse, resize, close).
     *
     * Must be called once per frame on the main thread.
     */
    void poll_events() { glfwPollEvents(); }

    /**
     * @brief Maps a window created with `visible = false`, and focuses it.
     *
     * Presenting a frame BEFORE this call is what keeps a newly mapped window from showing
     * whatever the window system had behind it: the surface already holds an image when the
     * compositor first samples it.
     */
    void show() {
        glfwShowWindow(window_);
        visible_ = true;
    }

    /** @brief True once the window is mapped (false for a headless run, or before show()). */
    bool visible() const { return visible_; }

    /**
     * @brief Blocks until at least one OS event arrives, then processes it.
     *
     * Unlike poll_events() (returns immediately), this sleeps the calling
     * thread -- useful for waiting out a minimized window (0x0 framebuffer)
     * without a busy-poll loop; see gfx::app::Context's resize handling.
     */
    void wait_events() { glfwWaitEvents(); }

    /**
     * @brief Returns the underlying GLFWwindow pointer.
     *
     * Required for Vulkan surface creation and ImGui integration.
     *
     * @return Raw GLFWwindow*.
     */
    GLFWwindow* handle() const { return window_; }

    /** @brief One size of an application icon: straight-alpha RGBA8, row 0 at the top. */
    struct IconImage {
        int width = 0, height = 0;
        const uint8_t* pixels = nullptr;
    };

    /**
     * @brief Sets the application's icon from one or more sizes of the same image.
     *
     * Windows / Linux (X11): the window's title-bar and taskbar icon -- the system picks the
     * closest size (glfwSetWindowIcon). macOS has no per-window icons, so this sets the
     * running process's Dock / app-switcher icon instead (NSApplication's
     * applicationIconImage) from every size given; an invisible window leaves the Dock alone.
     */
    void set_icon(const std::vector<IconImage>& images) {
        if (images.empty()) return;
#ifdef __APPLE__
        if (!visible_) return;
        set_dock_icon_(images);
#else
        std::vector<GLFWimage> glfw_images;
        for (const auto& im : images) {
            GLFWimage g;
            g.width = im.width;
            g.height = im.height;
            g.pixels = const_cast<unsigned char*>(im.pixels);
            glfw_images.push_back(g);
        }
        glfwSetWindowIcon(window_, static_cast<int>(glfw_images.size()), glfw_images.data());
#endif
    }

    /**
     * @brief Returns the current framebuffer size in pixels.
     *
     * Use this (not the window size) when creating or recreating the swapchain,
     * as they differ on HiDPI/Retina displays.
     *
     * @return Pair of (width, height) in pixels.
     */
    std::pair<uint32_t, uint32_t> framebuffer_size() const {
        int w = 0, h = 0;
        glfwGetFramebufferSize(window_, &w, &h);
        return { static_cast<uint32_t>(w), static_cast<uint32_t>(h) };
    }

    /**
     * @brief Returns the window size in screen coordinates -- the space cursor positions are
     *        reported in. Equal to framebuffer_size() except on HiDPI/Retina displays.
     */
    std::pair<uint32_t, uint32_t> window_size() const {
        int w = 0, h = 0;
        glfwGetWindowSize(window_, &w, &h);
        return { static_cast<uint32_t>(w), static_cast<uint32_t>(h) };
    }

    /**
     * @brief Framebuffer pixels per screen coordinate (2 on a Retina display, else 1): the
     *        factor that turns a cursor position into framebuffer pixels.
     */
    float content_scale() const {
        const auto [fw, fh] = framebuffer_size();
        const auto [ww, wh] = window_size();
        (void)fh; (void)wh;
        return ww > 0 && fw > 0 ? static_cast<float>(fw) / static_cast<float>(ww) : 1.0f;
    }

    /**
     * @brief Returns true if the framebuffer was resized since last checked.
     *
     * Call reset_resized() after handling the resize event (e.g. after
     * recreating the swapchain).
     *
     * @return True if a resize event has been recorded.
     */
    bool was_resized() const { return resized_; }

    /**
     * @brief Clears the resized flag after the caller has handled it.
     */
    void reset_resized() { resized_ = false; }

private:
    /**
     * @brief Implements coopa::input::IInputBackend against GLFW, so Input's
     * cursor/clipboard control calls have somewhere to go. Nested (rather
     * than implemented directly on Window) so the IInputBackend virtuals
     * don't appear on Window's own public API -- callers only ever see
     * Window::input().
     *
     * A nested class has the same access to its enclosing class's private
     * members as any other member does, so this reaches window_/cursor_
     * directly through `owner`.
     */
    struct GlfwInputBackend : coopa::input::IInputBackend {
        Window* owner = nullptr;

        void set_cursor_shape(coopa::input::CursorShape shape) override {
            if (owner->cursor_) {
                glfwDestroyCursor(owner->cursor_);
            }
            owner->cursor_ = glfwCreateStandardCursor(detail::to_glfw(shape));
            glfwSetCursor(owner->window_, owner->cursor_);
        }

        void set_cursor_mode(coopa::input::CursorMode mode) override {
            glfwSetInputMode(owner->window_, GLFW_CURSOR, detail::to_glfw(mode));
        }

        void set_cursor_position(double x, double y) override {
#ifdef __APPLE__
            to_window_coords_(owner->window_, x, y);
#endif
            glfwSetCursorPos(owner->window_, x, y);
        }

        void set_clipboard_text(const std::string& text) override {
            glfwSetClipboardString(owner->window_, text.c_str());
        }

        std::string clipboard_text() const override {
            const char* text = glfwGetClipboardString(owner->window_);
            return text ? std::string(text) : std::string();
        }
    };

    /**
     * @brief GLFW framebuffer resize callback.
     *
     * Sets the resized_ flag so the render loop can detect and recreate
     * the swapchain. Handles the case where the window is minimized
     * (size 0x0) by waiting until a non-zero size is reported.
     *
     * @param window The GLFW window that was resized.
     * @param width New framebuffer width.
     * @param height New framebuffer height.
     */
#ifdef __APPLE__
    /**
     * @brief [NSApp setApplicationIconImage:] built from RGBA pixels, through the Objective-C
     *        runtime so this header stays plain C++ (GLFW has already started NSApplication).
     */
    static void set_dock_icon_(const std::vector<IconImage>& images) {
        using Id = void*;
        auto cls = [](const char* n) { return reinterpret_cast<Id>(objc_getClass(n)); };
        auto sel = [](const char* n) { return sel_registerName(n); };
        auto send = reinterpret_cast<Id (*)(Id, SEL)>(objc_msgSend);
        Id ns_image_cls = cls("NSImage"), rep_cls = cls("NSBitmapImageRep"), app_cls = cls("NSApplication");
        if (!ns_image_cls || !rep_cls || !app_cls) return;
        Id color_space = reinterpret_cast<Id (*)(Id, SEL, const char*)>(objc_msgSend)(
            cls("NSString"), sel("stringWithUTF8String:"), "NSDeviceRGBColorSpace");
        struct Size { double w, h; };
        int largest = 0;
        for (const auto& im : images) largest = std::max(largest, im.width);
        Id image = reinterpret_cast<Id (*)(Id, SEL, Size)>(objc_msgSend)(
            send(ns_image_cls, sel("alloc")), sel("initWithSize:"), Size{double(largest), double(largest)});
        for (const auto& im : images) {
            Id rep = reinterpret_cast<Id (*)(Id, SEL, unsigned char**, long, long, long, long, signed char, signed char, Id, long, long)>(
                objc_msgSend)(send(rep_cls, sel("alloc")),
                              sel("initWithBitmapDataPlanes:pixelsWide:pixelsHigh:bitsPerSample:samplesPerPixel:hasAlpha:isPlanar:"
                                  "colorSpaceName:bytesPerRow:bitsPerPixel:"),
                              nullptr, im.width, im.height, 8, 4, 1, 0, color_space, long(im.width) * 4, 32);
            if (!rep) continue;
            // NSBitmapImageRep wants premultiplied alpha by default.
            auto* dst = reinterpret_cast<unsigned char* (*)(Id, SEL)>(objc_msgSend)(rep, sel("bitmapData"));
            const size_t n = size_t(im.width) * size_t(im.height);
            for (size_t i = 0; i < n; ++i) {
                const unsigned a = im.pixels[i * 4 + 3];
                for (int c = 0; c < 3; ++c) dst[i * 4 + c] = static_cast<unsigned char>((im.pixels[i * 4 + c] * a + 127) / 255);
                dst[i * 4 + 3] = static_cast<unsigned char>(a);
            }
            reinterpret_cast<void (*)(Id, SEL, Id)>(objc_msgSend)(image, sel("addRepresentation:"), rep);
            send(rep, sel("release"));
        }
        Id app = send(app_cls, sel("sharedApplication"));
        reinterpret_cast<void (*)(Id, SEL, Id)>(objc_msgSend)(app, sel("setApplicationIconImage:"), image);
        send(image, sel("release"));
    }
#endif

    static void framebuffer_resize_callback(GLFWwindow* window, int width, int height) {
        (void)width; (void)height;
        auto* self = reinterpret_cast<Window*>(glfwGetWindowUserPointer(window));
        if (self) {
            self->resized_ = true;
        }
    }

    /// @brief GLFW scroll callback. Forwarded straight into Input.
    static void scroll_callback(GLFWwindow* window, double xoffset, double yoffset) {
        auto* self = reinterpret_cast<Window*>(glfwGetWindowUserPointer(window));
        if (self) {
            self->input_.push_scroll(xoffset, yoffset);
        }
    }

    /// @brief GLFW char callback. Forwarded straight into Input.
    static void char_callback(GLFWwindow* window, unsigned int codepoint) {
        auto* self = reinterpret_cast<Window*>(glfwGetWindowUserPointer(window));
        if (self) {
            self->input_.push_char(codepoint);
        }
    }

    /// @brief GLFW key callback. Converted via detail::glfw_keys.h, then forwarded into Input.
    static void key_callback(GLFWwindow* window, int key, int scancode, int action, int mods) {
        auto* self = reinterpret_cast<Window*>(glfwGetWindowUserPointer(window));
        if (self) {
            self->input_.push_key(detail::from_glfw(key), scancode,
                                   detail::action_from_glfw(action), detail::mods_from_glfw(mods));
        }
    }

    /// @brief GLFW mouse button callback. Converted via detail::glfw_keys.h, then forwarded into Input.
    static void mouse_button_callback(GLFWwindow* window, int button, int action, int mods) {
        auto* self = reinterpret_cast<Window*>(glfwGetWindowUserPointer(window));
        if (self) {
            self->input_.push_mouse_button(detail::mouse_from_glfw(button),
                                            detail::action_from_glfw(action), detail::mods_from_glfw(mods));
        }
    }

    /// @brief GLFW cursor position callback. Forwarded straight into Input.
    static void cursor_pos_callback(GLFWwindow* window, double xpos, double ypos) {
        auto* self = reinterpret_cast<Window*>(glfwGetWindowUserPointer(window));
        if (self) {
#ifdef __APPLE__
            to_framebuffer_coords_(window, xpos, ypos);
#endif
            self->input_.push_cursor_position(xpos, ypos);
        }
    }

#ifdef __APPLE__
    // Retina: GLFW reports cursor positions in window points, but the swapchain
    // (and therefore every consumer's letterbox / UI / picking maths) is sized in
    // framebuffer pixels. Convert at the boundary so Input speaks pixels, the
    // same unit it already speaks on a 1:1 Linux display.
    static void cursor_scale_(GLFWwindow* window, double& sx, double& sy) {
        int ww = 0, wh = 0, fw = 0, fh = 0;
        glfwGetWindowSize(window, &ww, &wh);
        glfwGetFramebufferSize(window, &fw, &fh);
        sx = (ww > 0 && fw > 0) ? static_cast<double>(fw) / ww : 1.0;
        sy = (wh > 0 && fh > 0) ? static_cast<double>(fh) / wh : 1.0;
    }
    static void to_framebuffer_coords_(GLFWwindow* window, double& x, double& y) {
        double sx, sy;
        cursor_scale_(window, sx, sy);
        x *= sx;
        y *= sy;
    }
    static void to_window_coords_(GLFWwindow* window, double& x, double& y) {
        double sx, sy;
        cursor_scale_(window, sx, sy);
        x /= sx;
        y /= sy;
    }
#endif

    /// @brief GLFW cursor-enter/leave callback. Forwarded straight into Input.
    static void cursor_enter_callback(GLFWwindow* window, int entered) {
        auto* self = reinterpret_cast<Window*>(glfwGetWindowUserPointer(window));
        if (self) {
            self->input_.push_cursor_enter(entered != 0);
        }
    }

    /// @brief GLFW window-focus callback. Forwarded straight into Input, which
    /// releases every held key/button on focus loss -- see Input::push_focus().
    static void window_focus_callback(GLFWwindow* window, int focused) {
        auto* self = reinterpret_cast<Window*>(glfwGetWindowUserPointer(window));
        if (self) {
            self->input_.push_focus(focused != 0);
        }
    }

    GLFWwindow*  window_  = nullptr; /**< The underlying GLFW window handle. */
    GLFWcursor*  cursor_  = nullptr; /**< Current standard cursor, if Input::set_cursor_shape() was called. */
    uint32_t     width_;             /**< Initial window width in pixels. */
    bool         visible_ = true;    /**< Mapped on screen (false for headless runs). */
    uint32_t     height_;            /**< Initial window height in pixels. */
    bool         resized_ = false;   /**< Set to true when a framebuffer resize event arrives. */

    coopa::input::Input   input_;         /**< All keyboard/mouse state; see input(). */
    GlfwInputBackend       input_backend_; /**< Implements IInputBackend against GLFW; attached to input_ in the constructor. */

    /// @brief Process-wide count of live Window instances, so only the
    /// last one destroyed calls glfwTerminate(). Not atomic: GLFW (and
    /// therefore Window) is assumed single-threaded, matching every other
    /// assumption this class already makes (its callbacks, poll_events(),
    /// etc. are all main-thread-only per GLFW's own threading rules).
    static inline int live_window_count_ = 0;
};

} // namespace presentation
} // namespace gfx
} // namespace coopa

#endif // COOPA_GFX_PRESENTATION_WINDOW_H
