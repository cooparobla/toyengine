#include "editor/app/editor_context.h"

#include "editor/app/editor_app.h"

namespace toy {
namespace editor {

core::Engine& EditorContext::engine() { return app_.engine(); }

const std::deque<std::pair<int, std::string>>& EditorContext::log() const { return app_.log(); }

void EditorContext::clear_log() { app_.clear_log(); }

} // namespace editor
} // namespace toy
