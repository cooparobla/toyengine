#include "editor/app/asset_documents.h"

namespace toy {
namespace editor {

size_t edit_mesh_bytes(const EditMesh& m) {
    size_t b = sizeof(EditMesh) + m.positions.size() * sizeof(glm::vec3) + m.faces.size() * sizeof(Face);
    for (const Face& f : m.faces) b += f.corners.size() * sizeof(Corner);
    for (const auto& w : m.weights) b += sizeof(w) + w.size() * sizeof(VertexWeight);
    b += m.joints.size() * sizeof(glm::ivec4) + m.joint_weights.size() * sizeof(glm::vec4);
    return b;
}

void MeshDocument::load(const std::filesystem::path& p) {
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

void MeshDocument::reset(EditMesh m, const std::string& n) {
    mesh = std::move(m);
    name = n;
    path.clear();
    selection = {};
    undo.clear();
    saved_revision = undo.revision() - 1;   // unsaved: dirty from the start
    ++geometry_revision;
}

void MeshDocument::save(const std::filesystem::path& p) {
    if (!p.empty()) path = p;
    coopa::yaml::save_document(path, mesh_to_node(mesh));
    saved_revision = undo.revision();
    std::error_code ec;
    file_time = std::filesystem::last_write_time(path, ec);
}

void MeshDocument::edit(const std::string& label, const std::function<void(EditMesh&, MeshSelection&)>& fn,
          const std::string& merge_key) {
    EditMesh before = mesh;
    fn(mesh, selection);
    mesh.sync_vertex_data();   // any vertex an operation added without data joins no group
    selection.validate(mesh);
    undo.push(label, std::move(before), mesh, merge_key);
    ++geometry_revision;
}

void MeshDocument::begin_live() { live_before_ = mesh; live_ = true; }

void MeshDocument::end_live(const std::string& label) {
    if (!live_) return;
    live_ = false;
    if (live_before_ == mesh) { live_before_ = {}; return; }
    undo.push(label, std::move(live_before_), mesh, {});
    live_before_ = {};
    ++geometry_revision;
    ++position_revision;
}

void MeshDocument::do_undo() { if (const EditMesh* m = undo.undo()) { mesh = *m; selection.validate(mesh); ++geometry_revision; } }

void MeshDocument::do_redo() { if (const EditMesh* m = undo.redo()) { mesh = *m; selection.validate(mesh); ++geometry_revision; } }

void MaterialDocument::load(const std::filesystem::path& p, const std::string& reference) {
    node = coopa::yaml::load_document(coopa::yaml::resolve_variant(p));
    if (!node.is_mapping()) node = Node::mapping();
    path = p;
    ref = reference;
    undo.clear();
    saved_revision = undo.revision();
    ++revision;
}

void MaterialDocument::save() {
    coopa::yaml::save_document(path, node);
    saved_revision = undo.revision();
}

void MaterialDocument::commit(const std::string& label, const Node& before, const std::string& merge_key) {
    undo.push(label, before, node, merge_key);
    ++revision;
}

void MaterialDocument::do_undo() { if (const Node* n = undo.undo()) { node = *n; ++revision; } }

void MaterialDocument::do_redo() { if (const Node* n = undo.redo()) { node = *n; ++revision; } }

void ConfigDocument::load(const std::filesystem::path& p) {
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

void ConfigDocument::save() {
    coopa::yaml::save_document(path, node);
    saved_revision = undo.revision();
}

void ConfigDocument::commit(const std::string& label, const Node& before, const std::string& merge_key) {
    undo.push(label, before, node, merge_key);
    ++revision;
}

void ConfigDocument::do_undo() { if (const Node* n = undo.undo()) { node = *n; ++revision; } }

void ConfigDocument::do_redo() { if (const Node* n = undo.redo()) { node = *n; ++revision; } }

} // namespace editor
} // namespace toy
