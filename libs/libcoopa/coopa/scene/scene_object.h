/**
 * @file scene_object.h
 * @brief Named scene node with a component list and child hierarchy.
 *
 * SceneObject is the "GameObject" of the scene graph. It carries no
 * assumptions about which component types exist — those are supplied by
 * callers (e.g. SceneLoader adds a TransformComponent to every parsed
 * object, but a programmatically-built SceneObject starts with none).
 * Components are owned via unique_ptr; children are also owned.
 */

#ifndef COOPA_SCENE_SCENE_OBJECT_H
#define COOPA_SCENE_SCENE_OBJECT_H

#include <coopa/scene/component.h>
#include <coopa/scene/components/transform_component.h>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>
#include <string>
#include <string_view>
#include <functional>
#include <typeindex>
#include <stdexcept>

namespace coopa {
namespace scene {

/**
 * @class SceneObject
 * @brief A named node in the scene hierarchy, owning components and child objects.
 *
 * Usage:
 * @code
 * auto obj = std::make_unique<SceneObject>("Cube");
 * auto* tc = obj->add_component<TransformComponent>();
 * obj->add_child(std::make_unique<SceneObject>("Child"));
 * @endcode
 */
class SceneObject {
public:
    /**
     * @brief Constructs a SceneObject with the given name.
     *
     * Carries no components until add_component()/attach_component() is
     * called — SceneLoader adds a TransformComponent to every object it
     * parses, but that is a loader convention, not a guarantee of this class.
     *
     * @param name The object's name.
     * @param active Whether the object is active on creation.
     */
    explicit SceneObject(std::string name, bool active = true)
        : name_(std::move(name)), active_(active), parent_(nullptr)
    {}

    // --- Identity ---

    /** @brief Returns the object name. */
    const std::string& name() const { return name_; }

    /** @brief Returns whether this object is active. */
    bool active() const { return active_; }

    /** @brief Sets the active state. */
    void set_active(bool active) {
        if (active_ == active) return;
        active_ = active;
        notify_structure_changed_();
    }

    /** @brief Returns the parent SceneObject (non-owning, may be nullptr). */
    SceneObject* parent() const { return parent_; }

    // --- Components ---

    /**
     * @brief Constructs and attaches a component of type T.
     *
     * Sets the component's owner pointer to this object.
     *
     * @tparam T Component subclass to create.
     * @tparam Args Constructor argument types.
     * @param args Arguments forwarded to T's constructor.
     * @return Raw pointer to the newly added component.
     */
    template<typename T, typename... Args>
    T* add_component(Args&&... args) {
        auto comp = std::make_unique<T>(std::forward<Args>(args)...);
        comp->owner = this;
        T* raw = comp.get();
        components_.push_back(std::move(comp));
        notify_structure_changed_();
        return raw;
    }

    /**
     * @brief Returns the first component of type T, or nullptr if not found.
     * @tparam T Component type to search for.
     * @return Pointer to the component, or nullptr.
     */
    template<typename T>
    T* get_component() const {
        for (const auto& comp : components_) {
            if (T* ptr = dynamic_cast<T*>(comp.get())) {
                return ptr;
            }
        }
        return nullptr;
    }

    /**
     * @brief Returns every component of type T found on this object or any descendant.
     * @tparam T Component type to search for.
     * @return Pointers to all matching components, in pre-order.
     */
    template<typename T>
    std::vector<T*> get_components_in_children() const {
        std::vector<T*> result;
        for_each_recursive([&result](const SceneObject& obj) {
            if (T* ptr = obj.get_component<T>()) {
                result.push_back(ptr);
            }
        });
        return result;
    }

    /**
     * @brief Removes and destroys the given component if owned by this object.
     * @param comp Pointer to the component to remove.
     * @return True if a matching component was found and removed.
     */
    bool remove_component(Component* comp) {
        auto it = std::find_if(components_.begin(), components_.end(),
            [comp](const std::unique_ptr<Component>& c) { return c.get() == comp; });
        if (it == components_.end()) return false;
        (*it)->owner = nullptr;
        components_.erase(it);
        notify_structure_changed_();
        return true;
    }

    /**
     * @brief Removes and destroys the first component of type T.
     * @tparam T Component type to remove.
     * @return True if a matching component was found and removed.
     */
    template<typename T>
    bool remove_component() {
        if (T* ptr = get_component<T>()) {
            return remove_component(ptr);
        }
        return false;
    }

    /**
     * @brief Returns the TransformComponent (convenience alias for get_component<TransformComponent>).
     *
     * Avoids template syntax in generic pipeline code. TransformComponent is
     * the one component type the scene system itself defines, so this
     * accessor is safe to keep here without pulling in any external module.
     *
     * @return Pointer to the TransformComponent, or nullptr if missing.
     */
    TransformComponent* get_transform() const {
        return get_component<TransformComponent>();
    }

    /**
     * @brief Directly attaches a pre-constructed component (takes ownership).
     *
     * Sets the component's owner pointer to this object.
     *
     * @param comp Owning unique_ptr to the component.
     */
    void attach_component(std::unique_ptr<Component> comp) {
        comp->owner = this;
        components_.push_back(std::move(comp));
        notify_structure_changed_();
    }

