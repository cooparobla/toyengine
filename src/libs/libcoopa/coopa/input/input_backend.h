/**
 * @file input_backend.h
 * @brief The seam a concrete windowing library implements to drive coopa::input::Input.
 *
 * Input owns all keyboard/mouse STATE and is fed by pushes (push_key(),
 * push_cursor_position(), ...) from whatever backend is polling the OS. This
 * interface is the other direction: COMMANDS Input needs to hand off to that
 * same backend (changing the cursor shape, capturing the cursor, clipboard
 * access) — things libcoopa itself has no way to perform, since it links no
 * windowing library. gfxcoopa's presentation::Window implements this against
 * GLFW; a headless Input (no backend attached) simply caches the requested
 * state and never actually moves a cursor, which is what keeps Input unit-
 * testable with no window at all.
 */

#ifndef COOPA_INPUT_INPUT_BACKEND_H
#define COOPA_INPUT_INPUT_BACKEND_H

#include <string>

#include <coopa/input/keys.h>

namespace coopa {
namespace input {

/**
 * @class IInputBackend
 * @brief Cursor/clipboard commands an Input forwards to its owning window.
 */
class IInputBackend {
public:
    virtual ~IInputBackend() = default;

    /// @brief Sets the mouse cursor's shape (e.g. for hover feedback over UI widgets).
    virtual void set_cursor_shape(CursorShape shape) = 0;

    /// @brief Sets the cursor's visibility/capture behavior (see CursorMode).
    virtual void set_cursor_mode(CursorMode mode) = 0;

    /// @brief Warps the OS cursor to (x, y) in window coordinates.
    virtual void set_cursor_position(double x, double y) = 0;

    /// @brief Sets the system clipboard's text contents.
    virtual void set_clipboard_text(const std::string& text) = 0;

    /// @brief Returns the system clipboard's current text contents.
    virtual std::string clipboard_text() const = 0;
};

} // namespace input
} // namespace coopa

#endif // COOPA_INPUT_INPUT_BACKEND_H
