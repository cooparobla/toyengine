/**
 * @file animated_property.h
 * @brief Maps a (Component::type_name(), property name) pair to a typed getter/setter.
 *
 * This is how an Animator can drive fields on component types libcoopa never
 * names — RectTransform, Graphic::color, a future material property — without
 * a virtual hook on coopa::scene::Component. It mirrors the seam
 * SceneLoader::register_component_parser() already establishes: a
 * function-local-static registry, keyed by a string, populated from one
 * entry point per repo (see uicoopa's register_ui_animated_properties()).
 */

#ifndef COOPA_ANIMATION_ANIMATED_PROPERTY_H
#define COOPA_ANIMATION_ANIMATED_PROPERTY_H

#include <coopa/scene/component.h>
#include <coopa/scene/components/transform_component.h>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace coopa {
namespace anim {

/**
 * @brief Splits a trailing channel suffix off a property name.
 *
 * Accepts `.x/.y/.z/.w` and `.r/.g/.b/.a`, single or combined (e.g. ".xy",
 * ".rgb") — x/r share bit 0, y/g bit 1, z/b bit 2, w/a bit 3, so mixing the
 * two spellings in one suffix (unusual, but not rejected) just ORs together.
 * A name with no recognized suffix, or no '.' at all, returns the full mask
 * (0x0F) unchanged — callers intersect that with the target property's own
 * component_count-derived mask, since "no suffix" means "every channel this
 * property has", not literally 4.
 *
 * @param in Property name as written in YAML, e.g. "anchored_position.y".
 * @param out_base Receives the name with any suffix removed.
 * @param out_mask Receives the decoded channel bitmask.
 * @return True if a channel suffix was recognized and stripped.
 */
inline bool split_channel_suffix(const std::string& in, std::string& out_base, uint8_t& out_mask) {
    auto dot = in.rfind('.');
    if (dot == std::string::npos) {
        out_base = in;
        out_mask = 0x0F;
        return false;
    }
    std::string suffix = in.substr(dot + 1);
    uint8_t mask = 0;
    bool valid = !suffix.empty();
    for (char ch : suffix) {
        switch (ch) {
            case 'x': case 'r': mask |= 0x1; break;
            case 'y': case 'g': mask |= 0x2; break;
            case 'z': case 'b': mask |= 0x4; break;
            case 'w': case 'a': mask |= 0x8; break;
            default: valid = false; break;
        }
        if (!valid) break;
    }
    if (!valid) {
        out_base = in;
        out_mask = 0x0F;
        return false;
    }
    out_base = in.substr(0, dot);
    out_mask = mask;
    return true;
}

/**
 * @struct AnimatedProperty
 * @brief One registered, typed binding target.
 *
 * `cast` is resolved ONCE per binding, at an Animator's bind time, and its
 * result cached — see animator.h's Binding struct. `get`/`set` then operate
 * on that cached `void*` every frame with no further RTTI.
 */
struct AnimatedProperty {
    std::string component_type;      ///< Matches Component::type_name() exactly.
    std::string name;                ///< Bare name, no channel suffix.
    uint8_t     component_count = 1; ///< 1..4.

    std::function<void*(coopa::scene::Component&)> cast; ///< dynamic_cast to the concrete/base pointer this property expects.
    std::function<void(void*, float*)>             get;  ///< Writes component_count floats.
    std::function<void(void*, const float*)>       set;  ///< Reads component_count floats.
};

/**
 * @class AnimatedPropertyRegistry
 * @brief Function-local-static registry of AnimatedProperty, keyed by "component_type.name".
 */
class AnimatedPropertyRegistry {
public:
    /** @brief The shared instance. Registers the built-in Transform properties on first call. */
    static AnimatedPropertyRegistry& instance() {
        static AnimatedPropertyRegistry registry;
        registry.add_builtins_if_empty_();
        return registry;
    }

    void add(AnimatedProperty prop) {
        props_[prop.component_type + "." + prop.name] = std::move(prop);
    }

    /**
     * @brief Looks up a property, decoding any channel suffix on `property`.
     *
     * @param component_type A Component::type_name() value.
     * @param property Bare property name, optionally with a channel suffix.
     * @param out_channel_mask If non-null, receives the decoded mask —
     *        0x0F when `property` carried no suffix, meaning "every channel";
     *        callers should intersect this with (1 << component_count) - 1.
     * @return The matching property, or nullptr.
     */
    const AnimatedProperty* find(const std::string& component_type, const std::string& property,
                                 uint8_t* out_channel_mask = nullptr) const {
        std::string base;
        uint8_t mask = 0x0F;
        split_channel_suffix(property, base, mask);
        auto it = props_.find(component_type + "." + base);
        if (it == props_.end()) return nullptr;
        if (out_channel_mask) *out_channel_mask = mask;
        return &it->second;
    }