    /** @brief Returns all components (read-only). */
    const std::vector<std::unique_ptr<Component>>& components() const { return components_; }
    /** @brief Returns all components (mutable). */
    std::vector<std::unique_ptr<Component>>& components() { return components_; }

    /**
     * @brief Returns the index-th component whose type_name() matches, or nullptr.
     *
     * The string-keyed counterpart to get_component<T>(), for callers that
     * only know a type by the name it was registered under (SceneLoader's
     * component tags, an AnimationClip track's `component:` field) and
     * cannot name the C++ type at the call site.
     *
     * @param type_name Value that would be returned by the target's type_name().
     * @param index Which match to return, in component list order, if more
     *              than one component on this object shares the same
     *              type_name() (e.g. two Image components).
     * @return Pointer to the component, or nullptr if not found.
     */
    Component* get_component_by_type_name(const std::string& type_name, size_t index = 0) const {
        size_t seen = 0;
        for (const auto& comp : components_) {
            if (comp->type_name() == type_name) {
                if (seen == index) return comp.get();
                ++seen;
            }
        }
        return nullptr;
    }

    // --- Hierarchy ---

    /**
     * @brief Adds a child SceneObject (takes ownership).
     *
     * Only the SceneObject ownership link is established here — a child's
     * TransformComponent (if any) is NOT auto-linked to this object's
     * Transform, because SceneLoader::parse_object_ already performs that
     * linking explicitly (via TransformComponent::set_parent_transform)
     * before calling add_child, and doing it again here would register the
     * child's Transform twice in the parent's dirty-propagation list.
     * Callers building a hierarchy by hand must link transforms themselves;
     * set_parent() below does this automatically for reparenting.
     *
     * @param child The child to add. Its parent pointer is set to this.
     * @return Raw pointer to the child.
     */
    SceneObject* add_child(std::unique_ptr<SceneObject> child) {
        child->parent_ = this;
        SceneObject* raw = child.get();
        children_.push_back(std::move(child));
        // The added subtree joins this object's Scene (if any): it must share the Scene's
        // structure counter so its own future edits invalidate the component caches too.
        if (structure_hook_ != nullptr) raw->set_structure_hook(structure_hook_);
        notify_structure_changed_();
        return raw;
    }

    /**
     * @brief Detaches a child from this object without destroying it.
     *
     * The returned unique_ptr owns the detached subtree; the child's parent
     * pointer is cleared, and its TransformComponent (if any) is unlinked
     * from this object's Transform child-list so no dangling pointer is left
     * behind for dirty-flag propagation.
     *
     * @param child Pointer to the direct child to detach.
     * @return Owning pointer to the detached child, or nullptr if not found.
     */
    std::unique_ptr<SceneObject> detach_child(SceneObject* child) {
        auto it = std::find_if(children_.begin(), children_.end(),
            [child](const std::unique_ptr<SceneObject>& c) { return c.get() == child; });
        if (it == children_.end()) return nullptr;
        std::unique_ptr<SceneObject> detached = std::move(*it);
        if (auto* child_tc = detached->get_transform()) {
            if (auto* self_tc = get_transform()) {
                self_tc->transform().remove_child(&child_tc->transform());
            }
            child_tc->transform().set_parent(nullptr);
        }
        children_.erase(it);
        detached->parent_ = nullptr;
        notify_structure_changed_();
        return detached;
    }

    /**
     * @brief Reparents this object under new_parent, preserving the subtree.
     *
     * Detaches this object from its current parent (if any) and adds it to
     * new_parent's children. No-op if new_parent is already the current
     * parent. If this object carries a TransformComponent, its Transform is
     * also re-linked under new_parent's TransformComponent (or unlinked to
     * root if new_parent has none) — detach_child() already unlinks it from
     * the old Transform parent, so this only needs to (re)link the new side.
     *
     * @param new_parent The SceneObject to become the new parent. Must not be null.
     * @throws std::runtime_error if this object has no current SceneObject parent
     *         (e.g. it is a Scene root object) — ownership cannot be transferred
     *         without access to whatever container currently owns it.
     */
    void set_parent(SceneObject* new_parent) {
        if (new_parent == parent_ || new_parent == this) return;
        if (!parent_) {
            throw std::runtime_error(
                "SceneObject::set_parent: object has no SceneObject parent to detach from "
                "(likely a Scene root object); reparent it via Scene::root_objects() instead.");
        }
        std::unique_ptr<SceneObject> self_owned = parent_->detach_child(this);
        if (auto* self_tc = self_owned->get_transform()) {
            auto* new_parent_tc = new_parent->get_transform();
            self_tc->set_parent_transform(new_parent_tc ? &new_parent_tc->transform() : nullptr);
        }
        new_parent->add_child(std::move(self_owned));
    }

    /** @brief Returns the list of children (read-only). */
    const std::vector<std::unique_ptr<SceneObject>>& children() const { return children_; }
    /** @brief Returns the list of children (mutable). */
    std::vector<std::unique_ptr<SceneObject>>& children() { return children_; }

