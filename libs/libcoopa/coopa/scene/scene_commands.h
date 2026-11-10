/**
 * @file scene_commands.h
 * @brief Deferred structural-mutation buffer for jobified scene systems.
 *
 * A Scene is single-owner (see Scene's class doc): only its owning thread may
 * call a non-const method on it directly. A system fanned out across worker
 * jobs (e.g. a parallel_for over components) must not call
 * add_root_object()/attach_component()/etc. from a worker -- instead it
 * records the intent into its worker's own SceneCommandBuffer (via
 * FrameContext::commands, indexed by FrameContext::worker_index), and the
 * owning thread applies every buffer's commands serially, in worker-index
 * order, via Scene::flush_commands() at a sync point (after every system for
 * the frame has finished, so a job never observes another job's still-
 * pending structural change).
 */

#ifndef COOPA_SCENE_SCENE_COMMANDS_H
#define COOPA_SCENE_SCENE_COMMANDS_H

#include <coopa/event/event_bus.h>
#include <coopa/scene/component.h>
#include <coopa/scene/scene_object.h>

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace coopa {
namespace scene {

class Scene;

namespace detail {

struct AddRootObjectOp   { std::unique_ptr<SceneObject> obj; };
struct AddChildOp        { SceneObject* parent; std::unique_ptr<SceneObject> child; };
struct DestroyObjectOp   { SceneObject* obj; };
struct AttachComponentOp { SceneObject* obj; std::unique_ptr<Component> comp; };
struct RemoveComponentOp { Component* comp; };
struct SetActiveOp       { SceneObject* obj; bool active; };
struct EmitOp            { std::string object; std::string signal; coopa::event::EventArgs args; };
struct CustomOp          { std::function<void(Scene&)> fn; };

using SceneCommandOp = std::variant<
    AddRootObjectOp, AddChildOp, DestroyObjectOp,
    AttachComponentOp, RemoveComponentOp, SetActiveOp, EmitOp, CustomOp>;

} // namespace detail

/**
 * @class SceneCommandBuffer
 * @brief One worker's queue of deferred structural edits against one Scene.
 *
 * Every method here is safe to call from any single thread concurrently with
 * other threads calling the same methods on their OWN SceneCommandBuffer
 * instances -- this class itself holds no shared state and needs no
 * synchronization; the safety comes from each worker owning a distinct
 * buffer (see Scene::commands_for()) and only the owning thread ever
 * flushing them.
 */
class SceneCommandBuffer {
public:
    /// @brief Adds `obj` as a new scene root once flushed.
    void add_root_object(std::unique_ptr<SceneObject> obj) {
        ops_.emplace_back(detail::AddRootObjectOp{std::move(obj)});
    }

    /// @brief Adds `child` under `parent` once flushed.
    void add_child(SceneObject* parent, std::unique_ptr<SceneObject> child) {
        ops_.emplace_back(detail::AddChildOp{parent, std::move(child)});
    }

    /// @brief Detaches and destroys `obj` (root or non-root) once flushed.
    void destroy_object(SceneObject* obj) {
        ops_.emplace_back(detail::DestroyObjectOp{obj});
    }

    /// @brief Attaches `comp` onto `obj` once flushed.
    void attach_component(SceneObject* obj, std::unique_ptr<Component> comp) {
        ops_.emplace_back(detail::AttachComponentOp{obj, std::move(comp)});
    }

    /// @brief Removes and destroys `comp` from its owner once flushed.
    void remove_component(Component* comp) {
        ops_.emplace_back(detail::RemoveComponentOp{comp});
    }

    /// @brief Sets `obj`'s active flag once flushed.
    void set_active(SceneObject* obj, bool active) {
        ops_.emplace_back(detail::SetActiveOp{obj, active});
    }

    /// @brief Emits a named EventBus signal once flushed (EventBus is not thread-safe -- see its class doc).
    void emit(std::string object, std::string signal, coopa::event::EventArgs args = {}) {
        ops_.emplace_back(detail::EmitOp{std::move(object), std::move(signal), std::move(args)});
    }

    /// @brief Escape hatch: runs an arbitrary callback against the Scene once flushed.
    void enqueue(std::function<void(Scene&)> fn) {
        ops_.emplace_back(detail::CustomOp{std::move(fn)});
    }

    bool empty() const { return ops_.empty(); }
    void clear() { ops_.clear(); }

    /// @brief Internal use (Scene::flush_commands()): applies and clears every recorded op, in order.
    void flush_(Scene& scene);

private:
    std::vector<detail::SceneCommandOp> ops_;
};

} // namespace scene
} // namespace coopa

#endif // COOPA_SCENE_SCENE_COMMANDS_H