    /** @brief Lists every registered property name (bare, no channel suffix) for a component type. */
    std::vector<std::string> properties_for(const std::string& component_type) const {
        std::vector<std::string> result;
        for (const auto& entry : props_) {
            if (entry.second.component_type == component_type) result.push_back(entry.second.name);
        }
        return result;
    }

    /** @brief Test hook: clears everything, then re-adds the built-ins. */
    void clear() {
        props_.clear();
        add_builtins_if_empty_();
    }

    // --- One-line registration helpers ---

    /** @brief Registers a single-float property via a getter/setter method pair. */
    template <typename C>
    void register_float(const std::string& component_type, const std::string& name,
                         float (C::*getter)() const, void (C::*setter)(float)) {
        AnimatedProperty prop;
        prop.component_type = component_type;
        prop.name = name;
        prop.component_count = 1;
        prop.cast = [](coopa::scene::Component& c) -> void* { return dynamic_cast<C*>(&c); };
        prop.get = [getter](void* t, float* out) { out[0] = (static_cast<C*>(t)->*getter)(); };
        prop.set = [setter](void* t, const float* in) { (static_cast<C*>(t)->*setter)(in[0]); };
        add(std::move(prop));
    }

    /** @brief Registers a vector property whose getter returns `const V&`. */
    template <typename C, typename V>
    void register_vec(const std::string& component_type, const std::string& name,
                       const V& (C::*getter)() const, void (C::*setter)(const V&)) {
        AnimatedProperty prop;
        prop.component_type = component_type;
        prop.name = name;
        prop.component_count = static_cast<uint8_t>(V::length());
        prop.cast = [](coopa::scene::Component& c) -> void* { return dynamic_cast<C*>(&c); };
        prop.get = [getter](void* t, float* out) {
            const V& v = (static_cast<C*>(t)->*getter)();
            for (int i = 0; i < V::length(); ++i) out[i] = v[i];
        };
        prop.set = [setter](void* t, const float* in) {
            V v{};
            for (int i = 0; i < V::length(); ++i) v[i] = in[i];
            (static_cast<C*>(t)->*setter)(v);
        };
        add(std::move(prop));
    }

    /**
     * @brief Registers a vector property whose getter returns `V` BY VALUE.
     *
     * Needed separately from register_vec() because a single overload set
     * cannot bind both a `const V& (C::*)() const` and a `V (C::*)() const`
     * getter — e.g. RectTransform::offset_min()/offset_max() return by value.
     */
    template <typename C, typename V>
    void register_vec_value(const std::string& component_type, const std::string& name,
                            V (C::*getter)() const, void (C::*setter)(const V&)) {
        AnimatedProperty prop;
        prop.component_type = component_type;
        prop.name = name;
        prop.component_count = static_cast<uint8_t>(V::length());
        prop.cast = [](coopa::scene::Component& c) -> void* { return dynamic_cast<C*>(&c); };
        prop.get = [getter](void* t, float* out) {
            V v = (static_cast<C*>(t)->*getter)();
            for (int i = 0; i < V::length(); ++i) out[i] = v[i];
        };
        prop.set = [setter](void* t, const float* in) {
            V v{};
            for (int i = 0; i < V::length(); ++i) v[i] = in[i];
            (static_cast<C*>(t)->*setter)(v);
        };
        add(std::move(prop));
    }

    /** @brief Registers a vector property backed directly by a public member of C. */
    template <typename C, typename V>
    void register_vec_member(const std::string& component_type, const std::string& name, V C::*member) {
        register_vec_member_as<C, V>(component_type, name, member);
    }

