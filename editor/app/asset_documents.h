/**
 * @file asset_documents.h
 * @brief The non-scene documents the editor edits: meshes, material assets and config.yaml.
 *
 * Each pairs the file's value with a snapshot undo stack and a saved-revision marker, the
 * same model as SceneDocument.
 */

#ifndef TOYEDITOR_APP_ASSET_DOCUMENTS_H
#define TOYEDITOR_APP_ASSET_DOCUMENTS_H

#include "../core/undo.h"
#include "../core/yaml_util.h"
#include "../mesh/edit_mesh.h"
#include "../mesh/mesh_ops.h"

#include <coopa/yaml/document.h>
#include <coopa/yaml/writer.h>

#include <filesystem>
#include <functional>
#include <set>
#include <string>

namespace toy::editor {

/** @brief Rough heap size of an EditMesh snapshot, for the undo history's memory budget. */
inline size_t edit_mesh_bytes(const EditMesh& m) {
    size_t b = sizeof(EditMesh) + m.positions.size() * sizeof(glm::vec3) + m.faces.size() * sizeof(Face);
    for (const Face& f : m.faces) b += f.corners.size() * sizeof(Corner);
    for (const auto& w : m.weights) b += sizeof(w) + w.size() * sizeof(VertexWeight);
    b += m.joints.size() * sizeof(glm::ivec4) + m.joint_weights.size() * sizeof(glm::vec4);
    return b;
}

/** @brief One mesh file being edited. */
struct MeshDocument {
    /// History: up to 200 steps or ~384 MB of snapshots, whichever binds first (a dense mesh -- a
    /// 100x100 water grid is ~1.5 MB per step -- still keeps well over 100), never under 16.
    static constexpr size_t kUndoBudgetBytes = size_t(384) << 20;

    MeshDocument() { undo.set_budget(kUndoBudgetBytes, edit_mesh_bytes); }

    std::filesystem::path path;   ///< Empty until first saved.
    std::string name = "mesh";
    EditMesh mesh;
    MeshSelection selection;
    UndoStack<EditMesh> undo;
    /// Opened by Edit/Sculpt Mode on a scene object (not as a mesh asset of its own): saved
    /// automatically when the mode is left, and part of the scene's Ctrl+Z timeline.
    bool scene_owned = false;
    /// The file's write time when last loaded or saved -- a parked history (see EditorApp's
    /// park_mesh_doc_()) is only resumed if the file has not changed since.
    std::filesystem::file_time_type file_time{};
    uint64_t saved_revision = 0;
    uint64_t geometry_revision = 1;   ///< Bumps on every change (drives preview uploads).
    EditMesh live_before_;
    bool live_ = false;

    bool open() const { return !mesh.faces.empty() || !path.empty(); }
    bool dirty() const { return undo.revision() != saved_revision; }

    void load(const std::filesystem::path& p) {
        mesh = mesh_from_node(coopa::yaml::load_document(coopa::yaml::resolve_variant(p)));
        path = p;
        name = p.stem().string();
        selection = {};
        undo.clear();
        saved_revision = undo.revision();
        ++geometry_revision;
        std::error_code ec;
        file_time = std::filesystem::last_write_time(coopa::yaml::resolve_variant(p), ec);
    }
    void reset(EditMesh m, const std::string& n) {
        mesh = std::move(m);
        name = n;
        path.clear();
        selection = {};
        undo.clear();
        saved_revision = undo.revision() - 1;   // unsaved: dirty from the start
        ++geometry_revision;
    }
    void save(const std::filesystem::path& p = {}) {
        if (!p.empty()) path = p;
        coopa::yaml::save_document(path, mesh_to_node(mesh));
        saved_revision = undo.revision();
        std::error_code ec;
        file_time = std::filesystem::last_write_time(path, ec);
    }
    /** @brief Applies `fn` as one undoable step. */
    void edit(const std::string& label, const std::function<void(EditMesh&, MeshSelection&)>& fn,
              const std::string& merge_key = {}) {
        EditMesh before = mesh;
        fn(mesh, selection);
        mesh.sync_vertex_data();   // any vertex an operation added without data joins no group
        selection.validate(mesh);
        undo.push(label, std::move(before), mesh, merge_key);
        ++geometry_revision;
    }
    // --- live edits (sculpt strokes): positions change every frame, one undo step at the end ---

