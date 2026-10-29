/**
 * @file editor_types.h
 * @brief The small enums and aliases the editor's panels speak in.
 *
 * Separated from editor_app.h so a panel header can name `AssetType`, `PropTab`, `ObjectId`,
 * `Node` or `EditResult` in its declarations without including the whole app. That is what
 * keeps a panel's header off editor_app.h, and so keeps a panel edit from recompiling every
 * other panel. Add to this header only what a panel's *declarations* need.
 */

#ifndef TOYEDITOR_APP_EDITOR_TYPES_H
#define TOYEDITOR_APP_EDITOR_TYPES_H

#include "../core/scene_document.h"   // ObjectId, SceneDocument, Node
#include "../schema/inspector.h"      // EditResult, InspectorEnv
#include "asset_documents.h"          // MeshDocument, MaterialDocument, ThemeDocument

#include <uicoopa/immediate/imm.h>

#include <glm/glm.hpp>

// Types the shared structs below hold by value.
#include "editor/anim/clip_model.h"
#include "editor/mesh/edit_mesh.h"
#include "editor/mesh/mesh_ops.h"
#include "editor/mesh/mesh_topology.h"
#include "editor/mesh/paint.h"
#include "editor/mesh/primitives.h"
#include "editor/mesh/sculpt.h"
#include <coopa/animation/animation_clip.h>

#include <filesystem>

// Engine types a panel's *declarations* mention (always by pointer or reference). Forward
// declarations only: a panel header must not pull the engine in, or it stops being cheap.
namespace toy {
namespace particles { class ParticleSystem; }
// One component type serves both terrain modes (toyengine/world/terrain_component.h), so
// HeightmapTerrain is an ALIAS for it, not a class of its own -- aliasing the incomplete type
// keeps the panel headers that name it by pointer as cheap as a forward declaration.
namespace world { class TerrainComponent; using HeightmapTerrain = TerrainComponent; }
namespace weather { class WeatherSystem; }
namespace water { class WaterSystem; }
}  // namespace toy

namespace toy::editor {

namespace imm = coopa::ui::imm;
namespace fs  = std::filesystem;

/** @brief Bind-to-Rig weighting method. Shared by the Weights and Paint panels. */
enum class BindWeights { Automatic = 0, Envelope, EmptyGroups };

/**
 * @brief The kind of asset the editor has open. Exactly one asset is open at a time, and its
 *        type decides what the viewer shows and what the Properties editor offers.
 */
enum class AssetType { None = 0, Scene, Object, Mesh, Material, Texture, UI, Theme, Audio, NodeGraph };
/** @brief Blender's interaction modes for a mesh object (the viewport header's mode dropdown). */
enum class InteractionMode { Object = 0, Edit, Sculpt, VertexPaint, WeightPaint, Pose, Pen };
/** @brief Viewport shading, Blender's four buttons. */
enum class Shading { Wireframe = 0, Solid = 1, MaterialPreview = 2, Full = 3 };

/**
 * @brief How an object's origin is drawn in the viewport (Blender's "empty" display).
 *
 * Only objects with no mesh of their own get one -- a rig's joints, markers, group parents.
 */
enum class OriginStyle {
    Axes = 0,   ///< Three lines through the origin, one per local axis. The default.
    Orb,        ///< A small ball at the origin, with a tick along each axis.
};
/** @brief The origin glyph's size in metres when neither the object nor an ancestor sets one. */
inline constexpr float kDefaultOriginSize = 0.5f;
/** @brief The Properties editor's tabs. */
enum class PropTab { Tool, Render, Output, Scene, World, Object, Components, Physics, Data, Material, Canvas, Bindings, Theme, Graph, GraphNode };



// Types shared between EditorApp and more than one panel. At namespace scope so a panel
// header can name them in its declarations without including editor_app.h.
struct ClipDocument {
    std::string state;        ///< The Animator state naming it.
    fs::path path;
    ClipModel model;
    UndoStack<ClipModel> undo{200};
    uint64_t revision = 1;    ///< Bumps on every change (the preview re-parses).
    bool open() const { return !path.empty(); }
};

struct EditVisibility {
    static constexpr char kFront = 1, kBehind = 2;
    uint64_t revision = 0;
    glm::mat4 view{0.0f}, proj{0.0f};
    int xray = -1;   ///< Cache key: 0 occlusion, 1 everything shown, 2 X-ray.
    std::vector<char> verts, faces;
    std::map<Edge, char> edges;
};

struct KeyDrag { bool active = false; float start_x = 0.0f; float dt = 0.0f; };

struct LastOp {
    enum class Kind { None, LoopCut, Subdivide, CatmullClark, AddMeshPrimitive, AddObjectPrimitive } kind = Kind::None;
    EditMesh base;
    MeshSelection base_sel;
    uint64_t revision_after = 0;
    Edge seed{0, 0};
    int cuts = 1;
    float factor = 0.0f;             ///< Loop cut: the edge slide applied after it.
    int levels = 1;
    PrimitiveParams prim;
    glm::mat4 prim_xf{1.0f};
    ObjectId object = 0;             ///< Object-mode Add: the object created.
    uint64_t doc_revision_after = 0;
};

struct Lookdev {
    int shape = 0;            ///< 0 shader ball, 1 sphere, 2 rounded cube, 3 plane, 4 cylinder
    bool turntable = false;
    bool ground = true;
    float angle = 0.0f;
    uint64_t revision = 1;    ///< bumped when the shape changes
};

struct LoopCutTool {
    bool active = false;
    int cuts = 1;
    std::optional<Edge> seed;
    uint64_t revision = 0;
    int preview_cuts = 0;
    std::vector<std::vector<glm::vec3>> lines;   ///< mesh-local polylines
};

struct PaintStroke {
    bool active = false;
    PaintTool tool = PaintTool::Draw;
    bool invert = false;
    float radius = 0.1f;   ///< mesh-local
    glm::vec2 last_px{0.0f};
};

struct SculptResize { bool active = false; bool strength = false; glm::vec2 start{0.0f}; float start_value = 0.0f; };

struct Stroke {
    bool active = false;
    SculptBrush brush = SculptBrush::Draw;
    bool invert = false;
    float radius = 0.1f;          ///< mesh-local
    glm::vec2 last_px{0.0f};
    // Grab
    std::vector<SculptGrab> grabs;
    glm::vec3 plane_point{0.0f}, plane_normal{0, 0, 1};   ///< world
    glm::vec3 start_world{0.0f};
};

struct TimelineView { float start = 0.0f, end = 0.0f; };

/** @brief Asset-panel ordering. */
enum class AssetSort { Name, Modified, Tags, Size };

/** @brief An asset file's mtime and size, for the Asset panel's sorting. */
struct AssetStat { int64_t modified = 0; uintmax_t size = 0; };

} // namespace toy::editor

#endif // TOYEDITOR_APP_EDITOR_TYPES_H