    // --- Structure change tracking ---

    /**
     * @brief Points this object (and every descendant) at a scene-structure change counter.
     *
     * The owning Scene stamps this alongside Component::scene so that every structural edit --
     * a component added or removed, a child attached or detached, an active flag flipped --
     * bumps the Scene's counter and invalidates its cached component queries
     * (Scene::get_components() et al.). add_child() propagates the parent's hook onto the added
     * subtree, so an object spawned at runtime is covered from the moment it joins the tree.
     * Null (the default, and for free-standing subtrees) makes the notifications no-ops.
     *
     * @param hook The counter to bump on structural changes, or nullptr to detach.
     */
    void set_structure_hook(std::atomic<std::uint64_t>* hook) {
        for_each_recursive([hook](SceneObject& o) { o.structure_hook_ = hook; });
    }

    /**
     * @brief Depth-first traversal of this object and all descendants.
     * @param fn Callable receiving a mutable reference to each SceneObject.
     */
    template<typename Fn>
    void for_each_recursive(Fn&& fn) {
        fn(*this);
        for (auto& child : children_) {
            child->for_each_recursive(std::forward<Fn>(fn));
        }
    }

    /**
     * @brief Depth-first traversal (const version).
     */
    template<typename Fn>
    void for_each_recursive(Fn&& fn) const {
        fn(*this);
        for (const auto& child : children_) {
            child->for_each_recursive(std::forward<Fn>(fn));
        }
    }

    /**
     * @brief Depth-first search for a descendant with the given name.
     *
     * Excludes this object itself — only children and further descendants
     * are considered. Useful for resolving cross-references stored as names
     * in scene YAML (e.g. ScrollRect::content) once the whole tree exists.
     *
     * @param name Name to search for.
     * @return Non-owning pointer to the first matching descendant, or nullptr.
     */
    SceneObject* find_descendant(const std::string& name) const {
        for (const auto& child : children_) {
            if (child->name() == name) return child.get();
            if (SceneObject* found = child->find_descendant(name)) return found;
        }
        return nullptr;
    }

    /**
     * @brief Finds a DIRECT child with the given name (non-recursive).
     *
     * Unlike find_descendant(), does not search grandchildren — the building
     * block for path-style lookups (e.g. Scene::find_object_by_path()) where
     * each segment after the first must name an immediate child of the last
     * match, not just some descendant of it.
     *
     * @param name Name to search for among direct children.
     * @return Non-owning pointer to the matching child, or nullptr.
     */
    SceneObject* find_child(std::string_view name) const {
        for (const auto& child : children_) {
            if (child->name() == name) return child.get();
        }
        return nullptr;
    }

    // --- Lifecycle ---

    /**
     * @brief Calls start() on all components, then recurses to children.
     *
     * Iterates by index rather than range-based for: a component's start()
     * is allowed to add a further component to this same owner (e.g. a
     * widget lazily attaching a Mask the first time it's started), which
     * reallocates components_ -- a range-based for's cached begin()/end()
     * would then dereference a dangling iterator into the freed buffer.
     * Indexing re-reads size() each iteration, so it's immune to that, and
     * a component added this way correctly gets its own start() called too
     * rather than silently skipped.
     */
    void start() {
        if (!active_) return;
        for (size_t i = 0; i < components_.size(); ++i) components_[i]->start();
        for (auto& child : children_) child->start();
    }

    /**
     * @brief Calls update(dt) on all components, then recurses to active children.
     * @param delta_time Frame delta time in seconds.
     */
    void update(float delta_time) {
        if (!active_) return;
        for (auto& comp : components_) comp->update(delta_time);
        for (auto& child : children_) child->update(delta_time);
    }

    /**
     * @brief Calls late_update(dt) on all components, then recurses to active children.
     *
     * Mirrors update()'s exact shape (pre-order, self before children, prunes
     * inactive subtrees) — see Component::late_update() for why a second pass
     * exists at all.
     *
     * @param delta_time Frame delta time in seconds.
     */
    void late_update(float delta_time) {
        if (!active_) return;
        for (auto& comp : components_) comp->late_update(delta_time);
        for (auto& child : children_) child->late_update(delta_time);
    }

private:
    /** @brief Bumps the owning Scene's structure counter -- see set_structure_hook(). */
    void notify_structure_changed_() {
        if (structure_hook_ != nullptr) structure_hook_->fetch_add(1, std::memory_order_relaxed);
    }

    std::string                              name_;       /**< Object name. */
    bool                                     active_;     /**< Whether updates/rendering are active. */
    SceneObject*                             parent_;     /**< Non-owning parent pointer. */
    std::vector<std::unique_ptr<Component>>  components_; /**< Owned components. */
    std::vector<std::unique_ptr<SceneObject>>children_;  /**< Owned children. */
    /** @brief The owning Scene's structure change counter, or null -- see set_structure_hook(). */
    std::atomic<std::uint64_t>*              structure_hook_ = nullptr;
};

} // namespace scene
} // namespace coopa

#endif // COOPA_SCENE_SCENE_OBJECT_H
