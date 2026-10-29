/**
 * @file node_graph.cpp
 * @brief The node-graph workspace.
 */

#include "editor/app/ui/node_graph.h"

#include <algorithm>
#include <cmath>
#include <fstream>

#include <coopa/nodegraph/graph_yaml.h>
#include <coopa/nodegraph/node_type.h>
#include <coopa/nodegraph/type_registry.h>
#include <toyengine/procgraph/nodes.h>
#include <toyengine/procgraph/procedural_graph.h>
#include <toyengine/procgraph/procedural_graph_system.h>

#include "editor/app/editor_app.h"
#include "editor/node/node_wire.h"

namespace toy {
namespace editor {
namespace {

using imm::Box;

/// @brief A blank graph: one GroupOutput, so a new file has somewhere to wire to.
const char* k_blank_graph = R"(name: new_graph
outputs:
  - id: object
    type: ObjectSpec
nodes:
  - id: 1
    type: GroupOutput
    pos: [0, 0]
links: []
)";

} // namespace

// ---------------------------------------------------------------------------------------------
// Document
// ---------------------------------------------------------------------------------------------

bool NodeGraphPanel::open() const { return app_.graph_doc_.open(); }
bool NodeGraphPanel::dirty() const { return app_.graph_doc_.open() && app_.graph_doc_.dirty(); }
std::filesystem::path NodeGraphPanel::path() const { return app_.graph_doc_.path; }

bool NodeGraphPanel::open_graph(const std::filesystem::path& p) {
    // Every node type must be registered before a graph can be laid out: a node's sockets come
    // from its type's declare(), not from the file.
    procgraph::register_procgraph_nodes();

    ng::Diagnostics diag;
    ng::Graph loaded = ng::load_graph(p.string(), diag);
    if (!diag.ok()) {
        app_.log_error(app_.project_.relative(p) + " is not a node graph (" + diag.to_string() + ")");
        return false;
    }

    app_.graph_doc_.load(p, app_.project_.relative(p));
    graph_ = std::move(loaded);
    selection_.clear();
    want_frame_all_ = true;
    revalidate_();
    // The row in the asset browser highlights off this, and Rename/Delete gate on it.
    app_.active_path_ = p;
    return true;
}

bool NodeGraphPanel::new_graph(const std::string& name) {
    const std::filesystem::path dir = app_.project_.assets() / "graphs";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    std::filesystem::path p = dir / (name + ".yaml");
    for (int i = 2; std::filesystem::exists(p, ec); ++i) {
        p = dir / (name + "_" + std::to_string(i) + ".yaml");
    }
    { std::ofstream(p) << k_blank_graph; }
    app_.project_.refresh();
    return open_graph(p);
}

void NodeGraphPanel::save() {
    if (!open()) return;
    // Written through graph_to_node so the canvas and the file agree about key order; the
    // document then owns the atomic write and the saved-revision bookkeeping.
    app_.graph_doc_.node = ng::graph_to_node(graph_);
    app_.graph_doc_.save();

    // Authoring loop: a saved graph re-bakes every component that references it, so the scene
    // updates while the author is still looking at the canvas.
    if (app_.engine_.has_scene()) {
        if (auto* sys = procgraph::find_procgraph(app_.engine_.scene())) sys->rebake_all();
    }
}

void NodeGraphPanel::refresh_from_document() {
    if (!open()) return;
    ng::Diagnostics diag;
    graph_ = ng::graph_from_node(app_.graph_doc_.node, diag);
    selection_.clear();
    revalidate_();
}

void NodeGraphPanel::commit_(const node::NodeGraphEdit& edit) {
    if (!edit.changed) return;
    const fkyaml::node before = app_.graph_doc_.node;
    app_.graph_doc_.node = ng::graph_to_node(graph_);
    // `active` means a drag is still running, so every frame merges into one entry under the same
    // key rather than filling the undo stack with one step per mouse move.
    app_.graph_doc_.commit(edit.key, before, edit.active ? edit.key : std::string());
    revalidate_();
}

void NodeGraphPanel::revalidate_() {
    node_errors_.clear();
    graph_error_.clear();
    ng::Diagnostics diag;
    graph_.validate(diag);
    for (const auto& e : diag.entries()) {
        if (e.node == 0) {
            if (!graph_error_.empty()) graph_error_ += "\n";
            graph_error_ += e.message;
        } else {
            std::string& slot = node_errors_[e.node];
            if (!slot.empty()) slot += "\n";
            slot += e.message;
        }
    }
}

// ---------------------------------------------------------------------------------------------
// Selection
// ---------------------------------------------------------------------------------------------

bool NodeGraphPanel::selected_(ng::NodeId id) const {
    return std::find(selection_.begin(), selection_.end(), id) != selection_.end();
}

void NodeGraphPanel::select_only_(ng::NodeId id) {
    selection_.clear();
    if (id != 0) selection_.push_back(id);
}

void NodeGraphPanel::toggle_select_(ng::NodeId id) {
    auto it = std::find(selection_.begin(), selection_.end(), id);
    if (it == selection_.end()) selection_.push_back(id);
    else selection_.erase(it);
}

// ---------------------------------------------------------------------------------------------
// Colours
// ---------------------------------------------------------------------------------------------

glm::vec4 NodeGraphPanel::socket_color_(ng::TypeId type) const {
    const auto& t = app_.editor_theme().nodegraph;
    // Resolved by NAME through the registry rather than by a stored id, because ids are assigned
    // in registration order and a consumer that registers a different set would shift them.
    static const struct {
        const char* name;
        glm::vec4 EditorTheme::NodeGraph::*field;
    } table[] = {
        {"Float", &EditorTheme::NodeGraph::socket_float},
        {"Int", &EditorTheme::NodeGraph::socket_int},
        {"Bool", &EditorTheme::NodeGraph::socket_bool},
        {"Vector", &EditorTheme::NodeGraph::socket_vector},
        {"Rotation", &EditorTheme::NodeGraph::socket_vector},
        {"Color", &EditorTheme::NodeGraph::socket_color},
        {"String", &EditorTheme::NodeGraph::socket_string},
        {"Geometry", &EditorTheme::NodeGraph::socket_geometry},
        {"Object", &EditorTheme::NodeGraph::socket_object},
        {"ObjectSet", &EditorTheme::NodeGraph::socket_object},
        {"ObjectSpec", &EditorTheme::NodeGraph::socket_object},
        {"Component", &EditorTheme::NodeGraph::socket_color},
    };
    const std::string name = ng::TypeRegistry::instance().name_of(type);
    for (const auto& row : table) {
        if (name == row.name) return t.*(row.field);
    }
    return t.socket_default;   // An unregistered or unknown type reads as grey, which is honest.
}

// ---------------------------------------------------------------------------------------------
// Layout and drawing
// ---------------------------------------------------------------------------------------------

void NodeGraphPanel::relayout_(imm::Context& ctx) {
    // Measured at zoom 1 and scaled at draw time, so a node is the same size in the document
    // however far in the canvas is zoomed.
    const node::TextMeasure measure = [&ctx](const std::string& s) {
        return ctx.text_width(s);
    };
    layouts_ = node::layout_graph(graph_, measure, metrics_);
}

void NodeGraphPanel::draw_grid_(imm::Context& ctx, const Box& area) {
    const auto& t = app_.editor_theme().nodegraph;
    // Spacing doubles until the lines are at least 8 px apart, so zooming out fades the grid
    // rather than turning it into a solid block -- the trick the timeline's ruler uses.
    float step = 32.0f;
    while (step * view_.zoom < 8.0f) step *= 4.0f;

    const glm::vec2 lo = view_.to_graph(area, {area.x, area.y});
    const glm::vec2 hi = view_.to_graph(area, {area.right(), area.bottom()});
    const float x0 = std::floor(lo.x / step) * step;
    const float y0 = std::floor(lo.y / step) * step;

    for (float x = x0; x <= hi.x; x += step) {
        const float sx = view_.to_screen(area, {x, 0.0f}).x;
        const bool major = std::fabs(std::fmod(x, step * 4.0f)) < 0.01f;
        ctx.line({sx, area.y}, {sx, area.bottom()}, major ? t.grid_major : t.grid, 1.0f);
    }
    for (float y = y0; y <= hi.y; y += step) {
        const float sy = view_.to_screen(area, {0.0f, y}).y;
        const bool major = std::fabs(std::fmod(y, step * 4.0f)) < 0.01f;
        ctx.line({area.x, sy}, {area.right(), sy}, major ? t.grid_major : t.grid, 1.0f);
    }
}

void NodeGraphPanel::draw_wires_(imm::Context& ctx, const Box& area) {
    const auto& t = app_.editor_theme().nodegraph;
    const float thickness = std::clamp(2.5f * view_.zoom, 1.0f, 4.0f);

    for (const auto& link : graph_.links()) {
        glm::vec2 from{}, to{};
        if (!node::link_endpoints(link, layouts_, from, to)) continue;   // socket gone; nothing to draw
        const bool lit = selected_(link.from.node) || selected_(link.to.node);
        // Sampled in GRAPH space and transformed, so the curve's shape is the one hit_link()
        // tests against -- picking and drawing cannot drift apart.
        std::vector<glm::vec2> pts = node::wire_points(from, to);
        for (auto& p : pts) p = view_.to_screen(area, p);
        ctx.polyline(pts, lit ? t.wire_selected : t.wire, thickness);
    }

    if (wire_.active) {
        const node::NodeLayout* l = nullptr;
        for (const auto& candidate : layouts_) {
            if (candidate.id == wire_.node) l = &candidate;
        }
        if (l != nullptr) {
            const node::SocketLayout* s = l->find_socket(wire_.socket, wire_.from_input);
            if (s != nullptr) {
                const glm::vec2 anchor = view_.to_screen(area, s->pos);
                const glm::vec2 cursor = ctx.mouse();
                // The verdict colours the wire while it is being dragged, so an illegal drop is
                // visible before the mouse is released rather than silently doing nothing.
                const node::Hit over = node::hit_socket(
                    layouts_, view_.to_graph(area, cursor), metrics_.socket_r * 2.5f);
                glm::vec4 col = t.wire_dragging;
                if (over.kind == node::HitKind::Socket) {
                    const ng::SocketRef a{wire_.node, wire_.socket};
                    const ng::SocketRef b{over.node, over.socket};
                    const auto verdict = wire_.from_input ? node::can_connect(graph_, b, a)
                                                          : node::can_connect(graph_, a, b);
                    col = verdict == node::ConnectVerdict::Ok ? t.wire_dragging : t.wire_invalid;
                }
                const float d = std::clamp(std::fabs(cursor.x - anchor.x) * 0.5f, 30.0f, 200.0f);
                const float dir = wire_.from_input ? -1.0f : 1.0f;
                ctx.bezier(anchor, anchor + glm::vec2(d * dir, 0.0f),
                           cursor - glm::vec2(d * dir, 0.0f), cursor, col, thickness);
            }
        }
    }
}

void NodeGraphPanel::draw_nodes_(imm::Context& ctx, const Box& area) {
    const auto& t = app_.editor_theme().nodegraph;
    const float round = std::max(2.0f, 4.0f * view_.zoom);
    // Below this the labels are unreadable and the sockets unclickable, so the canvas draws
    // header-only blocks: a large graph stays legible as a shape, and cheap to draw.
    const bool detail = view_.zoom > 0.4f;
    const float font = std::clamp(11.0f * view_.zoom, 7.0f, 15.0f);

    for (const auto& l : layouts_) {
        const Box body = view_.box_of(area, l.lo, l.hi);
        if (body.right() < area.x || body.x > area.right() ||
            body.bottom() < area.y || body.y > area.bottom()) {
            continue;   // Off screen: a thousand-node graph should cost what is visible.
        }

        const Box header = view_.box_of(area, l.header_lo(), l.header_hi(metrics_));
        ctx.fill_rounded(body, t.node_bg, round);
        ctx.fill_rounded(header, t.node_header, round, imm::Context::kTop);

        const auto err = node_errors_.find(l.id);
        const bool bad = err != node_errors_.end();
        const glm::vec4 edge = selected_(l.id) ? t.node_selected
                             : bad             ? t.node_error
                                               : t.node_outline;
        ctx.outline_rounded(body, edge, round, selected_(l.id) || bad ? 2.0f : 1.0f);

        if (!detail) continue;

        const ng::Node* n = graph_.node(l.id);
        const ng::NodeType* type = n ? ng::NodeTypeRegistry::instance().find(n->type) : nullptr;
        const std::string title = type ? type->display() : (n ? n->type : std::string("?"));
        ctx.draw_text({header.x + 6.0f, header.y + header.h * 0.5f - font * 0.6f}, title,
                      t.node_text, font);

        for (const auto& s : l.sockets) {
            const glm::vec2 p = view_.to_screen(area, s.pos);
            const float r = std::max(2.0f, metrics_.socket_r * view_.zoom);
            // A pill rather than circle()+ring(): a filled disc plus an outline runs ~36
            // triangles per socket, and a busy graph has hundreds.
            ctx.fill_rounded({p.x - r, p.y - r, r * 2.0f, r * 2.0f}, socket_color_(s.type), r);
            if (s.multi_input) ctx.outline_rounded({p.x - r, p.y - r, r * 2.0f, r * 2.0f},
                                                   t.node_text, r, 1.0f);
            const float label_x = s.is_input ? p.x + r + 4.0f : p.x - r - 4.0f - ctx.text_width(s.identifier);
            ctx.draw_text({label_x, p.y - font * 0.6f}, s.identifier, t.node_text, font);
        }

        if (bad) ctx.tooltip(err->second);
    }
}

void NodeGraphPanel::draw_header_(imm::Context& ctx, const Box& b) {
    const std::string name = open() ? std::filesystem::path(app_.graph_doc_.ref).filename().string()
                                    : std::string("No graph open");
    ctx.draw_text({b.x + 10.0f, b.y + b.h * 0.5f - 7.0f}, name + (dirty() ? " *" : ""),
                  ctx.style.text, 13.0f);
    if (!graph_error_.empty()) {
        ctx.draw_text({b.x + 10.0f + ctx.text_width(name) + 24.0f, b.y + b.h * 0.5f - 7.0f},
                      graph_error_, app_.editor_theme().nodegraph.node_error, 11.0f);
    }
}

void NodeGraphPanel::draw_graph_view_(imm::Context& ctx, const Box& area) {
    const Box hb = app_.area_header_(ctx, area);
    draw_header_(ctx, hb);
    // The panel owns viewport_box_ because draw_viewport_ returned before setting it; the
    // statusbar and every shortcut gate read it.
    app_.viewport_box_ = Box{area.x, hb.bottom(), area.w, std::max(1.0f, area.h - hb.h)};
    const Box canvas = app_.viewport_box_;

    ctx.fill(canvas, ctx.style.panel_bg);
    // A plain clip, NOT begin_region(scroll): a scrolling region consumes the wheel and the
    // zoom would silently stop working.
    ctx.push_clip(canvas);

    relayout_(ctx);
    if (want_frame_all_) {
        glm::vec2 lo{0.0f}, hi{0.0f};
        if (node::graph_bounds(layouts_, lo, hi)) view_ = node::fit_view(canvas, lo, hi);
        want_frame_all_ = false;
    }

    draw_grid_(ctx, canvas);
    draw_wires_(ctx, canvas);
    draw_nodes_(ctx, canvas);
    handle_input_(ctx, canvas);

    if (band_.active) {
        const glm::vec2 m = ctx.mouse();
        const Box r{std::min(band_.press.x, m.x), std::min(band_.press.y, m.y),
                    std::fabs(m.x - band_.press.x), std::fabs(m.y - band_.press.y)};
        ctx.fill(r, app_.editor_theme().nodegraph.band_fill);
        ctx.outline(r, app_.editor_theme().nodegraph.band_outline);
    }

    ctx.pop_clip();
    draw_add_popup_(ctx);
}

// ---------------------------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------------------------

void NodeGraphPanel::handle_input_(imm::Context& ctx, const Box& area) {
    const imm::FrameInput& in = ctx.input();
    const glm::vec2 m = ctx.mouse();
    const bool over = ctx.is_hovered(area) && !ctx.any_popup_open();
    const glm::vec2 mg = view_.to_graph(area, m);

    // --- view ---
    if (over && in.scroll.y != 0.0f) view_.zoom_at(area, m, in.scroll.y > 0 ? 1.1f : 1.0f / 1.1f);
    if (over && in.pressed[2]) panning_ = true;
    if (!in.down[2]) panning_ = false;
    if (panning_) view_.pan_by(in.mouse_delta);

    // Tolerances divided by zoom, so a socket stays the same size in SCREEN pixels however far
    // out the canvas is -- a dot that got harder to hit as you zoomed out would be wrong.
    const float socket_tol = (metrics_.socket_r * 2.5f) / std::max(view_.zoom, 0.01f);
    const float link_tol   = 6.0f / std::max(view_.zoom, 0.01f);

    // --- wire drag: released ---
    if (wire_.active && in.released[0]) {
        const node::Hit drop = node::hit_socket(layouts_, mg, socket_tol);
        if (drop.kind == node::HitKind::Socket) {
            const ng::SocketRef a{wire_.node, wire_.socket};
            const ng::SocketRef b{drop.node, drop.socket};
            commit_(wire_.from_input ? node::connect(graph_, b, a) : node::connect(graph_, a, b));
        }
        wire_ = {};
        return;
    }

    // --- node drag: in progress ---
    if (drag_.active) {
        if (in.down[0]) {
            const glm::vec2 delta = mg - drag_.last;
            if (delta != glm::vec2(0.0f)) {
                drag_.moved = true;
                commit_(node::move_nodes(graph_, selection_, delta, /*finished=*/false));
                relayout_(ctx);
                drag_.last = mg;
            }
        } else {
            // One closing commit, so the whole drag collapses into a single undo entry.
            if (drag_.moved) commit_(node::move_nodes(graph_, selection_, {}, /*finished=*/true));
            drag_ = {};
        }
        return;
    }

    // --- rubber band ---
    if (band_.pending && in.down[0] && glm::distance(m, band_.press) > 5.0f) band_.active = true;
    if (band_.pending && in.released[0]) {
        if (band_.active) {
            const glm::vec2 a = view_.to_graph(area, band_.press);
            const std::vector<ng::NodeId> hits = node::nodes_in_box(layouts_, a, mg);
            if (!has(in.mods, coopa::input::Mods::Shift)) selection_.clear();
            for (ng::NodeId id : hits) {
                if (!selected_(id)) selection_.push_back(id);
            }
        } else if (!has(in.mods, coopa::input::Mods::Shift)) {
            selection_.clear();
        }
        band_ = {};
    }
    if (!in.down[0] && !in.released[0]) band_ = {};

    if (!over) return;

    // --- press ---
    if (in.pressed[0]) {
        const node::Hit hit = node::hit_test(graph_, layouts_, metrics_, mg, socket_tol, link_tol);
        switch (hit.kind) {
            case node::HitKind::Socket:
                wire_.active     = true;
                wire_.node       = hit.node;
                wire_.socket     = hit.socket;
                wire_.from_input = hit.is_input;
                break;
            case node::HitKind::Header:
            case node::HitKind::Node:
                if (has(in.mods, coopa::input::Mods::Shift)) toggle_select_(hit.node);
                else if (!selected_(hit.node)) select_only_(hit.node);
                // Only the header drags, so clicking a node's body can later hit a widget on it.
                if (hit.kind == node::HitKind::Header) {
                    drag_.active = true;
                    drag_.moved  = false;
                    drag_.last   = mg;
                }
                break;
            case node::HitKind::Link:
                if (has(in.mods, coopa::input::Mods::Alt)) commit_(node::disconnect(graph_, hit.link));
                break;
            case node::HitKind::None:
                band_.pending = true;
                band_.press   = m;
                break;
        }
    }

    // --- keys and menus ---
    if (in.pressed[1]) {
        add_at_ = mg;
        ctx.open_popup("graph_add", m);
    }
    if (ctx.key_pressed(coopa::input::Key::X) || ctx.key_pressed(coopa::input::Key::Delete)) {
        delete_selection();
    }
    if (ctx.shortcut(coopa::input::Key::D, coopa::input::Mods::Super)) duplicate_selection();
    if (ctx.key_pressed(coopa::input::Key::A) && has(in.mods, coopa::input::Mods::Shift)) {
        add_at_ = mg;
        ctx.open_popup("graph_add", m);
    }
}

void NodeGraphPanel::draw_add_popup_(imm::Context& ctx) {
    if (!ctx.begin_popup("graph_add", 220.0f)) return;

    ctx.input_text("Search", &palette_filter_);
    std::string needle = palette_filter_;
    std::transform(needle.begin(), needle.end(), needle.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    // Straight from the registry, so a graph kind that registers other node types gets its own
    // palette with no code here.
    for (const std::string& category : ng::NodeTypeRegistry::instance().categories()) {
        std::vector<const ng::NodeType*> shown;
        for (const ng::NodeType* t : ng::NodeTypeRegistry::instance().by_category(category)) {
            std::string name = t->display();
            std::transform(name.begin(), name.end(), name.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (needle.empty() || name.find(needle) != std::string::npos) shown.push_back(t);
        }
        if (shown.empty()) continue;
        ctx.label(category);
        for (const ng::NodeType* t : shown) {
            if (!ctx.selectable(t->display(), false)) continue;
            ng::NodeId id = 0;
            commit_(node::add_node_at(graph_, t->name, add_at_, id));
            select_only_(id);
            ctx.close_current_popup();
            break;
        }
    }
    ctx.end_popup();
}

void NodeGraphPanel::move_selection(glm::vec2 delta) {
    if (selection_.empty() || delta == glm::vec2(0.0f)) return;
    commit_(node::move_nodes(graph_, selection_, delta, /*finished=*/false));
    commit_(node::move_nodes(graph_, selection_, {}, /*finished=*/true));
}

void NodeGraphPanel::delete_selection() {
    if (selection_.empty()) return;
    commit_(node::delete_nodes(graph_, selection_));
    selection_.clear();
}

void NodeGraphPanel::duplicate_selection() {
    if (selection_.empty()) return;
    std::vector<ng::NodeId> made;
    commit_(node::duplicate(graph_, selection_, made));
    selection_ = made;
}

void NodeGraphPanel::frame_selection() {
    if (selection_.empty()) { want_frame_all_ = true; return; }
    glm::vec2 lo{0.0f}, hi{0.0f};
    bool any = false;
    for (const auto& l : layouts_) {
        if (!selected_(l.id)) continue;
        if (!any) { lo = l.lo; hi = l.hi; any = true; }
        else { lo = glm::min(lo, l.lo); hi = glm::max(hi, l.hi); }
    }
    if (!any) { want_frame_all_ = true; return; }
    view_ = node::fit_view(app_.viewport_box_, lo, hi);
}

// ---------------------------------------------------------------------------------------------
// Properties
// ---------------------------------------------------------------------------------------------

void NodeGraphPanel::draw_graph_props_(imm::Context& ctx) {
    if (!open()) { ctx.label("No graph open"); return; }

    ctx.label("Graph");
    std::string name = graph_.name();
    if (ctx.input_text("Name", &name) && name != graph_.name()) {
        const fkyaml::node before = app_.graph_doc_.node;
        graph_.set_name(name);
        app_.graph_doc_.node = ng::graph_to_node(graph_);
        app_.graph_doc_.commit("Rename Graph", before, "graph_name");
    }

    ctx.separator();
    draw_interface_(ctx, /*as_outputs=*/false);
    ctx.separator();
    draw_interface_(ctx, /*as_outputs=*/true);

    if (!graph_error_.empty()) {
        ctx.separator();
        ctx.label(graph_error_);
    }
    ctx.separator();
    ctx.label(std::to_string(graph_.nodes().size()) + " nodes, " +
              std::to_string(graph_.links().size()) + " links");
}

void NodeGraphPanel::draw_node_props_(imm::Context& ctx) {
    if (selection_.size() != 1) {
        ctx.label(selection_.empty() ? "No node selected"
                                     : std::to_string(selection_.size()) + " nodes selected");
        return;
    }
    ng::Node* n = graph_.node(selection_.front());
    if (n == nullptr) { ctx.label("No node selected"); return; }

    const ng::NodeType* type = ng::NodeTypeRegistry::instance().find(n->type);
    ctx.label(type ? type->display() : n->type);
    if (type != nullptr && !type->tooltip.empty()) ctx.label(type->tooltip);

    const auto err = node_errors_.find(n->id);
    if (err != node_errors_.end()) {
        ctx.separator();
        ctx.label(err->second);
    }

    // The node's own settings. There is no schema for a PropertyBag -- each node type hardcodes
    // the keys it reads -- so the keys actually present are enumerated and edited as text. That
    // is honest about what is knowable, and it round-trips unknown keys untouched.
    ctx.separator();
    ctx.label("Settings");
    const fkyaml::node& raw = n->props.raw();
    if (!raw.is_mapping() || raw.size() == 0) {
        ctx.label("  (none)");
    } else {
        for (auto it = raw.begin(); it != raw.end(); ++it) {
            const std::string key = it.key().get_value<std::string>();
            std::string value = fkyaml::node::serialize(it.value());
            while (!value.empty() && (value.back() == '\n' || value.back() == ' ')) value.pop_back();
            if (!ctx.input_text(key, &value)) continue;
            const fkyaml::node before = app_.graph_doc_.node;
            try {
                n->props.raw()[key] = fkyaml::node::deserialize(value);
            } catch (const std::exception&) {
                continue;   // Mid-type the text is briefly unparseable; keep the last good value.
            }
            app_.graph_doc_.node = ng::graph_to_node(graph_);
            app_.graph_doc_.commit("Edit " + key, before, "prop_" + key);
            revalidate_();
        }
    }
}

void NodeGraphPanel::draw_graph_palette_(imm::Context& ctx, const Box& area) {
    ctx.begin_region("graph_palette", area, true);
    ctx.label("Nodes");
    for (const auto& l : layouts_) {
        const ng::Node* n = graph_.node(l.id);
        if (n == nullptr) continue;
        const ng::NodeType* t = ng::NodeTypeRegistry::instance().find(n->type);
        const std::string label = t ? t->display() : n->type;
        if (ctx.selectable(label + "##" + std::to_string(l.id), selected_(l.id))) {
            select_only_(l.id);
            frame_selection();
        }
    }
    ctx.end_region();
}


/**
 * @brief The add/remove/rename rows for one end of the graph's interface.
 *
 * The interface is the graph's signature: these are the sockets a GroupInput hands out, the ones
 * a `ProceduralGraph` component binds, and the ones its inspector builds rows from. Editing one
 * therefore reaches three places at once, which is why every change goes through
 * `node::*_interface_socket()` -- those keep the structural nodes' links in step, and nothing
 * else in the library would notice if they did not.
 *
 * One row per socket, laid out by hand rather than through the usual label/field flow: a grip to
 * drag it up or down, its identifier, its label, its type, and a delete. The row order IS the
 * socket order everywhere downstream, so dragging it is the natural gesture.
 */
void NodeGraphPanel::draw_interface_(imm::Context& ctx, bool as_outputs) {
    using I = imm::Icon;
    const std::vector<ng::InterfaceSocket>& sockets = as_outputs ? graph_.outputs() : graph_.inputs();
    const ng::TypeRegistry& types = ng::TypeRegistry::instance();

    // Every registered socket type, for the type dropdown. Built per draw: a game project can add
    // its own types, and the list is a dozen entries.
    std::vector<std::string> type_names;
    for (std::size_t i = 1; i <= types.type_count(); ++i) {
        type_names.push_back(types.name_of(static_cast<ng::TypeId>(i)));
    }

    const float row_h = ctx.style.row_height;
    const float gap   = ctx.style.spacing;

    // Header: the title, with the add button pinned to the right edge.
    {
        const Box head = ctx.next_box(row_h);
        ctx.text_in(head, as_outputs ? "Outputs" : "Inputs", ctx.style.text, 0.0f);
        const Box add{head.right() - row_h, head.y, row_h, row_h};
        if (ctx.icon_button(as_outputs ? "iface_add_out" : "iface_add_in", I::Plus,
                            as_outputs ? "Add an output" : "Add an input",
                            false, 0.0f, imm::Context::kAll, add, /*framed=*/true)) {
            const ng::TypeId fallback = types.find(as_outputs ? "Geometry" : "Float");
            commit_(node::add_interface_socket(graph_, as_outputs,
                                               fallback != ng::k_invalid_type ? fallback : ng::TypeId(1)));
            return;   // The vector just grew; draw it next frame.
        }
    }

    if (sockets.empty()) {
        ctx.label(as_outputs ? "  No outputs yet." : "  No inputs yet.");
        return;
    }

    const imm::FrameInput& in = ctx.input();
    node::NodeGraphEdit edit;

    for (std::size_t i = 0; i < sockets.size(); ++i) {
        const ng::InterfaceSocket& s = sockets[i];
        const Box row = ctx.next_box(row_h);

        // Widths: fixed grip and delete at the ends, the rest split between id, label and type.
        const float ends  = row_h * 2.0f + gap * 2.0f;
        const float field = std::max(40.0f, (row.w - ends - gap * 2.0f) / 3.0f);
        const Box grip  {row.x, row.y, row_h, row_h};
        const Box id_b  {grip.right() + gap, row.y, field, row_h};
        const Box lab_b {id_b.right() + gap, row.y, field, row_h};
        const Box type_b{lab_b.right() + gap, row.y, field, row_h};
        const Box del_b {row.right() - row_h, row.y, row_h, row_h};

        const bool dragging_this = iface_drag_.active && iface_drag_.as_outputs == as_outputs &&
                                   iface_drag_.index == i;
        ctx.icon_button(as_outputs ? "grip_out" : "grip_in", I::Grip, "Drag to reorder",
                        dragging_this, 0.0f, imm::Context::kAll, grip, /*framed=*/false);
        // The grip is a drag handle, not a button: what matters is the press and the travel
        // since, so it is read from the raw hover/press rather than from a click.
        if (!iface_drag_.active && ctx.is_hovered(grip) && in.pressed[0]) {
            iface_drag_ = {true, as_outputs, i, ctx.mouse().y};
        }

        ctx.push_id(static_cast<int64_t>(i) + (as_outputs ? 10000 : 0));
        std::string id = s.identifier;
        if (ctx.input_text_box("id", id_b, &id, "id")) {
            edit = node::rename_interface_socket(graph_, as_outputs, i, id);
        }
        std::string label = s.label;
        if (ctx.input_text_box("label", lab_b, &label, "label")) {
            (as_outputs ? graph_.outputs() : graph_.inputs())[i].label = label;
            edit.changed = true; edit.finished = true; edit.key = "Label " + s.identifier;
        }
        int type_idx = 0;
        for (std::size_t t = 0; t < type_names.size(); ++t) {
            if (type_names[t] == types.name_of(s.type)) type_idx = static_cast<int>(t);
        }
        if (ctx.combo_box("type", type_b, &type_idx, type_names)) {
            edit = node::retype_interface_socket(
                graph_, as_outputs, i, types.find(type_names[static_cast<std::size_t>(type_idx)]));
        }
        const bool del = ctx.icon_button("del", I::Trash, "Remove this socket", false, 0.0f,
                                         imm::Context::kAll, del_b, /*framed=*/false);
        ctx.pop_id();
        if (del) edit = node::remove_interface_socket(graph_, as_outputs, i);

        // A drag that has travelled a whole row swaps with the neighbour it passed, and the
        // anchor moves with it so a long drag keeps stepping instead of swapping once.
        if (dragging_this && in.down[0]) {
            const float travel = ctx.mouse().y - iface_drag_.anchor_y;
            const int   step   = static_cast<int>(travel / (row_h + gap));
            if (step != 0) {
                const node::NodeGraphEdit moved =
                    node::move_interface_socket(graph_, as_outputs, i, step > 0 ? 1 : -1);
                if (moved.changed) {
                    iface_drag_.index  = step > 0 ? i + 1 : i - 1;
                    iface_drag_.anchor_y += (step > 0 ? 1.0f : -1.0f) * (row_h + gap);
                    edit = moved;
                }
            }
        }

        if (edit.changed) break;   // The vector just moved under us; redraw before touching it.
    }

    if (iface_drag_.active && !in.down[0]) iface_drag_ = {};
    if (edit.changed) commit_(edit);
}

} // namespace editor
} // namespace toy
