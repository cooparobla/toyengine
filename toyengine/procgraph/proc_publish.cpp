/**
 * @file proc_publish.cpp
 * @brief Realizing a baked tree: build_object(), then the out-of-band payloads.
 */

#include <toyengine/procgraph/proc_publish.h>

#include <algorithm>
#include <memory>
#include <string>

#include <glm/gtc/quaternion.hpp>

#include <coopa/asset/asset_manager.h>
#include <coopa/scene/scene.h>
#include <coopa/scene/scene_loader.h>
#include <coopa/scene/scene_object.h>
#include <gfxcoopa/engine/components/mesh_renderer.h>
#include <gfxcoopa/engine/data/mesh.h>
#include <physxcoopa/components/mesh_collider.h>
#include <physxcoopa/geometry/triangle_mesh.h>

#include <toyengine/instance/instance_painter.h>
#include <toyengine/instance/instance_set.h>
#include <toyengine/scene/runtime_object.h>

namespace toy {
namespace procgraph {
namespace {

using coopa::gfx::engine::components::MeshRenderer;
using coopa::gfx::engine::data::Mesh;
using coopa::scene::SceneObject;

/// @brief A name safe to put in an asset id and in an object path.
std::string sanitize_(const std::string& s) {
    std::string out = s.empty() ? std::string("object") : s;
    for (char& c : out) {
        if (c == '/' || c == '\\' || c == ' ') c = '_';
    }
    return out;
}

/**
 * @brief One object's stable id within a bake: its path through the tree.
 *
 * Path-derived rather than counter-derived on purpose: a re-bake must land on the SAME id so
 * `AssetManager::create()` retires the previous payload instead of leaking a slot per bake. A
 * counter would hand out a new id every time.
 */
std::string path_id_(const std::string& parent_path, const BakedObject& obj, size_t index) {
    return parent_path + "/" + std::to_string(index) + "_" + sanitize_(obj.name);
}

/// @brief Writes the transform onto an already-built object.
void apply_transform_(SceneObject& obj, const BakedObject& baked) {
    auto* tc = obj.get_transform();
    if (tc == nullptr) return;
    coopa::util::Transform& t = tc->transform();
    t.set_position(baked.position);
    t.set_rotation_quat(baked.rotation);
    t.set_scale(baked.scale);
}

/// @brief A convex hull's triangles, or the mesh's own when a hull is not wanted.
std::shared_ptr<const coopa::physx::geometry::TriangleMesh> build_collision_(
    const MeshGeometry& mesh, BakedObject::Collider kind) {
    if (mesh.empty()) return nullptr;
    std::vector<glm::vec3> positions;
    positions.reserve(mesh.vertices.size());
    for (const auto& v : mesh.vertices) positions.push_back(v.position);

    // Convex is not implemented as a hull here: physxcoopa's TriangleMesh is the runtime shape
    // this engine has, and a hull builder does not exist in the tree. A convex request therefore
    // gets the trimesh, which is correct but heavier -- better than silently producing nothing.
    (void)kind;
    auto shape = std::make_shared<coopa::physx::geometry::TriangleMesh>(
        coopa::physx::geometry::TriangleMesh::from_triangles(positions, mesh.indices));
    if (shape->empty()) return nullptr;
    return shape;
}

/// @brief Recursively walks the baked tree and the spawned objects together.
void attach_payloads_(const BakedObject& baked, SceneObject& obj, const PublishContext& ctx,
                      const std::string& path, PublishedGraph& out) {
    apply_transform_(obj, baked);

    if (baked.mesh && !baked.mesh->empty()) {
        if (ctx.device == nullptr || ctx.allocator == nullptr || ctx.assets == nullptr) {
            out.errors.push_back("cannot upload geometry for '" + baked.name +
                                 "': no device, allocator or asset manager");
        } else {
            MeshCpuData cpu = baked.mesh->to_cpu_data();
            // Uploaded here on the main thread. The terrain system uploads from its worker
            // instead, which is also allowed; doing it here keeps the publish step the single
            // place that touches the device.
            auto gpu = std::make_shared<Mesh>(
                Mesh::from_cpu(*ctx.device, *ctx.allocator, std::move(cpu)));
            const std::string asset_id = ctx.id_prefix + path + "/mesh";
            auto handle = ctx.assets->create<Mesh>(asset_id, gpu);
            out.asset_ids.push_back(asset_id);

            auto* renderer = obj.get_component<MeshRenderer>();
            if (renderer == nullptr) renderer = obj.add_component<MeshRenderer>();
            renderer->set_mesh(handle);
        }

        if (baked.collider != BakedObject::Collider::None) {
            auto shape = build_collision_(*baked.mesh, baked.collider);
            if (shape == nullptr) {
                out.errors.push_back("collider for '" + baked.name + "' came out empty");
            } else {
                auto* collider = obj.get_component<coopa::physx::components::MeshCollider>();
                // The component was already created by build_object() from the emitted
                // MeshCollider spec; only the shape is attached here.
                if (collider == nullptr) {
                    collider = obj.add_component<coopa::physx::components::MeshCollider>();
                }
                collider->set_mesh(std::move(shape));
            }
        }
    }

    if (baked.instances && !baked.instances->empty()) {
        // A synthetic, non-file-backed key. The emitted InstancePainter points at it, and the
        // EXISTING InstancePainterSystem then does the instanced rendering, frustum culling,
        // distance thinning and streamed collision -- this module draws nothing itself.
        const std::string key = ctx.id_prefix + path + ".instances";
        auto set = instance::InstanceSetStore::instance().adopt_transient(key);
        if (set == nullptr) {
            out.errors.push_back("could not claim an instance set for '" + baked.name + "'");
        } else {
            set->clear();

            // The painter is created here when the graph did not emit one. Requiring a Make
            // Instancer node would mean a graph that produced instances and wired them to an
            // Object Output drew nothing, with no error anywhere -- so geometry alone is enough,
            // and Make Instancer exists only to override the settings below.
            auto* painter = obj.get_component<instance::InstancePainter>();
            if (painter == nullptr) painter = obj.add_component<instance::InstancePainter>();

            // Settings an emitted Make Instancer component carried, applied to every prototype
            // this rebuild creates. Read before the list is replaced.
            instance::InstanceCollision collision = instance::InstanceCollision::None;
            float draw_distance = 0.0f;
            if (!painter->prototypes.empty()) {
                collision     = painter->prototypes.front().collision;
                draw_distance = painter->prototypes.front().draw_distance;
            }

            // Rebuilt from the groups rather than trusted from YAML: a record's `prototype` is an
            // INDEX into this list, so the two must be built together or the instances come out
            // wearing each other's meshes.
            painter->prototypes.clear();
            painter->prototypes.reserve(baked.instances->size());
            for (const InstanceGroup& group : *baked.instances) {
                instance::InstancePrototype prototype;
                prototype.name = group.prototype;
                if (group.prototype_is_mesh) {
                    prototype.mesh = group.prototype;
                    // A `mesh` prototype needs its own MeshRenderer proxy, and only whoever has
                    // the AssetManager can build it -- resolution runs later and has neither the
                    // manager nor a base dir, so it just warns and paints nothing. The component
                    // parser does this for an authored painter (instance_yaml.cpp); a painter
                    // built in code has to do it here.
                    if (ctx.assets != nullptr) {
                        prototype.own_proxy = std::make_shared<MeshRenderer>();
                        prototype.own_proxy->set_mesh_path(group.prototype);
                        prototype.own_proxy->set_mesh(ctx.assets->load_async<Mesh>(
                            "meshes/" + group.prototype + ".yaml", ctx.scene_path));
                        prototype.material = prototype.own_proxy->material;
                    } else {
                        out.errors.push_back("cannot build a proxy for mesh prototype '" +
                                             group.prototype + "': no asset manager");
                    }
                } else {
                    prototype.object = group.prototype;
                }
                prototype.collision     = collision;
                prototype.draw_distance = draw_distance;
                // The graph has already baked the final transforms, so the painter must not
                // re-randomise them: a non-zero yaw_range would spin every instance a second time.
                prototype.scale_range = glm::vec2(1.0f, 1.0f);
                prototype.yaw_range   = glm::vec2(0.0f, 0.0f);
                painter->prototypes.push_back(std::move(prototype));
            }

            // InstanceRecord positions are WORLD space: painted instances bypass the scene
            // graph entirely, so unlike a baked mesh (which hangs off this object and inherits
            // its transform) they are not carried by the parent. Without this, three copies of
            // one graph on three differently-placed objects all land on top of each other.
            auto* owner_tc = obj.get_transform();
            const glm::mat4 to_world = owner_tc ? owner_tc->get_world_matrix() : glm::mat4(1.0f);

            for (size_t gi = 0; gi < baked.instances->size(); ++gi) {
                const InstanceGroup& group = (*baked.instances)[gi];
                for (size_t i = 0; i < group.transforms.size(); ++i) {
                    instance::InstanceRecord record;
                    const glm::mat4 m = to_world * group.transforms[i];
                    record.position = glm::vec3(m[3]);
                    glm::vec3 sx(m[0]), sy(m[1]), sz(m[2]);
                    record.scale = glm::vec3(glm::length(sx), glm::length(sy), glm::length(sz));
                    if (record.scale.x > 1e-8f) sx /= record.scale.x;
                    if (record.scale.y > 1e-8f) sy /= record.scale.y;
                    if (record.scale.z > 1e-8f) sz /= record.scale.z;
                    record.rotation  = glm::quat_cast(glm::mat3(sx, sy, sz));
                    record.prototype = static_cast<std::uint16_t>(gi);
                    record.seed = i < group.seeds.size() ? group.seeds[i]
                                                         : static_cast<std::uint32_t>(i);
                    set->add(record);
                }
            }
            out.instance_keys.push_back(key);

            // `data` is assigned, not just `data_file`. The component's own parser already ran
            // during build_object() and bound `data` to a set it acquired from the store by path
            // -- an empty, persistent one. Setting only `data_file` would leave the painter
            // reading that empty set: instances baked, nothing drawn, and no error anywhere.
            painter->data      = set;
            painter->data_file = key;
        }
    }

    for (size_t i = 0; i < baked.children.size() && i < obj.children().size(); ++i) {
        if (obj.children()[i]) {
            attach_payloads_(baked.children[i], *obj.children()[i], ctx,
                             path_id_(path, baked.children[i], i), out);
        }
    }
}

} // namespace

fkyaml::node object_to_node(const BakedObject& obj) {
    fkyaml::node node = fkyaml::node::mapping();
    node["name"] = fkyaml::node(obj.name.empty() ? std::string("Generated") : obj.name);

    if (!obj.components.empty()) {
        // A SEQUENCE of mappings, each carrying its own `type:` alongside its keys. This is the
        // shape SceneLoader::parse_component_ dispatches on, and getting it wrong is silent: it
        // iterates `components` as a sequence and `return`s on any entry without a `type` key, so
        // a mapping-keyed-by-type block would spawn objects with no components and no error.
        fkyaml::node components = fkyaml::node::sequence();
        for (const auto& c : obj.components) {
            if (c.type.empty()) continue;
            fkyaml::node entry = fkyaml::node::mapping();
            entry["type"] = fkyaml::node(c.type);
            if (c.props.is_mapping()) {
                for (auto it = c.props.begin(); it != c.props.end(); ++it) {
                    const std::string key = it.key().get_value<std::string>();
                    if (key == "type") continue;   // Never let props shadow the dispatch key.
                    entry[key] = it.value();
                }
            }
            components.as_seq().push_back(std::move(entry));
        }
        node["components"] = std::move(components);
    }

    // A baked mesh's material comes from its FIRST slot name, emitted as an ordinary
    // MeshRenderer spec so SceneLoader parses it the way it parses every other material
    // reference. Without this a Set Material node renames a slot and nothing else: publish
    // attaches the mesh handle to a default renderer and the geometry comes out white, with
    // nothing anywhere reporting that the material went missing.
    if (obj.mesh != nullptr && !obj.mesh->slot_names.empty() && !obj.mesh->slot_names[0].empty()) {
        if (!node.contains("components")) node["components"] = fkyaml::node::sequence();
        bool has_renderer = false;
        for (const auto& c : node.at("components").as_seq()) {
            if (c.contains("type") && c.at("type").get_value<std::string>() == "MeshRenderer") {
                has_renderer = true;
            }
        }
        if (!has_renderer) {
            fkyaml::node renderer = fkyaml::node::mapping();
            renderer["type"] = fkyaml::node(std::string("MeshRenderer"));
            renderer["material"] = fkyaml::node(obj.mesh->slot_names[0]);
            node["components"].as_seq().push_back(std::move(renderer));
        }
    }

    if (!obj.children.empty()) {
        fkyaml::node children = fkyaml::node::sequence();
        for (const auto& child : obj.children) children.as_seq().push_back(object_to_node(child));
        node["children"] = std::move(children);
    }
    return node;
}

PublishedGraph publish(const BakeResult& result, const PublishContext& ctx,
                       const PublishedGraph& previous) {
    PublishedGraph out;
    unpublish(previous, ctx);

    if (ctx.scene == nullptr || ctx.owner == nullptr) {
        out.errors.push_back("publish() needs a scene and an owner object");
        return out;
    }
    if (!result.root.carries_anything()) {
        // Nothing to spawn is a legitimate outcome, not a failure: a graph under construction, or
        // one whose inputs selected an empty branch.
        return out;
    }

    fkyaml::node node = object_to_node(result.root);
    std::unique_ptr<SceneObject> built;
    try {
        built = coopa::scene::SceneLoader::build_object(
            node, ctx.owner->get_transform(), ctx.scene_path, /*auto_transform=*/true);
    } catch (const std::exception& e) {
        out.errors.push_back(std::string("could not build the baked object tree: ") + e.what());
        return out;
    }
    if (!built) {
        out.errors.push_back("building the baked object tree produced nothing");
        return out;
    }

    SceneObject* root = built.get();
    // Marked before it is adopted, so the editor never sees an unmarked runtime object even for a
    // frame -- and the whole subtree inherits it, which is what is_runtime_object() walks for.
    root->add_component<toy::scene::RuntimeObject>(
        "ProceduralGraph", "baked from a graph; edit the graph asset, not this");

    ctx.owner->add_child(std::move(built));
    ctx.scene->adopt(*root);

    attach_payloads_(result.root, *root, ctx, "", out);

    root->start();
    out.root = root;
    return out;
}

coopa::scene::SceneObject* PublishedGraph::live_root(const coopa::scene::SceneObject* owner) const {
    if (root == nullptr || owner == nullptr) return nullptr;
    for (const auto& child : owner->children()) {
        // Address comparison only: `root` may already be freed, and reading through it is what
        // this function exists to prevent.
        if (child.get() != root) continue;
        const auto* marker = child->get_component<toy::scene::RuntimeObject>();
        if (marker == nullptr || marker->owner_system != "ProceduralGraph") return nullptr;
        return child.get();
    }
    return nullptr;
}

void unpublish(const PublishedGraph& published, const PublishContext& ctx) {
    if (SceneObject* live = published.live_root(ctx.owner)) {
        // detach_child hands back ownership; letting it fall out of scope destroys the subtree,
        // which is what retiring a previous bake means. Going through live_root() means a
        // subtree the editor already destroyed in a scene rebuild is simply not there to retire,
        // rather than a stale pointer handed to detach_child.
        std::unique_ptr<SceneObject> retired = ctx.owner->detach_child(live);
        (void)retired;
    }
    if (ctx.assets != nullptr) {
        for (const std::string& id : published.asset_ids) {
            ctx.assets->unload(coopa::asset::AssetId::from_path(id));
        }
    }
    for (const std::string& key : published.instance_keys) {
        instance::InstanceSetStore::instance().forget(key);
    }
}

} // namespace procgraph
} // namespace toy
