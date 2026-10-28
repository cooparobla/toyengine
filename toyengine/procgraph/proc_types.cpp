/**
 * @file proc_types.cpp
 * @brief Socket type registration, YAML hooks and implicit conversions.
 */

#include <toyengine/procgraph/proc_types.h>

#include <cmath>

#include <glm/gtc/matrix_transform.hpp>

#include <toyengine/core/yaml_read.h>

namespace toy {
namespace procgraph {
namespace {

using coopa::nodegraph::hash_bytes;
using coopa::nodegraph::TypeRegistry;

// --- YAML hooks -------------------------------------------------------------------------------
// A socket default can only round-trip through a file for a type that registers these, which is
// why the scalar and vector types all do and the aggregates (Geometry, ObjectSet) do not: there is
// no sensible literal for a mesh in a graph file.

bool float_from_yaml(const fkyaml::node& n, void* dst) {
    if (!n.is_float_number() && !n.is_integer()) return false;
    *static_cast<float*>(dst) = core::yaml::num(n, 0.0f);
    return true;
}
bool float_to_yaml(const void* src, fkyaml::node& out) {
    out = fkyaml::node(static_cast<double>(*static_cast<const float*>(src)));
    return true;
}

bool int_from_yaml(const fkyaml::node& n, void* dst) {
    if (n.is_integer()) {
        *static_cast<int32_t*>(dst) = static_cast<int32_t>(n.get_value<int64_t>());
        return true;
    }
    if (n.is_float_number()) {
        *static_cast<int32_t*>(dst) = static_cast<int32_t>(n.get_value<double>());
        return true;
    }
    return false;
}
bool int_to_yaml(const void* src, fkyaml::node& out) {
    out = fkyaml::node(static_cast<int64_t>(*static_cast<const int32_t*>(src)));
    return true;
}

bool bool_from_yaml(const fkyaml::node& n, void* dst) {
    if (!n.is_boolean()) return false;
    *static_cast<bool*>(dst) = n.get_value<bool>();
    return true;
}
bool bool_to_yaml(const void* src, fkyaml::node& out) {
    out = fkyaml::node(*static_cast<const bool*>(src));
    return true;
}

bool vector_from_yaml(const fkyaml::node& n, void* dst) {
    // A scalar broadcasts, matching the Float -> Vector conversion, so `default: 0` works.
    if (n.is_float_number() || n.is_integer()) {
        *static_cast<glm::vec3*>(dst) = glm::vec3(core::yaml::num(n, 0.0f));
        return true;
    }
    if (!n.is_sequence() || n.size() < 3) return false;
    *static_cast<glm::vec3*>(dst) = core::yaml::as_vec3(n);
    return true;
}
bool vector_to_yaml(const void* src, fkyaml::node& out) {
    const auto* v = static_cast<const glm::vec3*>(src);
    out = fkyaml::node::sequence();
    for (int i = 0; i < 3; ++i) out.as_seq().push_back(fkyaml::node(static_cast<double>((*v)[i])));
    return true;
}

bool color_from_yaml(const fkyaml::node& n, void* dst) {
    auto* c = static_cast<glm::vec4*>(dst);
    if (n.is_sequence() && n.size() >= 3) {
        const glm::vec3 rgb = core::yaml::as_rgb(n);
        const float a = n.size() >= 4 ? core::yaml::num(n[3], 1.0f) : 1.0f;
        *c = glm::vec4(rgb, a);
        return true;
    }
    if (n.is_string()) {   // "#rrggbb", via the engine's shared colour parsing.
        *c = glm::vec4(core::yaml::as_rgb(n), 1.0f);
        return true;
    }
    return false;
}
bool color_to_yaml(const void* src, fkyaml::node& out) {
    const auto* c = static_cast<const glm::vec4*>(src);
    out = fkyaml::node::sequence();
    for (int i = 0; i < 4; ++i) out.as_seq().push_back(fkyaml::node(static_cast<double>((*c)[i])));
    return true;
}

bool rotation_from_yaml(const fkyaml::node& n, void* dst) {
    // Authored in Euler DEGREES, like every other rotation in this engine's YAML.
    if (!n.is_sequence() || n.size() < 3) return false;
    const glm::vec3 deg = core::yaml::as_vec3(n);
    *static_cast<glm::quat*>(dst) = glm::quat(glm::radians(deg));
    return true;
}
bool rotation_to_yaml(const void* src, fkyaml::node& out) {
    const glm::vec3 deg = glm::degrees(glm::eulerAngles(*static_cast<const glm::quat*>(src)));
    out = fkyaml::node::sequence();
    for (int i = 0; i < 3; ++i) out.as_seq().push_back(fkyaml::node(static_cast<double>(deg[i])));
    return true;
}

bool string_from_yaml(const fkyaml::node& n, void* dst) {
    if (!n.is_string()) return false;
    *static_cast<std::string*>(dst) = n.get_value<std::string>();
    return true;
}
bool string_to_yaml(const void* src, fkyaml::node& out) {
    out = fkyaml::node(*static_cast<const std::string*>(src));
    return true;
}

// --- Hashes ----------------------------------------------------------------------------------
// Drives `input_hash`, so a binding that moved is detected. An aggregate hashes its identity
// (which pointer it holds) rather than its contents: a re-resolve that produced the same shared
// payload reads as unchanged, and one that built a new payload reads as changed. That is the safe
// direction -- it can over-report a change but never under-report one.

void hash_f(const void* p, uint64_t& s) { hash_bytes(p, sizeof(float), s); }
void hash_i(const void* p, uint64_t& s) { hash_bytes(p, sizeof(int32_t), s); }
void hash_b(const void* p, uint64_t& s) { hash_bytes(p, sizeof(bool), s); }
void hash_v3(const void* p, uint64_t& s) { hash_bytes(p, sizeof(glm::vec3), s); }
void hash_v4(const void* p, uint64_t& s) { hash_bytes(p, sizeof(glm::vec4), s); }
void hash_q(const void* p, uint64_t& s) { hash_bytes(p, sizeof(glm::quat), s); }
void hash_str(const void* p, uint64_t& s) {
    const auto* str = static_cast<const std::string*>(p);
    hash_bytes(str->data(), str->size(), s);
}
void hash_object(const void* p, uint64_t& s) {
    const auto* o = static_cast<const ObjectRef*>(p);
    hash_bytes(o->name.data(), o->name.size(), s);
    hash_bytes(&o->position, sizeof(glm::vec3), s);
    hash_bytes(&o->rotation, sizeof(glm::quat), s);
    hash_bytes(&o->scale, sizeof(glm::vec3), s);
    hash_bytes(&o->valid, sizeof(bool), s);
}
void hash_object_set(const void* p, uint64_t& s) {
    // Every member's transform is folded in, not just the pointer: a `tag` binding re-resolves
    // into a fresh vector every frame, so pointer identity would report "changed" every time and
    // `bake: on_change` would re-bake forever.
    const auto* set = static_cast<const ObjectSet*>(p);
    const size_t n = set->size();
    hash_bytes(&n, sizeof(n), s);
    for (size_t i = 0; i < n; ++i) hash_object(&set->at(i), s);
}
void hash_component(const void* p, uint64_t& s) {
    const auto* c = static_cast<const ComponentSpec*>(p);
    hash_bytes(c->type.data(), c->type.size(), s);
    const std::string text = fkyaml::node::serialize(c->props);
    hash_bytes(text.data(), text.size(), s);
}

template <typename T>
void hash_shared_identity(const void* p, uint64_t& s) {
    const void* id = static_cast<const T*>(p)->data.get();
    hash_bytes(&id, sizeof(id), s);
}
void hash_geometry(const void* p, uint64_t& s) {
    const auto* g = static_cast<const GeometrySet*>(p);
    const MeshGeometry* m = g->mesh();
    const PointCloud* pts = g->points();
    const size_t verts = m ? m->vertices.size() : 0;
    const size_t tris  = m ? m->triangle_count() : 0;
    const size_t np    = pts ? pts->size() : 0;
    const size_t ni    = g->instances().size();
    hash_bytes(&verts, sizeof(verts), s);
    hash_bytes(&tris, sizeof(tris), s);
    hash_bytes(&np, sizeof(np), s);
    hash_bytes(&ni, sizeof(ni), s);
}

} // namespace

glm::mat4 ObjectRef::matrix() const {
    glm::mat4 m = glm::translate(glm::mat4(1.0f), position);
    m *= glm::mat4_cast(rotation);
    return glm::scale(m, scale);
}

const ProcTypeIds& register_proc_types() {
    static const ProcTypeIds ids = [] {
        auto& reg = TypeRegistry::instance();
        ProcTypeIds t;

        t.float_t       = reg.add<float>("Float");
        t.int_t         = reg.add<int32_t>("Int");
        t.bool_t        = reg.add<bool>("Bool");
        t.vector_t      = reg.add<glm::vec3>("Vector");
        t.color_t       = reg.add<glm::vec4>("Color");
        t.rotation_t    = reg.add<glm::quat>("Rotation");
        t.string_t      = reg.add<std::string>("String");
        t.geometry_t    = reg.add<GeometrySet>("Geometry");
        t.object_t      = reg.add<ObjectRef>("Object");
        t.object_set_t  = reg.add<ObjectSet>("ObjectSet");
        t.component_t   = reg.add<ComponentSpec>("Component");
        t.object_spec_t = reg.add<ObjectSpec>("ObjectSpec");

        reg.set_yaml_hooks(t.float_t, float_from_yaml, float_to_yaml);
        reg.set_yaml_hooks(t.int_t, int_from_yaml, int_to_yaml);
        reg.set_yaml_hooks(t.bool_t, bool_from_yaml, bool_to_yaml);
        reg.set_yaml_hooks(t.vector_t, vector_from_yaml, vector_to_yaml);
        reg.set_yaml_hooks(t.color_t, color_from_yaml, color_to_yaml);
        reg.set_yaml_hooks(t.rotation_t, rotation_from_yaml, rotation_to_yaml);
        reg.set_yaml_hooks(t.string_t, string_from_yaml, string_to_yaml);

        reg.set_hash(t.float_t, hash_f);
        reg.set_hash(t.int_t, hash_i);
        reg.set_hash(t.bool_t, hash_b);
        reg.set_hash(t.vector_t, hash_v3);
        reg.set_hash(t.color_t, hash_v4);
        reg.set_hash(t.rotation_t, hash_q);
        reg.set_hash(t.string_t, hash_str);
        reg.set_hash(t.object_t, hash_object);
        reg.set_hash(t.object_set_t, hash_object_set);
        reg.set_hash(t.component_t, hash_component);
        reg.set_hash(t.geometry_t, hash_geometry);
        reg.set_hash(t.object_spec_t, hash_shared_identity<ObjectSpec>);

        // glm's vector operator== returns a bool but its quat comparison needs an explicit one,
        // and the aggregates compare by shared identity.
        reg.set_equals(t.vector_t, [](const void* a, const void* b) {
            return *static_cast<const glm::vec3*>(a) == *static_cast<const glm::vec3*>(b);
        });
        reg.set_equals(t.color_t, [](const void* a, const void* b) {
            return *static_cast<const glm::vec4*>(a) == *static_cast<const glm::vec4*>(b);
        });
        reg.set_equals(t.rotation_t, [](const void* a, const void* b) {
            const auto& qa = *static_cast<const glm::quat*>(a);
            const auto& qb = *static_cast<const glm::quat*>(b);
            return qa.x == qb.x && qa.y == qb.y && qa.z == qb.z && qa.w == qb.w;
        });

        // --- Implicit conversions: Blender's widening, and nothing lossy ---
        reg.add_conversion(t.int_t, t.float_t, [](const void* s, void* d) {
            *static_cast<float*>(d) = static_cast<float>(*static_cast<const int32_t*>(s));
        });
        reg.add_conversion(t.float_t, t.int_t, [](const void* s, void* d) {
            *static_cast<int32_t*>(d) = static_cast<int32_t>(std::lround(*static_cast<const float*>(s)));
        });
        reg.add_conversion(t.bool_t, t.float_t, [](const void* s, void* d) {
            *static_cast<float*>(d) = *static_cast<const bool*>(s) ? 1.0f : 0.0f;
        });
        reg.add_conversion(t.bool_t, t.int_t, [](const void* s, void* d) {
            *static_cast<int32_t*>(d) = *static_cast<const bool*>(s) ? 1 : 0;
        });
        reg.add_conversion(t.float_t, t.bool_t, [](const void* s, void* d) {
            *static_cast<bool*>(d) = *static_cast<const float*>(s) != 0.0f;
        });
        reg.add_conversion(t.int_t, t.bool_t, [](const void* s, void* d) {
            *static_cast<bool*>(d) = *static_cast<const int32_t*>(s) != 0;
        });
        reg.add_conversion(t.float_t, t.vector_t, [](const void* s, void* d) {
            const float f = *static_cast<const float*>(s);
            *static_cast<glm::vec3*>(d) = glm::vec3(f, f, f);
        });
        reg.add_conversion(t.int_t, t.vector_t, [](const void* s, void* d) {
            const auto f = static_cast<float>(*static_cast<const int32_t*>(s));
            *static_cast<glm::vec3*>(d) = glm::vec3(f, f, f);
        });
        reg.add_conversion(t.float_t, t.color_t, [](const void* s, void* d) {
            const float f = *static_cast<const float*>(s);
            *static_cast<glm::vec4*>(d) = glm::vec4(f, f, f, 1.0f);
        });
        reg.add_conversion(t.vector_t, t.color_t, [](const void* s, void* d) {
            *static_cast<glm::vec4*>(d) = glm::vec4(*static_cast<const glm::vec3*>(s), 1.0f);
        });
        reg.add_conversion(t.color_t, t.vector_t, [](const void* s, void* d) {
            *static_cast<glm::vec3*>(d) = glm::vec3(*static_cast<const glm::vec4*>(s));
        });
        // An Object feeding a Geometry socket hands over whatever mesh the snapshot captured, so
        // `{object: Blockout, geometry: true}` can be wired straight into a geometry operation.
        reg.add_conversion(t.object_t, t.geometry_t, [](const void* s, void* d) {
            *static_cast<GeometrySet*>(d) = static_cast<const ObjectRef*>(s)->geometry;
        });

        // Deliberately absent: Vector -> Float. Length, x, or the mean are all defensible and
        // picking one silently turns a wiring mistake into a plausible wrong number. An author
        // uses Separate XYZ or Vector Math instead.
        return t;
    }();
    return ids;
}

} // namespace procgraph
} // namespace toy
