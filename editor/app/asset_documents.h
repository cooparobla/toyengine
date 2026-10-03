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

/** @brief One mesh file being edited. */
struct MeshDocument {
    std::filesystem::path path;   ///< Empty until first saved.
    std::string name = "mesh";
    EditMesh mesh;
    MeshSelection selection;
    UndoStack<EditMesh> undo;
    uint64_t saved_revision = 0;
    uint64_t geometry_revision = 1;   ///< Bumps on every change (drives preview uploads).

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
    }
    /** @brief Applies `fn` as one undoable step. */
    void edit(const std::string& label, const std::function<void(EditMesh&, MeshSelection&)>& fn,
              const std::string& merge_key = {}) {
        EditMesh before = mesh;
        fn(mesh, selection);
        selection.validate(mesh);
        undo.push(label, std::move(before), mesh, merge_key);
        ++geometry_revision;
    }
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