    /**
     * @brief Registers a vector property backed by a BASE class's public member,
     *        keyed under a DERIVED type_name().
     *
     * This is how Graphic::color (a member of the abstract base Graphic) is
     * registered separately under "Image", "Text", "RawImage", "Icon" — the
     * cast dynamic_casts to Base*, which succeeds for any derived instance
     * regardless of which concrete key string it was registered under.
     */
    template <typename Base, typename V>
    void register_vec_member_as(const std::string& component_type, const std::string& name, V Base::*member) {
        AnimatedProperty prop;
        prop.component_type = component_type;
        prop.name = name;
        prop.component_count = static_cast<uint8_t>(V::length());
        prop.cast = [](coopa::scene::Component& c) -> void* { return dynamic_cast<Base*>(&c); };
        prop.get = [member](void* t, float* out) {
            V& v = static_cast<Base*>(t)->*member;
            for (int i = 0; i < V::length(); ++i) out[i] = v[i];
        };
        prop.set = [member](void* t, const float* in) {
            V& v = static_cast<Base*>(t)->*member;
            for (int i = 0; i < V::length(); ++i) v[i] = in[i];
        };
        add(std::move(prop));
    }

private:
    void add_builtins_if_empty_() {
        if (!props_.empty()) return;

        // Transform.position/rotation/scale route through
        // TransformComponent::transform() rather than a direct member
        // pointer on TransformComponent itself, so these three are
        // hand-written instead of going through register_vec() (which
        // expects the getter/setter pair directly on the cast-to type).
        // All three of coopa::util::Transform's accessors share the shape
        // `const glm::vec3& (...)() const` / `void set_X(const glm::vec3&)`,
        // so the cast lambda is identical across all three registrations.
        auto cast_to_transform = [](coopa::scene::Component& c) -> void* {
            return dynamic_cast<coopa::scene::TransformComponent*>(&c);
        };

        AnimatedProperty position;
        position.component_type = "Transform";
        position.name = "position";
        position.component_count = 3;
        position.cast = cast_to_transform;
        position.get = [](void* t, float* out) {
            const glm::vec3& v = static_cast<coopa::scene::TransformComponent*>(t)->transform().position();
            out[0] = v.x; out[1] = v.y; out[2] = v.z;
        };
        position.set = [](void* t, const float* in) {
            static_cast<coopa::scene::TransformComponent*>(t)->transform().set_position(glm::vec3(in[0], in[1], in[2]));
        };
        add(std::move(position));

        // Registered as "rotation" (not "rotation_degrees") to match the
        // clip YAML schema. Per-channel Euler-degree lerp, deliberately not
        // shortest-path-wrapped: 0 -> 720 means two full spins, matching
        // Unity's Euler-curve semantics. No quaternions are introduced here
        // or anywhere in coopa::util::Transform.
        AnimatedProperty rotation;
        rotation.component_type = "Transform";
        rotation.name = "rotation";
        rotation.component_count = 3;
        rotation.cast = cast_to_transform;
        rotation.get = [](void* t, float* out) {
            const glm::vec3& v = static_cast<coopa::scene::TransformComponent*>(t)->transform().rotation_degrees();
            out[0] = v.x; out[1] = v.y; out[2] = v.z;
        };
        rotation.set = [](void* t, const float* in) {
            static_cast<coopa::scene::TransformComponent*>(t)->transform().set_rotation(glm::vec3(in[0], in[1], in[2]));
        };
        add(std::move(rotation));

        // Quaternion rotation (x, y, z, w) -- what rigs key. Channels interpolate per
        // component like every track, then the setter normalises: nlerp, which follows the
        // shortest arc as long as consecutive keys share a hemisphere (dot >= 0) -- the
        // toyeditor flips a key's sign when recording to keep it that way. No gimbal lock and
        // no 350 -> 10 degree long way round, unlike the Euler "rotation" above.
        AnimatedProperty rotation_quat;
        rotation_quat.component_type = "Transform";
        rotation_quat.name = "rotation_quat";
        rotation_quat.component_count = 4;
        rotation_quat.cast = cast_to_transform;
        rotation_quat.get = [](void* t, float* out) {
            const glm::quat q = static_cast<coopa::scene::TransformComponent*>(t)->transform().rotation_quat();
            out[0] = q.x; out[1] = q.y; out[2] = q.z; out[3] = q.w;
        };
        rotation_quat.set = [](void* t, const float* in) {
            glm::quat q(in[3], in[0], in[1], in[2]);   // glm: (w, x, y, z)
            const float len = glm::length(q);
            q = len > 1e-8f ? q / len : glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
            static_cast<coopa::scene::TransformComponent*>(t)->transform().set_rotation_quat(q);
        };
        add(std::move(rotation_quat));

        AnimatedProperty scale;
        scale.component_type = "Transform";
        scale.name = "scale";
        scale.component_count = 3;
        scale.cast = cast_to_transform;
        scale.get = [](void* t, float* out) {
            const glm::vec3& v = static_cast<coopa::scene::TransformComponent*>(t)->transform().scale();
            out[0] = v.x; out[1] = v.y; out[2] = v.z;
        };
        scale.set = [](void* t, const float* in) {
            static_cast<coopa::scene::TransformComponent*>(t)->transform().set_scale(glm::vec3(in[0], in[1], in[2]));
        };
        add(std::move(scale));
    }

    std::unordered_map<std::string, AnimatedProperty> props_;
};

} // namespace anim
} // namespace coopa

#endif // COOPA_ANIMATION_ANIMATED_PROPERTY_H