    uint64_t position_revision = 1;   ///< Bumps on live edits (and every geometry change).
    /** @brief Starts a live edit: one copy of the mesh for undo, none per frame. */
    void begin_live() { live_before_ = mesh; live_ = true; }
    /** @brief Positions changed during the live edit (no undo entry, no topology change). */
    void touch_live() { ++position_revision; }
    /** @brief Ends it as one undo step (skipped if nothing changed). */
    void end_live(const std::string& label) {
        if (!live_) return;
        live_ = false;
        if (live_before_ == mesh) { live_before_ = {}; return; }
        undo.push(label, std::move(live_before_), mesh, {});
        live_before_ = {};
        ++geometry_revision;
        ++position_revision;
    }
    bool live() const { return live_; }

    void do_undo() { if (const EditMesh* m = undo.undo()) { mesh = *m; selection.validate(mesh); ++geometry_revision; } }
    void do_redo() { if (const EditMesh* m = undo.redo()) { mesh = *m; selection.validate(mesh); ++geometry_revision; } }
};

/** @brief One materials/*.yaml asset being edited. */
struct MaterialDocument {
    std::filesystem::path path;
    std::string ref;        ///< "materials/brick" -- how renderers reference it.
    Node node = Node::mapping();
    UndoStack<Node> undo;
    uint64_t saved_revision = 0;
    uint64_t revision = 1;

    bool open() const { return !path.empty(); }
    bool dirty() const { return undo.revision() != saved_revision; }

    void load(const std::filesystem::path& p, const std::string& reference) {
        node = coopa::yaml::load_document(coopa::yaml::resolve_variant(p));
        if (!node.is_mapping()) node = Node::mapping();
        path = p;
        ref = reference;
        undo.clear();
        saved_revision = undo.revision();
        ++revision;
    }
    void save() {
        coopa::yaml::save_document(path, node);
        saved_revision = undo.revision();
    }
    void commit(const std::string& label, const Node& before, const std::string& merge_key) {
        undo.push(label, before, node, merge_key);
        ++revision;
    }
    void do_undo() { if (const Node* n = undo.undo()) { node = *n; ++revision; } }
    void do_redo() { if (const Node* n = undo.redo()) { node = *n; ++revision; } }
};

/**
 * @brief One ui/themes/*.yaml game UI theme being edited -- the same shape as a material: a
 *        raw document with undo, saved on request. `ref` is "ui/themes/<name>".
 */
using ThemeDocument = MaterialDocument;

/**
 * @brief config.yaml, edited as its raw document.
 *
 * Only keys the user touches are written (plus the ones already in the file): config
 * precedence is defaults < quality preset < explicit key, so writing every field would pin
 * the preset-covered ones forever. "Reset" removes a key, handing it back to the preset.
 */
struct ConfigDocument {
    std::filesystem::path path;
    Node node = Node::mapping();
    UndoStack<Node> undo;
    uint64_t saved_revision = 0;
    uint64_t revision = 1;

    bool dirty() const { return undo.revision() != saved_revision; }

    void load(const std::filesystem::path& p) {
        path = p;
        try {
            node = coopa::yaml::load_document(coopa::yaml::resolve_variant(p));
        } catch (...) {
            node = Node::mapping();
        }
        if (!node.is_mapping()) node = Node::mapping();
        undo.clear();
        saved_revision = undo.revision();
        ++revision;
    }
    void save() {
        coopa::yaml::save_document(path, node);
        saved_revision = undo.revision();
    }
    Node& section(const std::string& key) { return ensure_map(node, key); }
    void commit(const std::string& label, const Node& before, const std::string& merge_key) {
        undo.push(label, before, node, merge_key);
        ++revision;
    }
    void do_undo() { if (const Node* n = undo.undo()) { node = *n; ++revision; } }
    void do_redo() { if (const Node* n = undo.redo()) { node = *n; ++revision; } }
};

} // namespace toy::editor

#endif // TOYEDITOR_APP_ASSET_DOCUMENTS_H
