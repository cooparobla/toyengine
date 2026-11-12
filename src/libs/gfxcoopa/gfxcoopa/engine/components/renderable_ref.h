/**
 * @file renderable_ref.h
 * @brief A renderable SceneObject pre-resolved to its MeshRenderer/Transform,
 *        plus a stateless helper to gather every one from a Scene.
 *
 * coopa::scene::Scene deliberately knows nothing about render component types
 * (see coopa/scene/scene.h) — it only exposes generic get_components<T>() /
 * find_objects_with<T>() / find_first_component<T>() queries. gather_renderables()
 * is the render side's equivalent of uicoopa's collect_canvases() (see
 * uicoopa/layout/canvas.h): a small free function built entirely on those
 * generic queries, not a cached wrapper class — callers (gi_baker.h,
 * gi_system.h) call it fresh each time they need the list, and otherwise
 * address coopa::scene::Scene directly.
 */

#ifndef GFXCOOPA_ENGINE_COMPONENTS_RENDERABLE_REF_H
#define GFXCOOPA_ENGINE_COMPONENTS_RENDERABLE_REF_H

#include <coopa/scene/scene.h>
#include <coopa/scene/scene_object.h>
#include <coopa/scene/components/transform_component.h>

#include <gfxcoopa/engine/components/mesh_renderer.h>

#include <vector>

namespace coopa {
namespace gfx {
namespace engine {
namespace components {

/**
 * @struct RenderableRef
 * @brief One renderable SceneObject, pre-resolved to its MeshRenderer/Transform.
 */
struct RenderableRef {
    coopa::scene::SceneObject*        object;    /**< Non-owning; the SceneObject carrying the renderer. */
    MeshRenderer*                     renderer;  /**< Non-owning; never null when this ref exists. */
    coopa::scene::TransformComponent* transform; /**< Non-owning; may be null if the object has none. */
};

/**
 * @brief Every active SceneObject in scene with a MeshRenderer, pre-resolved
 *        to its renderer/transform.
 *
 * A single DFS via Scene::find_objects_with<MeshRenderer>() — cheap relative
 * to a render frame. Stateless; call fresh whenever the current list is needed.
 */
inline std::vector<RenderableRef> gather_renderables(const coopa::scene::Scene& scene) {
    std::vector<RenderableRef> out;
    for (auto* obj : scene.find_objects_with<MeshRenderer>()) {
        out.push_back(RenderableRef{ obj, obj->get_component<MeshRenderer>(), obj->get_transform() });
    }
    return out;
}

} // namespace components
} // namespace engine
} // namespace gfx
} // namespace coopa

#endif // GFXCOOPA_ENGINE_COMPONENTS_RENDERABLE_REF_H
