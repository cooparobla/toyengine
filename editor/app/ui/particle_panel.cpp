#include "editor/app/editor_app.h"

#include <cmath>
#include <cstdio>
#include <set>

#include <toyengine/particles/terrain_scatter.h>

namespace toy {
namespace editor {

using toy::particles::ParticleSystem;

namespace {

/** @brief The schema's field for `key`, relabelled / re-explained for the panel. */
FieldDesc pfield(const std::string& key, const char* label = nullptr, const char* tip = nullptr) {
    static const ComponentSchema* schema = find_schema("ParticleSystem");
    FieldDesc f;
    if (schema) {
        for (const auto& s : schema->fields) {
            if (s.key == key) { f = s; break; }
        }
    }
    if (f.key.empty()) f.key = key;
    if (label) f.label = label;
    if (tip) f.tooltip = tip;
    return f;
}

std::string str_of(const Node& n, const char* key, const std::string& def) {
    return n.contains(key) ? get_string(n, key, def) : def;
}

float num_of(const Node& n, const char* key, float def) {
    if (!n.contains(key)) return def;
    const Node& v = n.at(key);
    if (v.is_float_number()) return static_cast<float>(v.get_value<double>());
    if (v.is_integer()) return static_cast<float>(v.get_value<int64_t>());
    return def;
}

} // namespace

ParticleSystem* EditorApp::live_particle_system_(ObjectId id) const {
    coopa::scene::SceneObject* live = sync_.live(id);
    return live ? live->get_component<ParticleSystem>() : nullptr;
}

EditResult EditorApp::draw_particle_panel_(imm::Context& ctx, ObjectId id, Node& comp, const InspectorEnv& env) {
    using I = imm::Icon;
    EditResult r;
    std::set<std::string> placed{"type", "id"};
    const glm::vec4 dim = ctx.style.text_dim;

    // Draws the listed fields (in order) and marks them placed.
    auto fields = [&](std::initializer_list<FieldDesc> list) {
        std::vector<FieldDesc> v(list);
        for (const auto& f : v) placed.insert(f.key);
        r.absorb(draw_fields(ctx, v, comp, env, false));
    };
    auto mark = [&](std::initializer_list<const char*> keys) { for (const char* k : keys) placed.insert(k); };
    auto section = [&](const char* title, I icon, const char* what, bool open) {
        const bool o = ctx.collapsing_header(title, open, nullptr, icon);
        if (o && what) ctx.paragraph(what, &dim);
        return o;
    };

    const bool scatter = str_of(comp, "mode", "emitter") == "scatter";
    const std::string shape = str_of(comp, "shape", "cone");
    const std::string render_mode = str_of(comp, "render_mode", "billboard");

    // --- What it is ------------------------------------------------------------------------
    {
        ctx.paragraph(scatter ? "Scatter: places a fixed set of meshes (grass, rocks, trees, pebbles) over a shape once. "
                                "Nothing moves or dies."
                              : "Emitter: particles are born over time, move, change and die (fire, smoke, sparks, rain).",
                      &dim);
        int mode = scatter ? 1 : 0;
        if (ctx.combo("Kind", &mode, {"Emitter (born over time)", "Scatter (placed once)"})) {
            comp["mode"] = Node(std::string(mode == 1 ? "scatter" : "emitter"));
            // A scatter draws meshes: switch the renderer over if it was on sprites.
            if (mode == 1 && render_mode != "mesh") comp["render_mode"] = Node(std::string("mesh"));
            r.changed = true;
            r.finished = true;
            r.key = "mode";
        }
        ctx.tooltip("Kind\nEmitter: a stream of short-lived particles. Scatter: a fixed set of instances placed over the shape.");
        placed.insert("mode");

        // Live status + preview controls.
        if (ParticleSystem* ps = live_particle_system_(id)) {
            char buf[160];
            if (ps->is_terrain_scatter()) {
                const auto* cache = ps->terrain_scatter_if();
                std::snprintf(buf, sizeof(buf), "Live: %zu instances in %zu chunks around the camera",
                              cache ? cache->instance_count() : 0u, cache ? cache->chunks().size() : 0u);
            } else if (scatter) {
                std::snprintf(buf, sizeof(buf), "Live: %u instances placed", ps->particle_count());
            } else {
                std::snprintf(buf, sizeof(buf), "Live: %u particles%s", ps->particle_count(),
                              ps->paused() ? " (paused)" : ps->is_playing() ? "" : " (stopped)");
            }
            ctx.label(buf, &dim);
            if (!scatter) {
                if (ctx.button("Restart", 90, true, I::Restart)) ps->restart();
                ctx.tooltip("Restart\nReplay the effect from the start (preview only; nothing is saved)");
                ctx.same_line();
                if (ctx.button(ps->paused() ? "Resume" : "Pause", 90, true, ps->paused() ? I::Play : I::Pause)) ps->pause(!ps->paused());
            }
        }
    }

    // --- Placement / Shape -----------------------------------------------------------------
    {
        const char* what = scatter ? "Where the instances go."
                                   : "Where particles are born and which way they first move (the shape's +Z is 'up').";
        if (section(scatter ? "Placement" : "Shape", I::Sphere, what, true)) {
            FieldDesc sh = pfield("shape", "Shape");
            if (scatter) {
                sh.options = {"terrain", "mesh", "box", "circle", "sphere", "hemisphere", "point"};
                sh.option_tips = {"Over a Heightmap Terrain, by paint layer and slope (grass, forests, rocks)",
                                  "Over a mesh's surface", "Inside a box", "On a disc", "In a sphere", "In a half sphere", "At one point"};
            } else {
                sh.options = {"cone", "sphere", "hemisphere", "box", "circle", "edge", "point", "mesh"};
                sh.option_tips = {"A fountain / jet: out of a disc, spreading by the angle", "Out of a ball in every direction",
                                  "Out of a dome, upward", "Inside a box, upward", "On a disc, outward", "Along a line",
                                  "From one point", "From a mesh's surface, along its normals"};
            }
            fields({sh});
            if (shape == "terrain") {
                r.absorb(draw_particle_terrain_section_(ctx, id, comp, env));
                mark({"terrain", "count", "radius", "radius_thickness", "angle", "arc", "box", "length", "mesh_path", "emit_from",
                      "distribution", "normal_offset", "random_direction", "shape_offset"});
            } else {
                if (scatter) fields({pfield("count", "Count", "How many instances are placed over the shape")});
                if (shape == "cone") {
                    fields({pfield("radius", "Radius (m)", "Radius of the disc particles are born on"),
                            pfield("angle", "Spread Angle", "How wide the cone opens (degrees from straight up)"),
                            pfield("radius_thickness", "Fill", "0: only on the rim; 1: anywhere on the disc"),
                            pfield("arc", "Arc", "How much of the circle is used (degrees)")});
                } else if (shape == "sphere" || shape == "hemisphere" || shape == "circle") {
                    fields({pfield("radius", "Radius (m)"), pfield("radius_thickness", "Fill", "0: only on the surface; 1: anywhere inside"),
                            pfield("arc", "Arc", "How much of the circle is used (degrees)")});
                } else if (shape == "box") {
                    fields({pfield("box", "Size (m)", "The box's full width, depth and height")});
                } else if (shape == "edge") {
                    fields({pfield("length", "Length (m)")});
                } else if (shape == "mesh") {
                    fields({pfield("mesh_path", "Mesh", "The surface to emit from; empty uses this object's own mesh"),
                            pfield("emit_from", "Emit From"),
                            pfield("distribution", "Distribution", "even: spread evenly with no clumps or bald patches"),
                            pfield("normal_offset", "Lift Off Surface (m)")});
                }
                if (!scatter) fields({pfield("random_direction", "Randomize Direction", "0: follow the shape; 1: any direction")});
                fields({pfield("shape_offset", "Offset (m)", "Moves the shape relative to the object")});
                mark({"terrain"});
            }
            fields({pfield("align_to_normal", "Align to Surface", "Tilt each particle / instance to the surface it came from"),
                    pfield("random_spin", "Random Spin", "Turn each one by a random angle about its up axis")});
        } else {
            mark({"shape", "terrain", "count", "radius", "radius_thickness", "angle", "arc", "box", "length", "mesh_path", "emit_from",
                  "distribution", "normal_offset", "random_direction", "shape_offset", "align_to_normal", "random_spin"});
        }
    }

    if (scatter) {
        // --- Instances ---------------------------------------------------------------------
        if (section("Instances", I::Object, "Size, turn and colour variety of the placed instances.", true)) {
            fields({pfield("start_size", "Size", "Scale of each instance, random between the two numbers"),
                    pfield("start_rotation", "Rotation (deg)", "Random turn, between the two numbers"),
                    pfield("start_color", "Colour"), pfield("start_color_b", "Colour Variation", "Each instance mixes between Colour and this"),
                    pfield("angular_velocity", "Spin Speed (deg/s)", "Keep turning, e.g. pinwheels; 0 for still props")});
        } else {
            mark({"start_size", "start_rotation", "start_color", "start_color_b", "angular_velocity"});
        }
        // Emitter-only settings do nothing here: keep them out of sight.
        mark({"duration", "looping", "prewarm", "start_delay", "play_on_start", "time_scale", "rate", "rate_over_distance", "bursts",
              "max_particles", "start_lifetime", "start_speed", "inherit_velocity", "gravity", "velocity", "force", "drag", "wind_influence", "orbital",
              "radial", "tumble", "noise_strength", "noise_frequency", "noise_scroll", "noise_octaves", "size_over_life",
              "alpha_over_life", "collide", "ground_height", "bounce", "collision_friction", "kill_on_collide", "on_death_collision_only",
              "wrap_box", "wrap_fade", "on_death", "simulation_space", "simulation", "color_over_life"});
    } else {
        // --- Emission ----------------------------------------------------------------------
        if (section("Emission", I::Plus, "How many particles are born, and when.", true)) {
            fields({pfield("rate", "Per Second"), pfield("rate_over_distance", "Per Metre Moved", "Trails: more particles the faster the object moves"),
                    pfield("bursts", "Bursts", "Extra particles all at once at set times (explosions, puffs)"),
                    pfield("max_particles", "Max Alive", "Cap on particles alive at once -- the memory budget")});
        } else {
            mark({"rate", "rate_over_distance", "bursts", "max_particles"});
        }
        // --- Start values ------------------------------------------------------------------
        if (section("Start Values", I::Play, "What each particle starts with. Two numbers mean 'random between'.", true)) {
            fields({pfield("start_lifetime", "Lifetime (s)"), pfield("start_speed", "Speed (m/s)"), pfield("start_size", "Size (m)"),
                    pfield("start_rotation", "Rotation (deg)"), pfield("start_color", "Colour"),
                    pfield("start_color_b", "Colour Variation", "Each particle mixes between Colour and this"),
                    pfield("inherit_velocity", "Inherit Motion", "How much of the object's own speed particles take with them")});
        } else {
            mark({"start_lifetime", "start_speed", "start_size", "start_rotation", "start_color", "start_color_b", "inherit_velocity"});
        }
        // --- Timing ------------------------------------------------------------------------
        if (section("Timing", I::Restart, "How long one cycle runs and when it starts.", false)) {
            fields({pfield("duration", "Duration (s)"), pfield("looping", "Loop"),
                    pfield("prewarm", "Prewarm", "Start already full, as if it had been running (ambient smoke, mist)"),
                    pfield("start_delay", "Start Delay (s)"), pfield("play_on_start", "Play on Start"),
                    pfield("time_scale", "Speed", "Plays the whole effect faster or slower")});
        } else {
            mark({"duration", "looping", "prewarm", "start_delay", "play_on_start", "time_scale"});
        }
        // --- Motion --------------------------------------------------------------------------
        if (section("Motion & Forces", I::Move, "How particles move after they are born.", false)) {
            fields({pfield("gravity", "Gravity", "1 falls like a stone, 0 floats, negative rises (hot smoke)"),
                    pfield("drag", "Drag", "Slows particles down over time"),
                    pfield("wind_influence", "Weather Wind",
                           "How much the World tab's wind (and its gusts) blows the particles: 0 ignores it, 1 carries them at the "
                           "full wind speed. Smoke ~0.6, embers ~0.8, sparks ~0.3"),
                    pfield("velocity", "Constant Velocity (m/s)"),
                    pfield("force", "Extra Force (m/s per s)", "A constant push in a fixed direction (buoyancy, a fan)"), pfield("orbital", "Swirl (rad/s)", "Spin around the object's up axis"),
                    pfield("radial", "Push Out (m/s)", "Positive spreads away from the axis, negative pulls in"),
                    pfield("tumble", "Tumble (deg/s)"), pfield("angular_velocity", "Spin (deg/s)")});
        } else {
            mark({"gravity", "drag", "wind_influence", "velocity", "force", "orbital", "radial", "tumble", "angular_velocity"});
        }
        if (section("Turbulence", I::Dots, "Swirling, smoke-like wandering.", false)) {
            fields({pfield("noise_strength", "Strength"), pfield("noise_frequency", "Scale", "Higher: smaller, busier swirls"),
                    pfield("noise_scroll", "Change Speed"), pfield("noise_octaves", "Detail")});
        } else {
            mark({"noise_strength", "noise_frequency", "noise_scroll", "noise_octaves"});
        }
        if (section("Over Lifetime", I::Palette, "How colour, size and fade change from birth (t = 0) to death (t = 1).", false)) {
            fields({pfield("color_over_life", "Colour"), pfield("size_over_life", "Size"), pfield("alpha_over_life", "Fade")});
        } else {
            mark({"color_over_life", "size_over_life", "alpha_over_life"});
        }
        if (section("Collision", I::Collider, "Bounce off (or die on) a flat ground.", false)) {
            fields({pfield("collide", "Collide with Ground")});
            if (comp.contains("collide") && comp.at("collide").is_boolean() && comp.at("collide").get_value<bool>()) {
                fields({pfield("ground_height", "Ground Height (m)"), pfield("bounce", "Bounce"),
                        pfield("collision_friction", "Friction"), pfield("kill_on_collide", "Die on Impact")});
            }
            mark({"ground_height", "bounce", "collision_friction", "kill_on_collide"});
        } else {
            mark({"collide", "ground_height", "bounce", "collision_friction", "kill_on_collide"});
        }
        if (section("Sub Emitters", I::Link, "Spawn another particle system where particles die (splashes, sparks).", false)) {
            fields({pfield("on_death", "On Death"), pfield("on_death_collision_only", "Only When Hitting Ground")});
        } else {
            mark({"on_death", "on_death_collision_only"});
        }
        if (section("Follow Camera (Wrap)", I::Camera, "Weather: keep a box of particles full around a moving camera.", false)) {
            fields({pfield("wrap_box", "Box (m)", "Particles leaving this box re-enter on the other side; 0 on an axis: off"),
                    pfield("wrap_fade", "Edge Fade")});
        } else {
            mark({"wrap_box", "wrap_fade"});
        }
    }

    // --- Rendering -------------------------------------------------------------------------
    if (section("Rendering", I::Render, scatter ? "Which mesh and material each instance draws with."
                                                : "What each particle looks like.", true)) {
        FieldDesc rm = pfield("render_mode", "Draw As");
        if (scatter) {
            rm.options = {"mesh", "none"};
            rm.option_tips = {"An instanced mesh", "Nothing (placement only)"};
        }
        fields({rm});
        if (render_mode == "mesh") {
            fields({pfield("render_mesh", "Mesh", "The mesh every particle / instance draws"),
                    pfield("cast_shadows", "Cast Shadows", "Off for grass and small clutter: their shadows cost a lot and show little")});
            r.absorb(draw_material_field(ctx, comp, env, [this](const std::string& rel) { open_asset(AssetType::Material, rel); }));
            placed.insert("material");
        } else if (render_mode != "none") {
            fields({pfield("sprite", "Sprite"), pfield("opacity", "Opacity"),
                    pfield("additive", "Glow (Additive)", "0 normal blending .. 1 glowing light (fire, sparks, magic)"),
                    pfield("emissive", "Brightness", "Above ~1.4 it blooms"),
                    pfield("lit", "Lit by Scene", "1: shaded by sun and lamps (smoke, dust); 0: self-lit (fire)"),
                    pfield("receive_shadows", "Receive Shadows"), pfield("softness", "Edge Softness"),
                    pfield("soft_distance", "Soft Near Surfaces (m)", "Fades where particles cut into geometry"),
                    pfield("camera_fade", "Fade Near Camera (m)")});
            if (str_of(comp, "sprite", "soft") == "texture") {
                fields({pfield("texture", "Texture"), pfield("flipbook", "Flipbook Grid", "Animation frames: columns (x) and rows (y)"),
                        pfield("flipbook_mode", "Flipbook Plays"), pfield("flipbook_fps", "Flipbook FPS")});
            }
            if (render_mode == "stretched") {
                fields({pfield("stretch_speed", "Stretch by Speed"), pfield("stretch_length", "Stretch Length")});
            }
            if (ctx.collapsing_header("More Look Settings", false)) {
                fields({pfield("aspect", "Aspect"), pfield("pivot", "Pivot"), pfield("distortion", "Edge Breakup"),
                        pfield("toon_bands", "Toon Bands"), pfield("scatter", "Light Scattering"),
                        pfield("scatter_anisotropy", "Scattering Direction"),
                        pfield("reactive", "Anti-Smear (TAA)", "1 for rain, snow and sparks: no streaky trails"),
                        pfield("sort", "Draw Order")});
            }
        }
        if (shape != "terrain") {
            fields({pfield("max_draw_distance", "Draw Distance (m)", "Not drawn beyond this from the camera; 0: always")});
        }
    }
    // Sprite settings never apply to meshes (and vice versa): out of sight unless they apply.
    mark({"sprite", "opacity", "additive", "emissive", "lit", "receive_shadows", "softness", "soft_distance", "camera_fade", "texture",
          "flipbook", "flipbook_mode", "flipbook_fps", "stretch_speed", "stretch_length", "aspect", "pivot", "distortion",
          "toon_bands", "scatter", "scatter_anisotropy", "reactive", "sort", "render_mesh", "material", "render_mode", "cast_shadows",
          "max_draw_distance"});

    // --- Advanced --------------------------------------------------------------------------
    if (section("Advanced", I::Gear, nullptr, false)) {
        fields({pfield("seed", "Random Seed", "Same seed, same result; 0: from the object's name")});
        if (!scatter) {
            fields({pfield("simulation_space", "Simulate In", "world: particles stay where born (trails); local: they ride along with the object"),
                    pfield("simulation", "Run On", "gpu: compute shaders, for 100k+ particles")});
        }
        // Anything the panel does not place itself (new or unusual keys) stays reachable here.
        std::vector<FieldDesc> rest;
        if (const ComponentSchema* schema = find_schema("ParticleSystem")) {
            for (const auto& f : schema->fields) {
                if (f.kind == FieldKind::Material || placed.count(f.key)) continue;
                rest.push_back(f);
            }
        }
        if (!rest.empty()) r.absorb(draw_fields(ctx, rest, comp, env, false));
    }
    return r;
}

EditResult EditorApp::draw_particle_terrain_section_(imm::Context& ctx, ObjectId id, Node& comp, const InspectorEnv& env) {
    using I = imm::Icon;
    EditResult r;
    const glm::vec4 dim = ctx.style.text_dim;
    Node tb = comp.contains("terrain") && comp.at("terrain").is_mapping() ? comp.at("terrain") : Node::mapping();
    const Node tb_before = tb;

    // The terrain it scatters over (live), for the pickers and read-outs.
    const toy::world::HeightmapTerrain* terrain = nullptr;
    std::vector<std::string> terrain_names{"(nearest above this object)"};
    for (ObjectId oid : doc_.all_ids()) {
        if (doc_.find_component(oid, "HeightmapTerrain") < 0) continue;
        if (const Node* o = doc_.find(oid)) terrain_names.push_back(get_string(*o, "name", "?"));
    }
    const std::string named = str_of(tb, "object", "");
    if (coopa::scene::SceneObject* live = sync_.live(id)) {
        if (!named.empty()) {
            if (coopa::scene::Scene* sc = engine_.has_scene() ? &engine_.scene() : nullptr) {
                if (coopa::scene::SceneObject* o = sc->find_object(named)) terrain = o->get_component<toy::world::HeightmapTerrain>();
            }
        } else {
            for (coopa::scene::SceneObject* o = live; o != nullptr && terrain == nullptr; o = o->parent()) {
                terrain = o->get_component<toy::world::HeightmapTerrain>();
            }
        }
    }

    ctx.paragraph("Instances grow over a Heightmap Terrain where its paint, slope and height allow, and stream in around "
                  "the camera chunk by chunk. Painting or sculpting the terrain re-grows them.", &dim);

    // Terrain picker.
    int ti = 0;
    for (std::size_t k = 1; k < terrain_names.size(); ++k) if (terrain_names[k] == named) ti = static_cast<int>(k);
    if (ctx.combo("Terrain", &ti, terrain_names)) {
        if (ti == 0) tb.as_map().erase("object");
        else tb["object"] = Node(terrain_names[static_cast<std::size_t>(ti)]);
        r.changed = r.finished = true;
        r.key = "terrain.object";
    }
    if (terrain == nullptr) ctx.label("No terrain found: put this object under a terrain, or pick one.", &ctx.style.warning);

    // Layer picker: the terrain's paint layers by name.
    {
        std::vector<std::string> layers{"(anywhere)"};
        if (terrain) for (const auto& l : terrain->layers) layers.push_back(l.name);
        const std::string cur = str_of(tb, "layer", "");
        int li = 0;
        for (std::size_t k = 1; k < layers.size(); ++k) if (layers[k] == cur) li = static_cast<int>(k);
        if (li == 0 && !cur.empty()) { layers.push_back(cur + " (missing)"); li = static_cast<int>(layers.size()) - 1; }
        if (ctx.combo("Grows On Layer", &li, layers)) {
            if (li == 0) tb.as_map().erase("layer");
            else tb["layer"] = Node(layers[static_cast<std::size_t>(li)]);
            r.changed = r.finished = true;
            r.key = "terrain.layer";
        }
        ctx.tooltip("Grows On Layer\nOnly where this paint layer is painted (Terrain Mode > Paint); (anywhere) ignores paint");
    }

    // Density: the setting people look for first, with what it means.
    {
        float density = num_of(tb, "density", 1.0f);
        const float before = density;
        ctx.drag_float("Density (per sq m)", &density, std::max(0.0005f, density * 0.02f), 0.0001f, 100.0f);
        const bool dragging = ctx.last_active();
        const bool released = ctx.last_deactivated();
        ctx.tooltip("Density\nHow many per square metre where it grows fully. 4 is a lawn; 0.01 a forest (one tree per 100 square metres).");
        if (ctx.button("Halve", 80, true, I::ArrowDown)) density *= 0.5f;
        ctx.same_line();
        if (ctx.button("Double", 80, true, I::ArrowRight)) density *= 2.0f;
        density = std::clamp(density, 0.0001f, 100.0f);
        if (density != before) {
            tb["density"] = Node(static_cast<double>(density));
            r.changed = true;
            r.key = "terrain.density";
            r.active = dragging;
            r.finished = !dragging;
        } else if (released) {
            r.finished = true;
        }
        char buf[200];
        const float spacing = 1.0f / std::sqrt(density);
        const float chunk = terrain && terrain->data ? terrain->data->geometry().chunk_size : 64.0f;
        std::snprintf(buf, sizeof(buf), "About one every %.2f m -- up to %.0f per %.0f m chunk", spacing, density * chunk * chunk, chunk);
        ctx.label(buf, &dim);
    }

    // The rest of the block, as ordinary fields on the nested map.
    auto f = [](const char* key, const char* label, float def, float speed, float lo, float hi, const char* tip) {
        FieldDesc d = f_float(key, def, speed, lo, hi);
        d.label = label;
        d.tooltip = tip;
        return d;
    };
    std::vector<FieldDesc> rest = {
        f("min_weight", "Paint Needed", 0.3f, 0.005f, 0.0f, 1.0f,
          "How strongly the layer must be painted before anything grows; fully painted ground grows at full density"),
        f("slope_min", "Min Slope (deg)", 0.0f, 0.2f, 0.0f, 90.0f, "Flatter ground than this stays bare"),
        f("slope_max", "Max Slope (deg)", 35.0f, 0.2f, 0.0f, 90.0f, "Steeper ground than this stays bare (trees: ~25, rocks: 90)"),
        f("height_min", "Min Height (m)", -1e9f, 0.5f, -1e9f, 1e9f, "World height below which nothing grows"),
        f("height_max", "Max Height (m)", 1e9f, 0.5f, -1e9f, 1e9f, "World height above which nothing grows (tree line)"),
        f("scale_min", "Size Variation Min", 0.8f, 0.01f, 0.01f, 100.0f, "Multiplies Size (Instances section)"),
        f("scale_max", "Size Variation Max", 1.2f, 0.01f, 0.01f, 100.0f, "Multiplies Size (Instances section)"),
        f("jitter", "Randomness", 1.0f, 0.01f, 0.0f, 1.0f, "0: a regular grid (orchards); 1: natural, random placement"),
        f("sink", "Sink Into Ground (m)", 0.05f, 0.005f, -10.0f, 10.0f, "Hides bases floating on slopes"),
        f("fade_start", "Thin Out From", 0.6f, 0.01f, 0.0f, 1.0f,
          "Fraction of the draw distance where instances start thinning out, reaching none at the draw distance (no hard "
          "edge, and far chunks cost less). 1: no thinning"),
    };
    const EditResult rr = draw_fields(ctx, rest, tb, env, false);
    if (rr.changed) { r.absorb(rr); r.key = "terrain." + rr.key; }
    else r.absorb(rr);

    // Draw distance lives on the system, but it is the other knob people need here.
    {
        std::vector<FieldDesc> dd = {pfield("max_draw_distance", "Draw Distance (m)",
                                            "Instances only exist this far from the camera -- the main cost control. "
                                            "0 = automatic: about 150x the mesh's height (grass ~90 m, an 8 m tree ~1 km), "
                                            "capped by the terrain's view distance")};
        r.absorb(draw_fields(ctx, dd, comp, env, false));
    }

    if (!(tb == tb_before)) comp["terrain"] = tb;
    return r;
}

} // namespace editor
} // namespace toy
