#include "editor/schema/component_schema.h"

namespace toy {
namespace editor {

std::string FieldDesc::display() const {
    if (!label.empty()) return label;
    std::string s = key;
    for (char& c : s) if (c == '_') c = ' ';
    if (!s.empty()) s[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(s[0])));
    return s;
}

FieldDesc f_bool(std::string k, bool def, bool in_default) {
    FieldDesc f; f.key = std::move(k); f.kind = FieldKind::Bool; f.def.x = def ? 1.0f : 0.0f; f.in_default = in_default; return f;
}

FieldDesc f_int(std::string k, int def, int lo, int hi, bool in_default) {
    FieldDesc f; f.key = std::move(k); f.kind = FieldKind::Int; f.def.x = static_cast<float>(def);
    f.min = static_cast<float>(lo); f.max = static_cast<float>(hi); f.speed = 0.2f; f.in_default = in_default; return f;
}

FieldDesc f_float(std::string k, float def, float speed, float lo, float hi, bool in_default) {
    FieldDesc f; f.key = std::move(k); f.kind = FieldKind::Float; f.def.x = def; f.speed = speed; f.min = lo; f.max = hi;
    f.in_default = in_default; return f;
}

FieldDesc f_vec3(std::string k, glm::vec3 def, float speed, bool in_default) {
    FieldDesc f; f.key = std::move(k); f.kind = FieldKind::Vec3; f.def = glm::vec4(def, 0.0f); f.speed = speed; f.in_default = in_default; return f;
}

FieldDesc f_color(std::string k, glm::vec3 def, bool in_default) {
    FieldDesc f; f.key = std::move(k); f.kind = FieldKind::Color; f.def = glm::vec4(def, 1.0f); f.in_default = in_default; return f;
}

FieldDesc f_enum(std::string k, std::vector<std::string> opts, bool in_default) {
    FieldDesc f; f.key = std::move(k); f.kind = FieldKind::Enum; f.options = std::move(opts);
    f.default_string = f.options.empty() ? "" : f.options.front(); f.in_default = in_default; return f;
}

FieldDesc f_int_enum(std::string k, std::vector<std::string> labels, int def,
                            std::vector<std::string> tips, bool in_default) {
    FieldDesc f = f_int(std::move(k), def, 0, static_cast<int>(labels.size()) - 1, in_default);
    f.options = std::move(labels); f.option_tips = std::move(tips); return f;
}

FieldDesc f_string(std::string k, std::string def, bool in_default) {
    FieldDesc f; f.key = std::move(k); f.kind = FieldKind::String; f.default_string = std::move(def); f.in_default = in_default; return f;
}

FieldDesc f_asset(std::string k, std::string dir, std::string ext, bool strip_ext, bool strip_dir,
                         std::string def, bool in_default) {
    FieldDesc f; f.key = std::move(k); f.kind = FieldKind::AssetRef; f.asset_dir = std::move(dir); f.asset_ext = std::move(ext);
    f.strip_ext = strip_ext; f.strip_dir = strip_dir; f.default_string = std::move(def); f.in_default = in_default; return f;
}

FieldDesc f_material() { FieldDesc f; f.key = "material"; f.kind = FieldKind::Material; f.in_default = true; return f; }

FieldDesc f_vec2(std::string k, glm::vec2 def, float speed, bool in_default) {
    FieldDesc f; f.key = std::move(k); f.kind = FieldKind::Vec2; f.def = glm::vec4(def, 0.0f, 0.0f); f.speed = speed; f.in_default = in_default; return f;
}

FieldDesc f_color4(std::string k, glm::vec4 def, bool in_default) {
    FieldDesc f; f.key = std::move(k); f.kind = FieldKind::Color4; f.def = def; f.in_default = in_default; return f;
}

FieldDesc f_padding(std::string k, bool in_default) {
    FieldDesc f; f.key = std::move(k); f.kind = FieldKind::Padding; f.def = glm::vec4(0.0f); f.speed = 0.5f; f.in_default = in_default; return f;
}

FieldDesc f_strings(std::string k, std::vector<std::string> def, bool in_default) {
    FieldDesc f; f.key = std::move(k); f.kind = FieldKind::StringList; f.in_default = in_default;
    f.def_node = Node::sequence();
    for (auto& d : def) f.def_node.as_seq().push_back(Node(d));
    return f;
}

FieldDesc f_child(std::string k, std::string def, bool in_default) {
    FieldDesc f; f.key = std::move(k); f.kind = FieldKind::ChildRef; f.default_string = std::move(def); f.in_default = in_default; return f;
}

FieldDesc f_items(std::string k, std::vector<FieldDesc> item, Node def, bool in_default) {
    FieldDesc f; f.key = std::move(k); f.kind = FieldKind::ItemList; f.item_fields = std::move(item); f.def_node = std::move(def);
    f.in_default = in_default; return f;
}

FieldDesc with_tip(FieldDesc f, std::string tip) { f.tooltip = std::move(tip); return f; }

FieldDesc f_texture(std::string k, std::string label, bool in_default) {
    FieldDesc f = f_asset(std::move(k), "textures", ".png", false, false, "", in_default);
    f.kind = FieldKind::Texture;
    f.label = std::move(label);
    return f;
}

FieldDesc f_curve(std::string k, std::vector<glm::vec2> def, bool in_default) {
    FieldDesc f;
    f.key = std::move(k);
    f.kind = FieldKind::Curve;
    f.in_default = in_default;
    f.def_node = Node::sequence();
    for (const glm::vec2& p : def) {
        const float v[2] = {p.x, p.y};
        f.def_node.as_seq().push_back(make_float_seq(v, 2));
    }
    return f;
}

FieldDesc f_gradient(std::string k, std::vector<std::pair<float, glm::vec3>> def, bool in_default) {
    FieldDesc f;
    f.key = std::move(k);
    f.kind = FieldKind::Gradient;
    f.in_default = in_default;
    f.def_node = Node::sequence();
    for (const auto& [t, c] : def) {
        const float v[4] = {t, c.r, c.g, c.b};
        f.def_node.as_seq().push_back(make_float_seq(v, 4));
    }
    return f;
}

Node field_default_node(const FieldDesc& f) {
    switch (f.kind) {
        case FieldKind::Bool:  return Node(f.def.x != 0.0f);
        case FieldKind::Int:   return Node(static_cast<int64_t>(f.def.x));
        case FieldKind::Float: return make_float(f.def.x);
        case FieldKind::Vec3:  return make_vec3(glm::vec3(f.def));
        case FieldKind::Vec4:  { float v[4] = {f.def.x, f.def.y, f.def.z, f.def.w}; return make_float_seq(v, 4); }
        case FieldKind::Color: return make_color(glm::vec3(f.def));
        case FieldKind::Vec2: {
            Node n = Node::mapping(); n["x"] = make_float(f.def.x); n["y"] = make_float(f.def.y); return n;
        }
        case FieldKind::Color4: {
            if (f.as_list) { float v[4] = {f.def.r, f.def.g, f.def.b, f.def.a}; return make_float_seq(v, 4); }
            Node n = make_color(glm::vec3(f.def)); n["a"] = make_float(f.def.w); return n;
        }
        case FieldKind::Padding: {
            Node n = Node::mapping();
            n["left"] = make_float(f.def.x); n["right"] = make_float(f.def.y); n["top"] = make_float(f.def.z); n["bottom"] = make_float(f.def.w);
            return n;
        }
        case FieldKind::StringList:
        case FieldKind::ItemList:
        case FieldKind::Curve:
        case FieldKind::Gradient: return f.def_node.is_sequence() ? f.def_node : Node::sequence();
        case FieldKind::Enum:
        case FieldKind::String:
        case FieldKind::AssetRef:
        case FieldKind::Texture:
        case FieldKind::ChildRef: return Node(f.default_string);
        case FieldKind::Material: {
            Node m = Node::mapping();
            m["albedo"] = make_color(glm::vec3(0.8f));
            m["metallic"] = make_float(0.0);
            m["roughness"] = make_float(0.5);
            return m;
        }
    }
    return Node();
}

FieldDesc with_label(FieldDesc f, std::string l) { f.label = std::move(l); return f; }

FieldDesc listed(FieldDesc f) { f.as_list = true; return f; }

FieldDesc root_relative(FieldDesc f) { f.ref_prefix = "assets/"; return f; }

FieldDesc startup(FieldDesc f) { f.startup_only = true; return f; }

FieldDesc project_only(FieldDesc f) { f.project_only = true; return f; }

FieldDesc with_default(FieldDesc f, std::string def) { f.default_string = std::move(def); return f; }

FieldDesc shown_when(FieldDesc f, std::string key, std::vector<std::string> values) {
    f.show.key = std::move(key);
    f.show.equals = std::move(values);
    f.show.negate = false;
    return f;
}

FieldDesc hidden_when(FieldDesc f, std::string key, std::vector<std::string> values) {
    f.show.key = std::move(key);
    f.show.equals = std::move(values);
    f.show.negate = true;
    return f;
}

FieldDesc as_slider(FieldDesc f) { f.slider = true; return f; }

FieldDesc with_unit(FieldDesc f, std::string unit) { f.suffix = std::move(unit); return f; }

const FieldDesc* ComponentSchema::field(const std::string& key) const {
    for (const auto& f : fields) if (f.key == key) return &f;
    return nullptr;
}

SectionBuilder::SectionBuilder(std::string title, std::string icon) {
    s_.title = std::move(title);
    s_.icon = std::move(icon);
}

SectionBuilder& SectionBuilder::says(std::string blurb) { s_.blurb = std::move(blurb); return *this; }

SectionBuilder& SectionBuilder::keys(std::vector<std::string> ks) {
    for (auto& k : ks) s_.keys.push_back(std::move(k));
    return *this;
}

SectionBuilder& SectionBuilder::sub(std::string title, std::string show_key, std::vector<std::string> values) {
    InspectorSubhead h;
    h.index = s_.keys.size();
    h.title = std::move(title);
    h.show.key = std::move(show_key);
    h.show.equals = std::move(values);
    s_.subheads.push_back(std::move(h));
    return *this;
}

SectionBuilder& SectionBuilder::toggle(std::string key) { s_.toggle = std::move(key); return *this; }

SectionBuilder& SectionBuilder::when(std::string key, std::vector<std::string> values) {
    s_.show.key = std::move(key);
    s_.show.equals = std::move(values);
    return *this;
}

SectionBuilder& SectionBuilder::custom(std::string id) { s_.custom = std::move(id); return *this; }

SectionBuilder& SectionBuilder::closed() { s_.open = false; return *this; }

InspectorSection SectionBuilder::done() { return std::move(s_); }

SectionBuilder sec(std::string title, std::string icon) { return SectionBuilder(std::move(title), std::move(icon)); }

const std::vector<FieldDesc>& material_fields() {
    static const std::vector<FieldDesc> fields = {
        f_color("albedo", glm::vec3(0.8f), true),
        as_slider(f_float("metallic", 0.0f, 0.005f, 0.0f, 1.0f, true)),
        as_slider(f_float("roughness", 0.5f, 0.005f, 0.0f, 1.0f, true)),
        with_tip(as_slider(f_float("ao", 1.0f, 0.005f, 0.0f, 1.0f)),
                 "Baked ambient occlusion over the whole surface; the renderer's own SSAO is on top of it"),
        f_color("emissive", glm::vec3(0.0f)),
        f_float("emissive_strength", 1.0f, 0.02f, 0.0f, 1000.0f),
        f_enum("alpha_mode", {"OPAQUE", "MASK", "CUTOUT", "BLEND"}),
        // Opacity does nothing to an OPAQUE material, and the cutoff only means something to
        // the alpha test MASK / CUTOUT run.
        hidden_when(as_slider(f_float("alpha", 1.0f, 0.005f, 0.0f, 1.0f)), "alpha_mode", {"OPAQUE"}),
        shown_when(as_slider(f_float("alpha_cutoff", 0.5f, 0.005f, 0.0f, 1.0f)), "alpha_mode", {"MASK", "CUTOUT"}),
        with_tip(hidden_when(f_bool("cull_backfaces", true), "alpha_mode", {"BLEND"}),
                 "Draw only the front of each face. Ignored when this material picks a surface shader (the "
                 "shader's own setting wins) and by transparent materials, which always cull the back"),
        f_bool("refraction", false),
        with_tip(f_float("ior", -1.0f, 0.005f, -1.0f, 3.0f), "-1: the render settings' refraction_ior (glass ~1.45, water ~1.33)"),
        with_tip(with_unit(f_float("refraction_thickness", -1.0f, 0.005f, -1.0f, 10.0f), "m"),
                 "-1: the render settings' refraction_thickness"),
        f_color("refraction_tint", glm::vec3(1.0f)),
        with_label(f_texture("texture_albedo"), "Albedo map"),
        with_label(f_texture("texture_normal"), "Normal map"),
        with_label(f_texture("texture_metallic_roughness"), "Metal/rough map"),
        with_label(f_texture("texture_alpha_mask"), "Alpha mask"),
        with_tip(with_label(f_texture("texture_displacement"), "Displacement map"),
                 "Height map (red channel) the tessellator moves vertices along their normal by; only on a "
                 "renderer with tessellation"),
        with_tip(with_unit(f_float("displacement_scale", 0.05f, 0.005f, -10.0f, 10.0f), "m"),
                 "Metres a white displacement texel moves"),
        with_tip(f_bool("snow", true), "Lying snow settles on this material's open, up-facing surfaces when the weather has it"),
        with_tip(f_bool("wet", false), "Rain and local wet patches darken this material and pool on its up-facing parts. Off by default: the stock rain / storm conditions carry wetness, so switching this on everywhere would restyle every wet scene"),
        // Toon (cel) shading, layered on this material's PBR inputs -- see PBRMaterial's
        // toon_* docs. Opaque materials only; a BLEND material ignores these.
        with_tip(f_int("toon_bands", 0, 0, 8), "Cel-shaded diffuse: how many light steps. 0 leaves this material PBR"),
        // 0 and 1 both mean "not cel-shaded", and then none of the three below does anything.
        with_tip(hidden_when(f_enum("toon_specular", {"pbr", "hard", "twostep", "off"}), "toon_bands", {"0", "1"}),
                 "Edge of the specular highlight: the PBR lobe untouched, cut to a hard step, a core plus a halo, or none"),
        with_tip(hidden_when(f_float("toon_rim", 0.0f, 0.25f, 0.0f, 1.75f), "toon_bands", {"0", "1"}),
                 "Rim accent on the lit side of each light; snaps to 0.25 steps"),
        with_tip(hidden_when(as_slider(f_float("toon_softness", 0.0f, 0.05f, 0.0f, 1.0f)), "toon_bands", {"0", "1"}),
                 "How soft each band's edge is, as a share of a band; snaps to 0 / 0.15 / 0.35 / 0.7"),
        with_tip(f_bool("toon_outline", false),
                 "Draw the outline on this material. Needs Outline on and its mode set to Toon"),
        // Drawn by draw_material_block()'s Shader section (named per-shader params), not as
        // plain fields -- listed here so they count as known material keys and overrides.
        f_enum("shader", {"", "triplanar", "foliage", "snow", "water"}),
        [] { FieldDesc f; f.key = "shader_params"; f.kind = FieldKind::Vec4; f.speed = 0.01f; return f; }(),
        [] { FieldDesc f; f.key = "shader_params_ext"; f.kind = FieldKind::Vec4; f.speed = 0.01f; return f; }(),
    };
    return fields;
}

const std::vector<InspectorSection>& material_sections() {
    static const std::vector<InspectorSection> sections = {
        sec("Surface", "Material")
            .says("The look of the material itself, before any map is applied.")
            .keys({"albedo", "metallic", "roughness", "ao"}).done(),
        sec("Maps", "Image")
            .says("Textures sampled at the mesh's UVs. Drop a file from the Textures tab onto a slot.")
            .keys({"texture_albedo", "texture_normal", "texture_metallic_roughness"})
            .sub("Displacement").keys({"texture_displacement", "displacement_scale"})
            .done(),
        sec("Emission", "Sun")
            .says("Light the surface appears to give off. It glows, but it does not light anything else.")
            .keys({"emissive", "emissive_strength"}).closed().done(),
        sec("Transparency", "ShadeWire")
            .says("Whether the surface is solid, cut out by a mask, or see-through.")
            .keys({"alpha_mode", "alpha", "alpha_cutoff", "texture_alpha_mask", "cull_backfaces"}).done(),
        sec("Refraction", "Sphere").toggle("refraction").when("alpha_mode", {"BLEND"})
            .says("Bend the background through the surface -- glass, water, ice. Each value falls back to the "
                  "render settings' when it is negative.")
            .keys({"ior", "refraction_thickness", "refraction_tint"}).done(),
        sec("Stylize", "Palette")
            .says("Cel shading, layered on the PBR result above. Opaque materials only.")
            .keys({"toon_bands", "toon_specular", "toon_rim", "toon_softness", "toon_outline"}).closed().done(),
        sec("Weather", "World").keys({"snow", "wet"}).closed().done(),
    };
    return sections;
}

const ComponentSchema& material_schema() {
    static const ComponentSchema schema = [] {
        ComponentSchema s;
        s.type = "Material";
        s.category = "Rendering";
        s.fields = material_fields();
        s.sections = material_sections();
        return s;
    }();
    return schema;
}

const std::vector<SurfaceShaderInfo>& surface_shaders() {
    static const std::vector<SurfaceShaderInfo> shaders = {
        {"", "Standard (PBR)", false, "Deferred PBR, mesh UVs. Every material uses it unless it picks another.", false, {}},
        {"triplanar", "Triplanar", false, "Maps projected along X/Y/Z instead of mesh UVs. Opaque only.", false, {
            {"Tiling", 1.0f, 0.01f, 100.0f, 0.01f, {}},
            {"Blend sharpness", 4.0f, 1.0f, 32.0f, 0.05f, {}},
            {"Space", 0.0f, 0.0f, 1.0f, 1.0f, {"World", "Object"}},
            {"Normal strength", 1.0f, 0.0f, 4.0f, 0.01f, {}},
        }},
        {"foliage", "Foliage", false,
         "Grass, bushes, canopies and leaf cards that bend in the World tab's wind (and its gusts). The material only says "
         "how the plant responds; the wind itself is the weather's.", false, {
            {"Flexibility", 0.12f, 0.0f, 2.0f, 0.005f, {}},   // tip sway / height at 10 m/s: grass ~0.35, bush ~0.1, canopy ~0.04
            {"Flutter rate", 1.6f, 0.0f, 20.0f, 0.01f, {}},   // rad/s
            {"Height", 1.0f, 0.0f, 100.0f, 0.01f, {}},        // the plant's height in mesh units; sway grows root -> this
            {"Up axis", 0.0f, 0.0f, 1.0f, 1.0f, {"Z (meshes)", "Y (stood-up cards)"}},
        }},
        {"snow", "Deep snow", false, "Raised by the weather's lying snow, carved by SnowDeformers. Give the renderer tessellation.", false, {}},
        {"water", "Water", true, "Waves, depth colour and foam. Needs a WaterBody on the object.", true, {}},
    };
    return shaders;
}

const SurfaceShaderInfo* find_surface_shader(const std::string& name) {
    for (const auto& s : surface_shaders()) if (s.name == name) return &s;
    return nullptr;
}

std::map<std::string, ComponentSchema>& schema_table_() {
    static std::map<std::string, ComponentSchema> table = [] {
        std::map<std::string, ComponentSchema> t;
        auto add = [&](ComponentSchema s) { t[s.type] = std::move(s); };
        const auto phys_mat = f_asset("material", "physics_materials", ".yaml", true, true);

        add({"Transform", "Core", {
            f_vec3("position", glm::vec3(0.0f), 0.02f, true),
            f_vec3("rotation", glm::vec3(0.0f), 0.5f, true),
            f_vec3("scale", glm::vec3(1.0f), 0.01f, true),
            with_tip(f_float("bone_length", 0.0f, 0.01f, 0.0f, 10000.0f),
                     "Optional length of this object as a bone (m), for the editor's Bone origin style. 0 lets the editor guess "
                     "it from the hierarchy; the runtime ignores it"),
            with_tip(f_enum("origin_style", {"Inherit", "Axes", "Orb"}),
                     "How this object's origin is drawn in the editor. Inherit takes the nearest ancestor's, and Axes if none "
                     "sets one; the runtime ignores it"),
            with_tip(f_float("origin_size", 0.0f, 0.005f, 0.0f, 0.5f),
                     "How big this object's origin glyph is drawn (m) in the editor. 0 inherits; the runtime ignores it"),
        }, true, false});
        add({"MeshRenderer", "Rendering", {
            f_asset("mesh_path", "meshes", ".yaml", true, true, "cube", true),
            f_material(),
            with_tip(f_float("lod_bias", 1.0f, 0.01f, 0.0f, 100.0f), "Multiplier on the LOD switch distances (1 = as authored)"),
            f_bool("lods_enabled", true),
            f_bool("affects_reflection_probes", true),
            with_tip(f_bool("tessellation", false), "Subdivide near the camera on the GPU (ocean planes, terrain, deep snow): the "
                                                     "material's displacement map and surface shader act on the new vertices"),
            with_tip(f_float("tess_edge_pixels", 6.0f, 0.1f, 0.5f, 256.0f), "Tessellation: target edge length on screen (px); smaller = denser"),
            with_tip(f_float("tess_max_factor", 16.0f, 0.1f, 1.0f, 64.0f), "Tessellation: most splits per edge"),
            with_tip(f_float("tess_max_distance", 60.0f, 0.5f, 0.0f, 10000.0f), "Tessellation: beyond this (m) the mesh is drawn as authored"),
        }});
        add({"Camera", "Rendering", {
            f_bool("main", true, true),
            f_enum("projection", {"Perspective", "Orthographic"}, true),
            f_float("fov", 60.0f, 0.2f, 1.0f, 179.0f, true),
            f_float("orthographic_size", 3.0f, 0.05f, 0.01f, 10000.0f),
            f_float("near_clip_plane", 0.1f, 0.01f, 0.0001f, 1000.0f, true),
            f_float("far_clip_plane", 1000.0f, 1.0f, 0.01f, 100000.0f, true),
            f_float("lens", 50.0f, 0.5f, 1.0f, 1000.0f),
            with_tip(f_float("aperture", 0.0f, 0.05f, 0.0f, 64.0f), "f-stop; 0 uses the render settings' dof_aperture"),
            with_tip(f_float("focus_distance", 0.0f, 0.05f, 0.0f, 10000.0f), "Metres; 0 uses the render settings' dof_focus_distance"),
            f_string("focus_object"),
            with_tip(f_bool("motion_blur", true), "Off keeps this camera's image free of motion blur when the render settings turn it on"),
        }});
        add({"DirectionalLight", "Lighting", {
            f_vec3("direction", glm::vec3(-0.35f, -0.45f, -0.82f), 0.01f, true),
            f_color("color", glm::vec3(1.0f, 0.97f, 0.9f), true),
            f_float("intensity", 1.0f, 0.01f, 0.0f, 1000.0f, true),
            f_bool("cast_shadows", true, true),
            f_float("shadow_intensity", 1.0f, 0.01f, 0.0f, 1.0f),
        }});
        add({"PointLight", "Lighting", {
            f_color("color", glm::vec3(1.0f), true),
            f_float("intensity", 50.0f, 0.2f, 0.0f, 100000.0f, true),
            f_float("range", 10.0f, 0.05f, 0.0f, 10000.0f, true),
            f_bool("cast_shadows", false, true),
            f_float("attenuation_constant", 1.0f, 0.05f, 0.1f, 64.0f),
        }, false});
        add({"SpotLight", "Lighting", {
            f_color("color", glm::vec3(1.0f), true),
            f_vec3("direction", glm::vec3(0.0f, 0.0f, -1.0f), 0.01f, true),
            f_float("intensity", 50.0f, 0.2f, 0.0f, 100000.0f, true),
            f_float("range", 10.0f, 0.05f, 0.0f, 10000.0f, true),
            f_float("inner_angle", 20.0f, 0.2f, 0.0f, 89.0f, true),
            f_float("outer_angle", 30.0f, 0.2f, 0.0f, 89.0f, true),
            f_bool("cast_shadows", true),
            f_float("attenuation_constant", 1.0f, 0.05f, 0.1f, 64.0f),
        }, false});
        add({"EnvironmentLight", "Lighting", {
            f_color("sky_color", glm::vec3(0.55f, 0.65f, 0.85f), true),
            f_color("ground_color", glm::vec3(0.25f, 0.22f, 0.2f), true),
            f_float("sky_intensity", 1.0f, 0.01f, 0.0f, 100.0f, true),
            f_asset("hdri_path", "textures", ".png"),
        }});
        add({"ReflectionProbe", "Lighting", {
            f_vec3("box_extent", glm::vec3(5.0f), 0.05f, true),
            f_int("resolution", 64, 8, 1024, true),
            f_float("blend_distance", 1.0f, 0.02f, 0.0f, 100.0f),
            f_int("importance", 1, -100, 100),
            f_float("intensity", 1.0f, 0.01f, 0.0f, 10.0f),
        }, false});
        add({"SdfRenderer", "Rendering", {
            f_vec3("bounds_center", glm::vec3(0.0f), 0.02f),
            f_vec3("bounds_extent", glm::vec3(1.0f), 0.02f, true),
            f_bool("cast_shadows", true),
            f_int("max_steps", 64, 1, 512),
            f_float("smoothing", 0.0f, 0.005f, 0.0f, 10.0f),
            f_material(),
        }});
        add({"SdfShape", "Rendering", {
            f_enum("shape", {"Sphere", "Box", "Plane"}, true),
            f_vec3("params", glm::vec3(0.5f), 0.01f, true),
            f_float("rounding", 0.0f, 0.005f, 0.0f, 10.0f),
            f_enum("op", {"Union", "Subtract", "Intersect"}),
            f_float("blend", 0.0f, 0.005f, 0.0f, 10.0f),
        }, false});
        add({"BoxCollider", "Physics", {
            f_vec3("size", glm::vec3(1.0f), 0.02f, true),
            f_vec3("center", glm::vec3(0.0f), 0.02f),
            phys_mat, f_bool("is_trigger", false), f_int("layer", 0, 0, 31),
        }, false});
        add({"SphereCollider", "Physics", {
            f_float("radius", 0.5f, 0.01f, 0.0f, 10000.0f, true),
            f_vec3("center", glm::vec3(0.0f), 0.02f),
            phys_mat, f_bool("is_trigger", false), f_int("layer", 0, 0, 31),
        }, false});
        add({"CapsuleCollider", "Physics", {
            f_float("radius", 0.5f, 0.01f, 0.0f, 10000.0f, true),
            f_float("height", 2.0f, 0.01f, 0.0f, 10000.0f, true),
            f_int_enum("direction", {"X", "Y", "Z"}, 2,
                       {"Capsule axis along local X", "Capsule axis along local Y", "Capsule axis along local Z (up)"}),
            f_vec3("center", glm::vec3(0.0f), 0.02f),
            phys_mat, f_bool("is_trigger", false), f_int("layer", 0, 0, 31),
        }, false});
        add({"MeshCollider", "Physics", {
            f_asset("mesh_path", "meshes", ".yaml", true, true, "", true),
            f_bool("convex", false),
            phys_mat, f_bool("is_trigger", false), f_int("layer", 0, 0, 31),
        }, false});
        add({"Rigidbody", "Physics", {
            f_float("mass", 1.0f, 0.01f, 0.0f, 1e6f, true),
            f_float("drag", 0.0f, 0.005f, 0.0f, 100.0f),
            f_float("angular_drag", 0.05f, 0.005f, 0.0f, 100.0f),
            with_tip(f_float("wind_influence", 0.0f, 0.01f, 0.0f, 100.0f),
                     "How hard the weather's wind pushes this body: about 0.6 x the area it shows the wind (sq m). "
                     "0 ignores the wind; light bodies blow about, heavy ones barely move"),
            with_tip(f_vec3("center_of_mass", glm::vec3(0.0f), 0.01f),
                     "Where the mass sits, as an offset from the collider's centre. Lowering it is what "
                     "stops a tall body tipping: it rolls when lateral acceleration passes "
                     "half-track / centre-of-mass height"),
            f_bool("use_gravity", true, true),
            f_bool("is_kinematic", false, true),
            f_enum("interpolation", {"Interpolate", "None"}),   // physx_yaml.h reads a string: "None" or anything else
        }});
        // physxcoopa vehicles (components/vehicle.h): `preset` picks a tuning from
        // vehicle/vehicle_presets.h and every other field overrides one of its values, so the
        // DEFAULTS BELOW ARE pickup_4x4's -- the preset VehicleComponent::config starts as. A row
        // whose default disagreed with the preset would write that key into the YAML the first
        // time the inspector touched the component, silently pinning it against the preset.
        add({"Vehicle", "Physics", {
            with_tip(f_enum("preset", {"pickup_4x4", "sports_car", "hauler_6x6", "buggy", "dozer", "trailer"}, true),
                     "Which tuning to start from; the fields below override it"),
            with_tip(f_enum("steer_mode", {"ackermann", "differential"}),
                     "ackermann turns the steered wheels; differential drives the two sides against "
                     "each other instead (a tracked machine, which can pivot on the spot)"),
            with_tip(f_float("max_steer_deg", 36.0f, 0.5f, 0.0f, 80.0f, true), "Steering lock at a standstill"),
            with_tip(f_float("min_steer_deg", 11.0f, 0.5f, 0.0f, 80.0f),
                     "Steering lock at steer_falloff_speed and above -- the gap to max_steer_deg is what "
                     "keeps a vehicle stable at speed without making it a bus at parking pace"),
            f_float("steer_falloff_speed", 28.0f, 0.5f, 0.0f, 200.0f),
            f_float("steer_rate_deg", 240.0f, 5.0f, 1.0f, 2000.0f),
            f_float("steer_return_rate_deg", 340.0f, 5.0f, 1.0f, 2000.0f),
            with_tip(f_float("brake_torque", 5200.0f, 50.0f, 0.0f, 1e6f), "Nm at full pedal, split by each wheel's brake_share"),
            with_tip(f_float("handbrake_torque", 7000.0f, 50.0f, 0.0f, 1e6f), "Nm, split by each wheel's handbrake_share"),
            with_tip(f_float("anti_roll", 8000.0f, 100.0f, 0.0f, 1e6f),
                     "N/m of compression difference transferred across an axle. Resists body roll without "
                     "stiffening the springs; keep it well under the wheel rate"),
            with_tip(f_float("drag", 0.9f, 0.01f, 0.0f, 100.0f), "Aerodynamic drag, N per (m/s)^2"),
            with_tip(f_float("downforce", 0.0f, 0.01f, 0.0f, 100.0f), "N per (m/s)^2, pressing the chassis down"),
            with_tip(f_float("air_righting", 2.5f, 0.1f, 0.0f, 50.0f),
                     "How hard an airborne chassis is torqued back level; 0 leaves it tumbling honestly"),
            f_float("air_damping", 1.5f, 0.1f, 0.0f, 50.0f),
            with_tip(f_float("nominal_wheel_load", 0.0f, 10.0f, 0.0f, 1e6f),
                     "Load the tyre gives its full grip at, N. 0 derives it from the chassis mass"),
            with_tip(f_float("recover_time", 1.4f, 0.05f, 0.1f, 10.0f),
                     "Seconds the righting takes when a flipped vehicle is recovered: it floats clear, "
                     "turns smoothly back upright about the heading it had, and lands on its suspension"),
            with_tip(f_float("recover_lift", 0.8f, 0.05f, 0.0f, 10.0f),
                     "How far above its ride height it floats while turning over"),
            with_tip(f_float("auto_recover_time", 0.0f, 0.1f, 0.0f, 60.0f),
                     "Seconds upside down and still before it rights itself unasked; 0 never does"),
            f_bool("player_input", true),
            with_tip(f_bool("active", true), "The vehicle the player's input reaches; V cycles it"),
            with_tip(f_items("wheels", {
                f_string("object", ""),
                f_vec3("anchor", glm::vec3(0.0f), 0.01f),
                f_float("radius", 0.40f, 0.005f, 0.01f, 5.0f),
                f_float("width", 0.28f, 0.005f, 0.01f, 5.0f),
                f_float("mass", 30.0f, 0.5f, 0.1f, 2000.0f),
                f_float("rest_length", 0.42f, 0.005f, 0.0f, 3.0f),
                f_float("travel", 0.26f, 0.005f, 0.0f, 3.0f),
                f_float("stiffness", 0.0f, 100.0f, 0.0f, 1e7f),
                f_float("natural_frequency", 1.45f, 0.01f, 0.05f, 10.0f),
                f_float("damping_ratio", 0.45f, 0.01f, 0.0f, 5.0f),
                f_float("rebound_ratio", 0.65f, 0.01f, 0.0f, 5.0f),
                f_bool("steered", false),
                f_float("drive_share", 0.0f, 0.05f, 0.0f, 10.0f),
                f_float("brake_share", 1.0f, 0.05f, 0.0f, 10.0f),
                f_float("handbrake_share", 0.0f, 0.05f, 0.0f, 10.0f),
                f_int("anti_roll_group", -1, -1, 15),
                f_enum("side", {"left", "right"}),
            }), "One entry per wheel, in the preset's own order (front axle first, left then right). "
                "`object` names the child drawn there; `stiffness` 0 derives the rate from natural_frequency"),
        }});
        // physxcoopa joints (components/hinge_joint.h, ball_joint.h, cone_twist_joint.h): anchor and
        // axis in THIS object's local frame; the other side is `connected_object`, and both need a
        // Collider. Angles in degrees, relative to the pose the joint binds in.
        add({"HingeJoint", "Physics", {
            with_tip(f_string("connected_object", "", true), "Object (by name) this hinges to"),
            with_tip(f_vec3("anchor", glm::vec3(0.0f), 0.01f, true), "Hinge point, in this object's local frame"),
            with_tip(f_vec3("axis", glm::vec3(0.0f, 0.0f, 1.0f), 0.01f, true), "Hinge axis, in this object's local frame"),
            f_bool("use_limits", false),
            f_float("min_angle", 0.0f, 0.5f, -360.0f, 360.0f),
            f_float("max_angle", 0.0f, 0.5f, -360.0f, 360.0f),
            with_tip(f_bool("enable_collision", false), "The two bodies still collide with each other"),
        }});
        add({"BallJoint", "Physics", {
            with_tip(f_string("connected_object", "", true), "Object (by name) this is pinned to"),
            with_tip(f_vec3("anchor", glm::vec3(0.0f), 0.01f, true), "Pivot, in this object's local frame"),
            with_tip(f_bool("enable_collision", false), "The two bodies still collide with each other"),
        }});
        add({"ConeTwistJoint", "Physics", {
            with_tip(f_string("connected_object", "", true), "Object (by name) this is jointed to"),
            with_tip(f_vec3("anchor", glm::vec3(0.0f), 0.01f, true), "Pivot, in this object's local frame"),
            with_tip(f_vec3("axis", glm::vec3(0.0f, 0.0f, 1.0f), 0.01f, true), "Twist axis (along the limb), in this object's local frame"),
            with_tip(f_float("swing_limit", 45.0f, 0.5f, -1.0f, 180.0f, true), "Cone half-angle; negative = free swing"),
            with_tip(f_float("twist_min", -30.0f, 0.5f, -180.0f, 180.0f), "Twist range; min > max = free twist"),
            f_float("twist_max", 30.0f, 0.5f, -180.0f, 180.0f),
            with_tip(f_bool("enable_collision", false), "The two bodies still collide with each other"),
        }});
        // toyengine/scene/ragdoll.h, on a rig root. Each bone's joint keys describe the joint to
        // its nearest ancestor bone (the first bone has none).
        add({"Ragdoll", "Physics", {
            with_tip(f_enum("mode", {"animated", "ragdoll"}, true), "animated: bones follow the Animator; ragdoll: limp from the start"),
            with_tip(f_vec3("start_impulse", glm::vec3(0.0f), 1.0f), "mode ragdoll: the impulse (N s) it starts with"),
            with_tip(f_bool("auto_generate", false), "With no bones listed: fit capsules to the rig's empties"),
            with_tip(f_float("mass", 70.0f, 0.1f, 0.0f, 100000.0f), "Total, shared by volume over bones with mass 0"),
            with_tip(f_float("blend_time", 0.5f, 0.01f, 0.0f, 10.0f), "Seconds to blend back into animation on recovery"),
            with_tip(f_string("recover_state", ""), "Animator state recovered into (empty: auto_play)"),
            with_tip(f_bool("collide_connected", false), "Jointed bone pairs collide with each other"),
            f_float("drag", 0.05f, 0.005f, 0.0f, 100.0f),
            f_float("angular_drag", 0.8f, 0.005f, 0.0f, 100.0f),
            f_int("layer", 0, 0, 31),
            with_tip(f_float("rest_time", 1.0f, 0.05f, 0.0f, 60.0f), "Seconds calm before a limp body is put to sleep; 0 = off"),
            with_tip(f_float("rest_travel", 0.02f, 0.005f, 0.0f, 10.0f), "At rest: every bone within this (m) of where it was for rest_time"),
            with_tip(f_float("max_limp_speed", 8.0f, 0.1f, 0.0f, 1000.0f), "Cap on the speed (m/s) a bone carries in from the animation; 0 = off"),
            with_tip(f_float("max_limp_spin", 12.0f, 0.1f, 0.0f, 1000.0f), "...and on its spin (rad/s)"),
            with_tip(f_bool("link_character", true), "Suspend this object's CharacterController while limp"),
            with_tip(f_bool("reposition_root", true), "Without a controller: stand up where the body fell"),
            with_tip(f_bool("input_toggle", false), "R goes limp / recovers"),
            with_tip(f_vec3("toggle_impulse", glm::vec3(0.0f), 1.0f), "Impulse (N s) applied when R goes limp"),
            with_tip(f_items("bones", {
                f_string("bone", "pelvis"),
                f_enum("shape", {"capsule", "box", "sphere"}),
                f_float("radius", 0.08f, 0.005f, 0.0f, 10.0f),
                f_float("height", 0.3f, 0.005f, 0.0f, 10.0f),
                f_enum("direction", {"z", "x", "y"}),
                f_vec3("size", glm::vec3(0.2f), 0.005f),
                f_vec3("center", glm::vec3(0.0f), 0.005f),
                f_float("mass", 0.0f, 0.05f, 0.0f, 10000.0f),
                f_enum("joint", {"cone_twist", "hinge", "ball"}),
                f_vec3("anchor", glm::vec3(0.0f), 0.005f),
                f_vec3("axis", glm::vec3(0.0f), 0.01f),
                f_float("swing", 45.0f, 0.5f, -1.0f, 180.0f),
                f_float("twist_min", -30.0f, 0.5f, -180.0f, 180.0f),
                f_float("twist_max", 30.0f, 0.5f, -180.0f, 180.0f),
                f_float("limit_min", -90.0f, 0.5f, -360.0f, 360.0f),
                f_float("limit_max", 90.0f, 0.5f, -360.0f, 360.0f),
            }), "Bone path, shape, mass, and the joint to the parent bone (degrees)"),
        }});
        // toyengine/water/: a body of water (baked by WaterSystem onto the sibling MeshRenderer)
        // and a floating Rigidbody. `size` uses x/y only (the procedural grid's extent).
        add({"WaterBody", "Water", {
            f_enum("mode", {"planar", "flowing"}, true),
            f_asset("mesh_path", "meshes", ".yaml", true, true),
            f_vec3("size", glm::vec3(20.0f, 20.0f, 0.0f), 0.1f, true),
            f_int("resolution", 48, 1, 512, true),
            f_float("tile_size", 0.0f, 0.5f, 0.0f, 1000.0f),
            f_float("wave_amplitude", 0.15f, 0.005f, 0.0f, 20.0f, true),
            f_float("wave_length", 8.0f, 0.05f, 0.1f, 500.0f, true),
            f_float("wave_direction", 0.0f, 0.5f, -360.0f, 360.0f),
            f_float("wave_steepness", 0.5f, 0.005f, 0.0f, 1.0f),
            with_tip(f_float("wind_response", 1.0f, 0.01f, 0.0f, 1.0f),
                     "How much the weather's wind reshapes the water: 1 = calm air flattens it, a gale roughens it and turns "
                     "the waves downwind (the waves above are the sea at ~6 m/s); 0 = always exactly as authored"),
            f_float("flow_speed", 1.0f, 0.01f, 0.0f, 50.0f),
            f_float("flow_min_speed", 0.6f, 0.01f, 0.0f, 50.0f),
            f_float("flow_slope_gain", 4.0f, 0.05f, 0.0f, 100.0f),
            f_float("obstacle_radius", 1.5f, 0.02f, 0.0f, 50.0f),
            f_float("wake_length", 4.0f, 0.05f, 0.0f, 100.0f),
            f_color("foam_color", glm::vec3(0.92f, 0.96f, 1.0f)),
            f_float("foam_amount", 1.0f, 0.01f, 0.0f, 4.0f),
            f_float("shore_foam_depth", 0.6f, 0.01f, 0.0f, 20.0f),
            f_float("edge_fade_depth", 0.15f, 0.005f, 0.0f, 5.0f),
            f_float("ripple_strength", 0.35f, 0.005f, 0.0f, 4.0f),
            f_float("ripple_scale", 1.2f, 0.01f, 0.01f, 20.0f),
            f_float("clarity", 3.0f, 0.02f, 0.01f, 200.0f),
            f_color("underwater_color", glm::vec3(0.05f, 0.24f, 0.28f)),
            f_float("underwater_visibility", 14.0f, 0.1f, 0.1f, 1000.0f),
            f_color("underwater_absorption", glm::vec3(0.35f, 0.09f, 0.06f)),
            f_float("caustics", 1.0f, 0.01f, 0.0f, 10.0f),
            f_float("density", 1000.0f, 1.0f, 1.0f, 20000.0f),
            f_float("max_depth", 60.0f, 0.5f, 0.1f, 10000.0f),
        }});
        // Hair/fur grown from the faces of the mesh beside it -- see toyengine/hair/README.md.
        // `thickness_over_length` is the first user of FieldKind::Curve in the tree: a width
        // profile is exactly the drag-a-point box that widget was written for.
        add({"Hair", "Rendering", {
            f_enum("shape", {"ribbon", "tube"}, true),
            with_tip(f_int("cross_section", 2, 1, 3, true),
                     "Ribbon: crossed planes per strand. 1 is cheapest but a strand vanishes when "
                     "seen edge-on; 2 is the usual choice for hair and fur"),
            with_tip(f_int("sides", 5, 3, 16),
                     "Tube: sides of the extruded strand. Opaque, so no alpha test -- what a few "
                     "deliberately thick stylised strands want"),
            with_tip(f_int("segments", 8, 1, 64, true),
                     "Points along one strand. Also the particle count per simulated guide"),

            with_tip(f_float("density", 20000.0f, 50.0f, 0.0f, 2e6f, true),
                     "Strands per SQUARE METRE of scalp, so a bigger head grows proportionally more"),
            with_tip(f_int("max_strands", 8000, 0, 1 << 20, true),
                     "Ceiling whatever density x area implies. A groom over it is thinned evenly, "
                     "never truncated"),
            with_unit(with_tip(f_float("length", 0.12f, 0.002f, 1e-4f, 100.0f, true),
                               "Before the mask's green channel and the guides"), "m"),
            as_slider(f_float("length_jitter", 0.25f, 0.005f, 0.0f, 0.95f)),
            with_unit(with_tip(f_float("thickness", 0.0012f, 0.0002f, 1e-5f, 1.0f, true),
                               "Across, at the root. 0.001 is a human hair; 0.03 a thick poly strand"), "m"),
            with_tip(f_curve("thickness_over_length", {{0.0f, 1.0f}, {1.0f, 0.25f}}),
                     "Multiplies thickness from root (left) to tip (right)"),

            with_tip(f_float("droop", 0.4f, 0.01f, 0.0f, 10.0f),
                     "How far a strand falls over its length, in multiples of its own length. 0 "
                     "stands straight off the scalp"),
            as_slider(with_tip(f_float("clump", 0.3f, 0.01f, 0.0f, 1.0f),
                               "Gathers nearby strands into locks, tips more than roots")),
            as_slider(f_float("clump_density", 0.02f, 0.002f, 1e-4f, 1.0f)),
            with_tip(f_float("curl", 0.0f, 0.05f, 0.0f, 20.0f), "Turns of a helix over the strand"),
            with_unit(f_float("curl_radius", 0.01f, 0.001f, 0.0f, 10.0f), "m"),
            with_unit(with_tip(f_float("frizz_degrees", 6.0f, 0.1f, 0.0f, 89.0f),
                               "Randomises each strand away from the scalp normal"), "deg"),

            with_tip(f_texture("root_mask", "Root Mask"),
                     "Sampled by the scalp's UV: red scales density, green scales length"),
            with_tip(f_child("guides"),
                     "An object whose descendants' Curve components shape the groom. Draw them "
                     "with Pen Mode"),
            as_slider(f_float("guide_influence", 1.0f, 0.01f, 0.0f, 1.0f)),
            f_int("guide_count", 3, 1, 8),

            with_tip(f_float("wind_flexibility", 0.25f, 0.005f, 0.0f, 4.0f),
                     "Tip sway in a 10 m/s wind, as a fraction of the strand's length. This is "
                     "what moves a groom that is not simulated, and it costs nothing per frame"),
            f_float("wind_frequency", 3.0f, 0.02f, 0.0f, 40.0f),

            with_tip(f_bool("simulate", false, true),
                     "Off: a static mesh swaying in the vertex shader. On: guide strands are "
                     "solved by the physics engine and collide with the body"),
            with_tip(f_int("sim_guides", 64, 1, 4096),
                     "Guides actually solved. Render strands follow these, so THIS is what the "
                     "solver costs scale with -- not density"),
            as_slider(with_tip(f_float("sim_stiffness", 0.12f, 0.005f, 0.0f, 1.0f),
                               "How hard hair is pulled back to the shape it was combed into, per "
                               "1/60 s. 0 is limp and forgets its groom; 1 is a wig that barely "
                               "reacts. Takes effect live -- no re-scatter")),
            with_tip(f_float("sim_stiffness_falloff", 1.6f, 0.02f, 0.0f, 16.0f),
                     "How fast that fades toward the tip. Higher frees the tips sooner, which is "
                     "what makes hair swing from its ends rather than as one board"),
            as_slider(with_tip(f_float("sim_curl_hold", 0.25f, 0.005f, 0.0f, 1.0f),
                               "How hard each segment's rest ANGLE is restored. What holds a curl")),
            f_float("sim_damping", 0.06f, 0.005f, 0.0f, 10.0f),
            with_tip(f_float("sim_drag", 0.6f, 0.01f, 0.0f, 20.0f),
                     "Drag against the air -- what makes hair trail behind a moving head"),
            with_unit(with_tip(f_float("sim_collision_thickness", 0.01f, 0.001f, 0.0f, 1.0f),
                               "How far off a collider a strand is held. Hair has no volume of "
                               "its own, so 0 would z-fight the skin"), "m"),
            as_slider(with_tip(f_float("sim_friction", 0.4f, 0.01f, 0.0f, 1.0f),
                               "1 grips, so hair resting on a shoulder stays there")),
            f_float("sim_gravity", 1.0f, 0.01f, -10.0f, 10.0f),

            with_tip(f_vec2("lod_screen_sizes", glm::vec2(0.09f, 0.035f), 0.002f),
                     "Projected height (a fraction of the screen's) below which each level is "
                     "drawn; each halves the strand count. Must descend"),
            with_tip(f_float("cull_screen_size", 0.012f, 0.001f, 0.0f, 1.0f),
                     "Below this projected height the groom is not drawn at all. 0 never culls"),

            f_enum("regenerate", {"on_change", "on_load", "manual"}),
            f_int("seed", 7, 0, 1000000),
            f_material(),
        }});
        add({"Buoyancy", "Water", {
            f_int("subdivisions", 2, 1, 4, true),
            f_float("volume", -1.0f, 0.01f, -1.0f, 1e6f),
            f_float("linear_drag", 1.5f, 0.01f, 0.0f, 100.0f, true),
            f_float("angular_drag", 1.0f, 0.01f, 0.0f, 100.0f, true),
            f_float("form_drag", 1.0f, 0.01f, 0.0f, 10.0f),
            f_float("buoyancy_scale", 1.0f, 0.01f, 0.0f, 10.0f),
        }});
        // toyengine/particles/: Unity-style modules + Blender's mesh emitter / scatter, keys as in
        // particle_yaml.h. Ranges are {x: min, y: max} (the parser also takes a scalar or [a, b]).
        {
            auto range = [](std::string k, glm::vec2 def, float speed, std::string tip, bool in_default = false) {
                return with_tip(f_vec2(std::move(k), def, speed, in_default), std::move(tip) + " -- random in [x, y]; x = y is a constant");
            };
            auto color_key = [](float t, glm::vec4 c) {
                Node n = Node::mapping();
                n["t"] = make_float(t);
                Node col = Node::mapping();
                col["r"] = make_float(c.r); col["g"] = make_float(c.g); col["b"] = make_float(c.b); col["a"] = make_float(c.a);
                n["color"] = col;
                return n;
            };
            auto value_key = [](float t, float v) {
                Node n = Node::mapping();
                n["t"] = make_float(t);
                n["value"] = make_float(v);
                return n;
            };
            Node fade = Node::sequence();
            fade.as_seq().push_back(color_key(0.0f, glm::vec4(1.0f)));
            fade.as_seq().push_back(color_key(1.0f, glm::vec4(1.0f, 1.0f, 1.0f, 0.0f)));
            Node grow = Node::sequence();
            grow.as_seq().push_back(value_key(0.0f, 1.0f));
            grow.as_seq().push_back(value_key(1.0f, 1.0f));
            FieldDesc mesh_material = f_material();
            mesh_material.in_default = false;
            mesh_material.tooltip = "Render mode `mesh`: the instanced mesh's material (opaque / cutout)";
            add({"ParticleSystem", "Effects", {
                // Main
                with_tip(f_enum("mode", {"emitter", "scatter"}, true),
                         "emitter: born over time and die (Unity); scatter: `count` placed once over the shape, never moving (Blender hair as instances)"),
                f_float("duration", 5.0f, 0.05f, 0.01f, 1e4f),
                f_bool("looping", true),
                with_tip(f_bool("prewarm", false), "Start as if it had already run one full duration"),
                f_float("start_delay", 0.0f, 0.01f, 0.0f, 1e4f),
                f_bool("play_on_start", true),
                range("start_lifetime", glm::vec2(1.5f, 2.5f), 0.01f, "Seconds", true),
                range("start_speed", glm::vec2(1.0f, 1.5f), 0.01f, "m/s along the shape's direction (a mesh's normal)", true),
                range("start_size", glm::vec2(0.2f, 0.35f), 0.005f, "Metres (quad height / mesh scale)", true),
                range("start_rotation", glm::vec2(0.0f, 0.0f), 0.5f, "Degrees"),
                f_color4("start_color", glm::vec4(1.0f), true),
                with_tip(f_color4("start_color_b", glm::vec4(1.0f)), "Each particle takes a random mix of start_color and this"),
                with_tip(f_float("gravity", 0.0f, 0.01f, -100.0f, 100.0f), "Multiplier on 9.81 m/s^2 down; negative rises (hot air)"),
                with_tip(f_enum("simulation_space", {"world", "local"}), "world: particles stay where born (trails); local: they move with the object"),
                with_tip(f_int("max_particles", 1000, 0, 1000000), "Pool cap; a GPU system's fixed buffer size"),
                with_tip(f_enum("simulation", {"cpu", "gpu"}), "gpu: emit / simulate / sort in compute shaders (100k+ particles); sub emitters, scatter and mesh mode fall back to cpu"),
                with_tip(f_int("seed", 0, 0, 2147483647), "0: derived from the object's name"),
                f_float("time_scale", 1.0f, 0.01f, 0.0f, 100.0f),
                // Emission
                with_tip(f_float("rate", 10.0f, 0.1f, 0.0f, 1e6f, true), "Particles per second"),
                with_tip(f_float("rate_over_distance", 0.0f, 0.1f, 0.0f, 1e6f), "Per metre the emitter travels"),
                with_tip(f_items("bursts", {f_float("time", 0.0f, 0.01f, 0.0f, 1e4f), f_vec2("count", glm::vec2(10.0f, 10.0f), 0.2f),
                                            f_int("cycles", 1, 0, 100000), f_float("interval", 0.5f, 0.01f, 0.001f, 1e4f),
                                            f_float("probability", 1.0f, 0.01f, 0.0f, 1.0f)}),
                         "Timed bursts; cycles 0 repeats every interval forever"),
                with_tip(f_int("count", 100, 0, 1000000), "Scatter mode: instances placed over the shape"),
                // Shape
                f_enum("shape", {"cone", "sphere", "hemisphere", "box", "circle", "edge", "point", "mesh", "terrain"}, true),
                f_float("radius", 0.5f, 0.005f, 0.0f, 1e4f),
                with_tip(f_float("radius_thickness", 1.0f, 0.01f, 0.0f, 1.0f), "0: born on the shell only; 1: anywhere inside"),
                with_tip(f_float("angle", 25.0f, 0.2f, 0.0f, 179.0f), "Cone half-angle (degrees)"),
                f_float("arc", 360.0f, 1.0f, 0.0f, 360.0f),
                f_vec3("box", glm::vec3(1.0f), 0.01f),
                f_float("length", 1.0f, 0.01f, 0.0f, 1e4f),
                f_vec3("shape_offset", glm::vec3(0.0f), 0.01f),
                with_tip(f_float("random_direction", 0.0f, 0.01f, 0.0f, 1.0f), "Blend each direction toward a random one"),
                with_tip(f_asset("mesh_path", "meshes", ".yaml", true), "Shape `mesh`: the emitter surface; empty uses this object's MeshRenderer"),
                f_enum("emit_from", {"faces", "vertices", "edges"}),
                with_tip(f_enum("distribution", {"random", "even"}), "even: stratified by area -- no clumps or bald patches (Blender's Jittered)"),
                with_tip(f_float("normal_offset", 0.0f, 0.001f, -10.0f, 10.0f), "Push mesh spawn points off the surface (m)"),
                with_tip(f_bool("align_to_normal", false), "Orient each particle to the emission normal (aligned sprites, instanced meshes)"),
                with_tip(f_bool("random_spin", true), "With align_to_normal: random twist about the normal"),
                f_float("inherit_velocity", 0.0f, 0.01f, -10.0f, 10.0f),
                // Velocity / forces / noise
                f_vec3("velocity", glm::vec3(0.0f), 0.01f),
                with_tip(f_vec3("force", glm::vec3(0.0f), 0.01f), "Constant acceleration, world space (wind, buoyancy)"),
                with_tip(f_float("drag", 0.0f, 0.01f, 0.0f, 100.0f), "Linear damping (1/s)"),
                with_tip(f_float("wind_influence", 0.0f, 0.01f, 0.0f, 4.0f),
                         "How strongly the weather's wind carries the particles: they are pushed downwind until they move at this "
                         "fraction of the wind (0 ignores it; smoke ~0.6, embers ~0.8, sparks ~0.3)"),
                with_tip(f_float("orbital", 0.0f, 0.01f, -100.0f, 100.0f), "Swirl about the emitter's +Z axis (rad/s)"),
                f_float("radial", 0.0f, 0.01f, -100.0f, 100.0f),
                with_tip(f_float("tumble", 0.0f, 1.0f, -1e4f, 1e4f), "3D tumble about a random axis (deg/s)"),
                range("angular_velocity", glm::vec2(0.0f, 0.0f), 0.5f, "In-plane spin, deg/s"),
                with_tip(f_float("noise_strength", 0.0f, 0.01f, 0.0f, 1000.0f), "Curl-noise turbulence (m/s^2 RMS)"),
                f_float("noise_frequency", 0.5f, 0.01f, 0.001f, 100.0f),
                f_float("noise_scroll", 0.5f, 0.01f, 0.0f, 100.0f),
                f_int("noise_octaves", 2, 1, 3),
                // Over lifetime
                with_tip(f_items("color_over_life", {f_float("t", 0.0f, 0.01f, 0.0f, 1.0f), f_color4("color", glm::vec4(1.0f))}, fade, true),
                         "Multiplies start colour over normalized life (scatter: per-instance variety)"),
                with_tip(f_items("size_over_life", {f_float("t", 0.0f, 0.01f, 0.0f, 1.0f), f_float("value", 1.0f, 0.01f, 0.0f, 1e4f)}, grow),
                         "Multiplies start size over normalized life"),
                with_tip(f_items("alpha_over_life", {f_float("t", 0.0f, 0.01f, 0.0f, 1.0f), f_float("value", 1.0f, 0.01f, 0.0f, 1.0f)}),
                         "Extra alpha multiplier over life"),
                // Collision
                with_tip(f_bool("collide", false), "Collide with a ground plane at ground_height (world space systems)"),
                f_float("ground_height", 0.0f, 0.01f),
                f_float("bounce", 0.3f, 0.01f, 0.0f, 1.0f),
                f_float("collision_friction", 0.2f, 0.01f, 0.0f, 1.0f),
                f_bool("kill_on_collide", false),
                with_tip(f_bool("on_death_collision_only", false), "Sub emitters fire only when a particle dies by hitting the ground"),
                with_tip(f_vec3("wrap_box", glm::vec3(0.0f), 0.1f), "World space: particles leaving this box (full extents, centred on the "
                                                                    "emitter) wrap back in on the other side -- precipitation that keeps up with a "
                                                                    "moving camera. 0 on an axis: no wrap"),
                with_tip(f_float("wrap_fade", 0.15f, 0.01f, 0.0f, 1.0f), "Alpha fades over this fraction of the wrap box's half width at its sides"),
                with_tip(f_items("on_death", {f_string("target"), f_vec2("count", glm::vec2(1.0f, 1.0f), 0.1f),
                                              f_float("inherit_velocity", 0.0f, 0.01f, -10.0f, 10.0f)}),
                         "Sub emitters: each death spawns into the named object's ParticleSystem"),
                // Renderer
                with_tip(f_enum("render_mode", {"billboard", "stretched", "horizontal", "vertical", "aligned", "mesh", "none"}, true),
                         "billboard: faces the camera; stretched: along velocity; vertical: upright (flames); aligned: in the particle's own plane"),
                f_enum("sprite", {"soft", "circle", "puff", "flame", "spark", "ring", "star", "leaf", "texture"}, true),
                with_tip(f_float("additive", 0.0f, 0.01f, 0.0f, 1.0f), "0 alpha blend .. 1 additive glow"),
                with_tip(f_float("lit", 0.0f, 0.01f, 0.0f, 1.0f), "How much sun / sky / point lights shade it (smoke 1, fire 0)"),
                with_tip(f_float("toon_bands", 0.0f, 0.1f, 0.0f, 16.0f), "> 1: lighting and flame cores in this many bands"),
                with_tip(f_float("emissive", 1.0f, 0.01f, 0.0f, 100.0f), "HDR multiplier; > ~1.4 blooms"),
                with_tip(f_bool("receive_shadows", true), "Lit particles darken in the sun's and shadowed lamps' shadows"),
                with_tip(f_float("scatter", 0.0f, 0.01f, 0.0f, 16.0f), "Light scattered forward toward the eye -- rain glinting against a "
                                                                    "lamp or a low sun, backlit smoke; 0 off"),
                with_tip(f_float("scatter_anisotropy", 0.75f, 0.01f, -0.95f, 0.95f), "How forward the scattering is: 0 even, ~0.9 a tight glow toward lights"),
                with_tip(f_float("reactive", 0.0f, 0.01f, 0.0f, 1.0f), "TAA: trust this particle's frame over the history under it, so fast thin "
                                                                     "particles (rain, snow, sparks) never smear into streaks. 1 for precipitation"),
                with_tip(f_float("softness", 0.5f, 0.01f, 0.0f, 1.0f), "Sprite edge: 0 crisp cel edge .. 1 feathered"),
                with_tip(f_float("soft_distance", 0.4f, 0.01f, 0.0f, 100.0f), "Fade where it meets opaque geometry (m)"),
                f_float("camera_fade", 0.3f, 0.01f, 0.0f, 100.0f),
                with_tip(f_float("aspect", 1.0f, 0.01f, 0.01f, 100.0f), "Quad width / height"),
                with_tip(f_float("pivot", 0.0f, 0.01f, -10.0f, 10.0f), "Shift along the quad's up, in sizes"),
                f_float("stretch_speed", 0.05f, 0.001f, 0.0f, 10.0f),
                f_float("stretch_length", 1.0f, 0.01f, 0.0f, 100.0f),
                with_tip(f_float("distortion", 0.5f, 0.01f, 0.0f, 4.0f), "Noise breakup of puff / flame / leaf edges"),
                f_float("opacity", 1.0f, 0.01f, 0.0f, 1.0f),
                with_tip(f_asset("texture", "textures", ".png"), "Sprite `texture`: albedo map (or flipbook atlas)"),
                with_tip(f_vec2("flipbook", glm::vec2(1.0f, 1.0f), 0.1f), "Atlas columns (x) and rows (y)"),
                f_enum("flipbook_mode", {"lifetime", "random", "fps"}),
                f_float("flipbook_fps", 12.0f, 0.1f, 0.0f, 240.0f),
                f_enum("sort", {"distance", "none", "oldest", "youngest"}),
                with_tip(f_float("max_draw_distance", 0.0f, 0.5f, 0.0f, 1e6f), "Cull beyond this from the camera (m); 0 never (terrain scatter: automatic)"),
                with_tip(f_bool("cast_shadows", true), "Mesh mode: false keeps the instances out of the shadow passes (grass, clutter)"),
                with_tip(f_asset("render_mesh", "meshes", ".yaml", true), "Render mode `mesh`: the instanced mesh"),
                mesh_material,
            }, false});
            add({"LightFlicker", "Effects", {
                with_tip(f_float("amount", 0.3f, 0.01f, 0.0f, 4.0f, true), "Fraction of the sibling PointLight's intensity it swings by"),
                f_float("speed", 6.0f, 0.05f, 0.0f, 100.0f, true),
                with_tip(f_float("wobble", 0.0f, 0.005f, 0.0f, 10.0f), "Positional jitter (m)"),
                with_tip(f_color("color_shift", glm::vec3(0.0f)), "Added to the light colour at bright peaks"),
            }});
        }
        // toyengine/weather/: which surfaces rain splashes on, and landings shown far off.
        add({"WeatherSurface", "Effects", {
            with_tip(f_bool("splashes", true, true), "Rain splashes / sprays and snow settles on this object and its children "
                                                    "(rain still stops on every surface; unmarked ones take it silently)"),
        }});
        // toyengine/world/snow_system.h: presses trails into deep snow.
        add({"SnowDeformer", "Effects", {
            with_tip(f_float("radius", 0.35f, 0.01f, 0.0f, 20.0f, true), "Radius of the disc pressed into the snow (m)"),
            with_tip(f_float("depth", -1.0f, 0.01f, -1.0f, 10.0f, true), "Metres pressed in; -1: down to the object's base"),
            with_tip(f_float("falloff", 0.5f, 0.01f, 0.0f, 1.0f), "0 hard-edged .. 1 a soft bowl"),
        }});
        add({"WeatherDistantLandings", "Effects", {
            with_tip(f_float("radius", 45.0f, 0.5f, 0.0f, 500.0f, true), "Landings (splashes) are shown out to this distance from the camera, "
                                                                        "past the drops' own wrap box"),
            with_tip(f_strings("targets"), "Which of the sibling ParticleSystem's on_death targets fire far off; empty = all"),
        }});
        // toyengine/env/: a local environmental influence -- a rotor's downwash, a vent, a wet
        // patch -- added to (or overriding) the global weather. See toyengine/env/README.md.
        add({"EnvInfluence", "Effects", {
            with_tip(f_bool("enabled", true), "Off contributes nothing at all (a switchable rotor)"),
            with_tip(f_enum("shape", {"sphere", "disc"}, true),
                     "sphere: a ball about the object. disc: a cylinder about its +Z axis -- a rotor's "
                     "column, a vent's plume"),
            with_unit(with_tip(f_float("radius", 6.0f, 0.05f, 0.0f, 200.0f, true),
                               "How far it reaches. The influence is exactly zero at and beyond this"), "m"),
            with_unit(with_tip(shown_when(f_float("height", 0.0f, 0.05f, 0.0f, 200.0f), "shape", {"disc"}),
                               "Disc only: how far the column extends either side of the object. 0 = unbounded"), "m"),
            with_tip(as_slider(f_float("falloff", 0.5f, 0.01f, 0.0f, 1.0f, true)),
                     "0 a hard edge .. 1 a soft bowl. Full strength inside radius x (1 - falloff)"),
            with_tip(f_enum("mode", {"directional", "radial"}, true),
                     "directional: one wind vector everywhere inside. radial: aimed away from the +Z "
                     "axis, so one source is both a downwash and an updraft"),
            with_tip(shown_when(f_vec3("wind", glm::vec3(0.0f), 0.05f), "mode", {"directional"}),
                     "The wind inside the shape (m/s). A sheltered pocket is this at zero with blend "
                     "set to override"),
            with_unit(with_tip(shown_when(f_float("outward", 0.0f, 0.05f, -200.0f, 200.0f, true), "mode", {"radial"}),
                               "How hard it blows away from the axis, at the rim"), "m/s"),
            with_unit(with_tip(shown_when(f_float("vertical", 0.0f, 0.05f, -200.0f, 200.0f, true), "mode", {"radial"}),
                               "Along +Z. NEGATIVE is a downwash (a rotor presses foliage flat); positive "
                               "lifts (a vent)"), "m/s"),
            with_tip(as_slider(f_float("turbulence", 0.0f, 0.01f, 0.0f, 1.0f)),
                     "Extra foliage flutter and particle noise inside the shape"),
            with_tip(as_slider(f_float("wetness", 0.0f, 0.01f, -1.0f, 1.0f)),
                     "Surface wetness added here -- a burst pipe, a spray zone. Negative is a local dry "
                     "zone. Shows on terrain, and on any material with `wet: true`"),
            with_unit(with_tip(f_float("temperature", 0.0f, 0.1f, -200.0f, 200.0f),
                               "Offset on the ambient temperature. Gameplay only -- nothing is drawn from it"), "C"),
            with_tip(as_slider(f_float("snow", 0.0f, 0.01f, 0.0f, 1.0f)),
                     "Lies snow here whatever the weather is doing -- a drift against a wall, a patch "
                     "the sun never reaches. Combined with the weather's own cover by max(), so it "
                     "works in a scene that never snows"),
            with_tip(as_slider(f_float("snow_clear", 0.0f, 0.01f, 0.0f, 1.0f)),
                     "How hard it scours lying snow, as a fraction of the full depth. Tracks drift back "
                     "in like a SnowDeformer's"),
            with_tip(as_slider(f_float("water_disturb", 0.0f, 0.01f, 0.0f, 1.0f)),
                     "Ripple rings on water beneath it -- a rotor churning a pond"),
            with_tip(f_enum("blend", {"add", "max", "override"}),
                     "add: on top of the weather and other sources. max: only the strongest overlapping "
                     "one counts. override: replaces everything inside the shape"),
            with_tip(as_slider(f_float("strength", 1.0f, 0.01f, 0.0f, 1.0f)),
                     "Master scale on every channel above. 0 costs nothing"),
        }});
        // toyengine/env/: a one-shot influence fired by an animation event -- a sword swing.
        add({"EnvPulseEmitter", "Effects", {
            with_tip(f_string("event", "", true),
                     "The Animator clip event name that fires it. Empty: code only (call fire())"),
            with_tip(f_vec3("offset", glm::vec3(0.0f), 0.01f),
                     "Where the pulse is born, in the object's own space -- a blade's tip"),
            with_unit(with_tip(f_float("radius", 2.5f, 0.05f, 0.0f, 200.0f, true), "How far the gust reaches"), "m"),
            with_tip(as_slider(f_float("falloff", 0.6f, 0.01f, 0.0f, 1.0f)), "0 a hard edge .. 1 a soft bowl"),
            with_tip(f_enum("mode", {"radial", "directional"}, true),
                     "radial: a gust blown outward from the point. directional: one vector"),
            with_unit(with_tip(shown_when(f_float("outward", 9.0f, 0.05f, -200.0f, 200.0f, true), "mode", {"radial"}),
                               "How hard it blows outward at the rim"), "m/s"),
            with_unit(with_tip(shown_when(f_float("vertical", 0.0f, 0.05f, -200.0f, 200.0f), "mode", {"radial"}),
                               "Along +Z. Negative presses down"), "m/s"),
            with_tip(shown_when(f_vec3("wind", glm::vec3(0.0f), 0.05f), "mode", {"directional"}), "The gust (m/s)"),
            with_tip(as_slider(f_float("turbulence", 0.8f, 0.01f, 0.0f, 1.0f)), "Extra flutter while it lasts"),
            with_unit(with_tip(f_float("attack", 0.05f, 0.01f, 0.0f, 10.0f), "Seconds from nothing to full"), "s"),
            with_unit(with_tip(f_float("hold", 0.05f, 0.01f, 0.0f, 10.0f), "Seconds at full strength"), "s"),
            with_unit(with_tip(f_float("decay", 0.35f, 0.01f, 0.0f, 10.0f),
                               "Seconds back to nothing -- what makes foliage snap and then recover"), "s"),
            with_tip(as_slider(f_float("strength", 1.0f, 0.01f, 0.0f, 1.0f)), "Master scale on every channel"),
        }});
        // toyengine/fire/: an object that catches fire at a point and burns across itself.
        add({"Burnable", "Effects", {
            with_unit(with_tip(f_float("spread_rate", 0.25f, 0.01f, 0.0f, 20.0f, true),
                               "How fast the burn front travels out over the surface"), "m/s"),
            with_unit(with_tip(f_float("burn_time", 14.0f, 0.5f, 0.1f, 3600.0f, true),
                               "Seconds from ignition until it is consumed"), "s"),
            with_tip(f_enum("on_burnt", {"char", "remove"}),
                     "char: it stays, blackened. remove: it is switched off once consumed"),
            with_unit(with_tip(f_float("overshoot", 0.3f, 0.05f, 0.0f, 10.0f),
                               "How far past its own extent the front runs, so the last corner burns"), "m"),
            with_tip(f_bool("ignited", false), "Already alight when the scene loads"),
        }});
        // toyengine/fire/: opts a scene into click-to-ignite.
        add({"FireIgniter", "Effects", {
            with_tip(f_enum("button", {"left", "right", "middle"}), "Mouse button that lights what it hits"),
            with_tip(f_string("reset_key", "R", true), "Key that puts every Burnable back; empty disables it"),
            with_unit(with_tip(f_float("max_distance", 1000.0f, 1.0f, 0.1f, 10000.0f), "How far the pick ray reaches"), "m"),
        }});
        // toyengine/env/: a panel of UI buttons switching between authored environment presets.
        // `presets` is a list of maps, which no generic widget can edit -- the inspector shows
        // the scalars and the YAML carries the list.
        add({"EnvPresetSwitcher", "Effects", {
            with_tip(f_string("start", "", true),
                     "Name of the preset live on load. Empty: the first one in the list"),
            with_tip(f_string("label_object", ""),
                     "Optional Text widget shown the active preset's one-line blurb"),
        }});
        // toyengine/weather/: switches lights, effects or children with the time of day / weather.
        add({"WeatherReactor", "Effects", {
            with_tip(f_vec2("hours", glm::vec2(18.0f, 6.0f), 0.05f), "On between these hours (x from, y to; wraps past midnight). "
                                                                       "Unset: any hour"),
            with_tip(f_strings("phases"), "On in these parts of the day: night, dawn, day, dusk. Empty: any"),
            with_tip(f_strings("conditions"), "On while the weather is (moving to) one of these conditions. Empty: any"),
            with_tip(f_float("min_precipitation", 0.0f, 0.01f, 0.0f, 1.0f), "On only with at least this much rain / snow"),
            with_tip(f_bool("invert", false), "On when the rule is NOT met (a fire that is out in the rain)"),
            with_tip(f_strings("target", {"lights", "effects"}, true), "What switches: lights (fade point / spot lights here and below), "
                                                                       "effects (stop / play particle systems here and below), children (set active)"),
            with_tip(f_float("fade", 1.5f, 0.05f, 0.0f, 60.0f, true), "Seconds lights take to fade"),
        }});
        // sfxcoopa (toyengine/audio/): a positioned or 2D sound, the ears (else the main camera
        // hears), and a settings-menu slider bound to a mixer bus.
        add({"AudioSource", "Audio", {
            f_asset("clip", "audio", ".wav", false, false, "", true),
            f_enum("bus", {"SFX", "Music", "UI", "Master"}, true),
            f_float("volume", 1.0f, 0.01f, 0.0f, 4.0f, true),
            f_float("pitch", 1.0f, 0.01f, 0.05f, 8.0f),
            f_bool("loop", false, true),
            f_bool("play_on_start", true, true),
            with_tip(f_bool("spatialize", false, true), "Positioned in 3D at this object (needs a mono clip: Force Mono in its import settings)"),
            f_float("min_distance", 1.0f, 0.05f, 0.0f, 10000.0f),
            f_float("max_distance", 50.0f, 0.5f, 0.0f, 100000.0f),
            f_enum("curve", {"Inverse", "Linear", "Logarithmic"}),
            f_int("priority", 0, -100, 100),
        }, false});
        add({"AudioListener", "Audio", {
            f_bool("track_velocity", true),
        }});
        // toyengine/audio/music_player.h: background music through AudioSystem::music().
        add({"PanelScreen", "UI", {
            with_tip(f_asset("panel", "ui", ".yaml", false, false, "", true),
                     "The authored panel asset -- tabs, rows and buttons (toyengine/ui/ui_panel.h)"),
            with_tip(f_string("theme_ref", "ui/themes/game_dark", true), "Theme styling the panel"),
            with_tip(f_string("font_ref", ""), "Font, relative to an asset root; empty uses the stock face"),
            with_tip(f_int("sort_order", 0, -10000, 10000), "Canvas draw order; higher draws on top"),
            with_tip(f_string("tunables_ref", ""),
                     "Placement overrides (.tune.yaml); empty derives one from the panel asset"),
            with_tip(f_bool("ui_sounds", true), "Play the hover/click cues"),
            with_tip(f_bool("hidden", false), "Start hidden, for a screen something else opens"),
        }});

        // The stock screen-space HUD. Values are editable so a capture can be posed, and
        // `animate` drives them instead when it is on.
        add({"DemoHud", "UI", {
            f_float("health", 72.0f, 1.0f, 0.0f, 100000.0f, true),
            f_float("health_max", 100.0f, 1.0f, 1.0f, 100000.0f, true),
            f_float("stamina", 48.0f, 1.0f, 0.0f, 100000.0f),
            f_float("stamina_max", 100.0f, 1.0f, 1.0f, 100000.0f),
            f_float("mana", 30.0f, 1.0f, 0.0f, 100000.0f),
            f_float("mana_max", 100.0f, 1.0f, 1.0f, 100000.0f),
            with_tip(f_string("quest_title", ""), "Quest line, top right; empty draws nothing"),
            f_string("quest_step", ""),
            with_tip(f_int("hotbar_slots", 6, 1, 12), "Cells along the bottom"),
            with_tip(f_bool("animate", true), "Drift the vitals, so a still frame shows partly-empty bars"),
            with_tip(f_string("theme_ref", "ui/themes/game_dark", true), "Theme styling the HUD"),
            with_tip(f_string("font_ref", ""), "Font, relative to an asset root; empty uses the stock face"),
            with_tip(f_int("sort_order", 0, -10000, 10000), "Canvas draw order; higher draws on top"),
            with_tip(f_string("tunables_ref", ""), "Placement overrides (.tune.yaml); empty derives one from the type name"),
            with_tip(f_bool("ui_sounds", true), "Play the hover/click cues"),
            f_bool("hidden", false),
        }});

        // A plate in the 3D world: a nameplate, a sign, a label on a machine. Parent the
        // object to the thing it labels and the plate follows it.
        add({"WorldLabel", "UI", {
            with_tip(f_string("title", "", true), "The line drawn on the plate"),
            f_string("subtitle", ""),
            with_tip(f_float("bar_value", 0.0f, 1.0f, 0.0f, 100000.0f), "Bar under the text; a max of 0 draws no bar"),
            f_float("bar_max", 0.0f, 1.0f, 0.0f, 100000.0f),
            with_tip(f_string("bar_role", "health"), "Which themed colour the bar takes (bars: section)"),
            with_tip(f_bool("backdrop", true), "Paint the plate behind the text"),
            with_tip(f_string("theme_ref", "ui/themes/game_dark", true), "Theme styling the plate"),
            with_tip(f_string("font_ref", ""), "Font, relative to an asset root; empty uses the stock face"),
            with_tip(f_vec2("world_size", glm::vec2(420.0f, 60.0f), 1.0f), "Plate size in canvas pixels"),
            with_tip(f_float("pixels_per_unit", 70.0f, 1.0f, 1.0f, 1000.0f), "Canvas pixels per world unit"),
            with_tip(f_float("world_text_supersample", 2.0f, 0.1f, 1.0f, 4.0f),
                     "Extra glyph-atlas density; a world plate's on-screen size is not known in advance"),
            with_tip(f_bool("world_occlude", true), "Hide the plate behind geometry in front of it"),
            with_tip(f_enum("billboard", {"CameraFacing", "Transform"}),
                     "CameraFacing turns to the camera; Transform keeps the object's rotation"),
            f_bool("hidden", false),
        }});

        // A line or two pinned to a corner: controls, a reminder.
        add({"ScreenHint", "UI", {
            with_tip(f_string("title", "", true), "Heading, in the accent colour"),
            with_tip(f_strings("lines"), "One line each, under the title"),
            with_tip(f_enum("anchor", {"TopLeft", "TopCenter", "TopRight", "MiddleLeft", "Center",
                                       "MiddleRight", "BottomLeft", "BottomCenter", "BottomRight"}),
                     "Which corner it sits in"),
            with_tip(f_vec2("offset", glm::vec2(0.0f), 1.0f),
                     "Moves the box from that corner, in UI space (+Y down), so a right or bottom "
                     "anchor insets with a negative value"),
            with_tip(f_bool("backdrop", true), "Paint a panel behind the text"),
            with_tip(f_string("theme_ref", "ui/themes/game_dark", true), "Theme styling the hint"),
            with_tip(f_string("font_ref", ""), "Font, relative to an asset root; empty uses the stock face"),
            with_tip(f_int("sort_order", 0, -10000, 10000), "Canvas draw order; higher draws on top"),
            with_tip(f_string("tunables_ref", ""), "Placement overrides (.tune.yaml); empty derives one from the type name"),
            f_bool("hidden", false),
        }});

        add({"HealthDriver", "Gameplay", {
            with_tip(f_string("label", "", true), "Name drawn above the bar; empty draws the bar alone"),
            f_float("max_health", 100.0f, 1.0f, 0.0f, 100000.0f, true),
            f_float("start_health", 100.0f, 1.0f, 0.0f, 100000.0f, true),
            with_tip(f_float("damage_per_second", 22.0f, 0.5f, 0.0f, 10000.0f, true),
                     "Drain rate while draining; 0 freezes the demo cycle"),
            f_float("regen_per_second", 14.0f, 0.5f, 0.0f, 10000.0f, true),
            with_tip(as_slider(f_float("turnaround_fraction", 0.15f, 0.01f, 0.0f, 1.0f)),
                     "Fraction of max the cycle turns around at, so the bar never bottoms out"),
            f_bool("show_heal_button", false),
            with_tip(f_string("theme_ref", "ui/themes/game_dark"), "Theme styling the plate"),
            with_tip(f_vec2("world_size", glm::vec2(150.0f, 48.0f), 1.0f),
                     "The plate's design size in CANVAS PIXELS, not world units"),
            with_tip(f_float("pixels_per_unit", 110.0f, 1.0f, 1.0f, 2000.0f),
                     "world_size / this = the plate's extent in metres"),
            with_tip(f_float("world_text_supersample", 2.1f, 0.05f, 0.5f, 4.0f),
                     "Glyph density; a world plate has no fixed screen scale"),
            with_tip(f_bool("world_occlude", false), "Let scene geometry hide the plate"),
            f_bool("hidden", false),
        }});

        add({"AudioDemoPanel", "Audio", {
            with_tip(f_string("playlist_object", "", true),
                     "Object carrying the MusicPlaylist the Restart button drives"),
            with_tip(f_string("theme_ref", "ui/themes/game_dark", true),
                     "Theme styling the panel; empty uses the built-in look"),
            with_tip(f_string("font_ref", ""), "Font, relative to an asset root; empty uses the stock face"),
            f_int_enum("start_page", {"Music", "Spatial", "UI Sounds", "Mixer"}, 0),
            f_float("fade_seconds", 2.5f, 0.05f, 0.0f, 10.0f),
            with_tip(f_int("sort_order", 0, -10000, 10000), "Canvas draw order; higher draws on top"),
            f_bool("hidden", false),
        }});

        add({"MusicPlaylist", "Audio", {
            with_tip(f_items("tracks", {
                f_string("name"),
                f_asset("clip", "audio", ".wav", false, false, "", true),
                f_float("volume", 1.0f, 0.01f, 0.0f, 4.0f),
            }), "Played in order, crossfading track to track; a track already playing carries on"),
            f_float("crossfade", 3.0f, 0.05f, 0.0f, 60.0f, true),
            f_bool("shuffle", false),
            with_tip(f_bool("repeat", true), "Wrap after the last track (else the music ends)"),
            with_tip(f_int("start", -1, -1, 1000), "First track index; -1: the first (random when shuffled)"),
            f_bool("play_on_start", true),
            with_tip(f_bool("stop_on_destroy", false), "Fade the music out when this object goes (else it plays into the next scene)"),
        }});
        add({"MusicZone", "Audio", {
            with_tip(f_string("track", "", true), "Track name or clip path played over the scene music while the listener is inside"),
            with_tip(f_vec3("size", glm::vec3(10.0f, 10.0f, 6.0f), 0.05f, true), "Box extents, centred on the object (times its scale)"),
            with_tip(f_float("radius", 0.0f, 0.05f, 0.0f, 100000.0f), "> 0: a sphere instead of the box"),
            f_float("fade", 2.0f, 0.05f, 0.0f, 60.0f, true),
        }});
        add({"CameraController", "Gameplay", {
            f_enum("mode", {"Orbit", "Fly", "FirstPerson"}, true),
            with_tip(f_string("tracker"), "Name of an object to follow (Orbit) or ride (FirstPerson); empty uses Target"),
            f_vec3("target", glm::vec3(0.0f), 0.02f),
            with_tip(f_float("distance", -1.0f, 0.05f, -1.0f, 10000.0f), "-1: from the camera's placement relative to the target"),
            with_tip(f_float("yaw_deg", 0.0f, 0.5f), "Unset: from the camera's placement relative to the target"),
            with_tip(f_float("pitch_deg", 0.0f, 0.5f, -89.0f, 89.0f), "Unset: from the camera's placement relative to the target"),
            with_tip(f_float("follow_smoothing", 10.0f, 0.1f, 0.0f, 200.0f),
                     "How fast the camera chases what it tracks (1/s): low trails behind it, high is rigid, 0 snaps"),
            with_tip(f_float("min_pitch_deg", 0.0f, 0.5f, -89.0f, 89.0f), "Lower elevation clamp; equal to max pins the angle"),
            with_tip(f_float("max_pitch_deg", 85.0f, 0.5f, -89.0f, 89.0f), "Upper elevation clamp"),
            f_float("min_distance", 0.5f, 0.05f, 0.0f, 10000.0f),
            f_float("max_distance", 100.0f, 0.5f, 0.0f, 100000.0f),
            f_bool("invert_x", false),
            f_bool("invert_y", false),
            f_float("mouse_sensitivity", 0.15f, 0.005f, 0.0f, 10.0f),
            f_float("zoom_speed", 1.0f, 0.01f, 0.0f, 100.0f),
            f_bool("capture_cursor", false, true),
            with_tip(f_enum("look_button", {"", "left", "right", "middle"}),
                     "Hold this mouse button to turn the camera; empty turns continuously"),
            f_float("auto_rotate_deg_per_sec", 0.0f, 0.1f),
            with_tip(f_float("key_orbit_speed_deg_per_sec", 0.0f, 0.5f, 0.0f, 3600.0f),
                     "Orbit: WASD / arrow keys turn the camera at this rate; 0 leaves the keys alone"),
            f_float("move_speed", 5.0f, 0.05f, 0.0f, 1000.0f),
            f_float("movement_smoothing", 0.0f, 0.005f, 0.0f, 1.0f),
            with_tip(f_bool("collide", true), "Orbit with a tracker: pull in when geometry blocks the view"),
            f_float("collision_radius", 0.2f, 0.005f, 0.0f, 10.0f),
            with_tip(f_float("collision_in_speed", 25.0f, 0.1f, 0.0f, 1000.0f), "Pull-in rate (1/s); 0 snaps"),
            with_tip(f_float("collision_out_speed", 4.0f, 0.1f, 0.0f, 1000.0f), "Recovery rate (1/s); 0 snaps"),
            with_tip(f_float("eye_height", 1.6f, 0.01f, -100.0f, 100.0f), "FirstPerson: camera height above the tracked object's origin"),
            f_float("first_person_pitch_limit", 85.0f, 0.5f, 0.0f, 89.0f),
        }});
        // Defaults mirror toyengine/scene/character_controller.h.
        add({"CharacterController", "Gameplay", {
            f_float("radius", 0.3f, 0.005f, 0.01f, 10.0f, true),
            with_tip(f_float("height", 1.8f, 0.01f, 0.02f, 20.0f, true), "Total capsule height; the origin is at the feet"),
            with_tip(f_float("step_height", 0.35f, 0.005f, 0.0f, 5.0f), "Tallest ledge walked up without jumping"),
            with_tip(f_float("snap_distance", 0.3f, 0.005f, 0.0f, 5.0f),
                     "How far down a grounded character is pulled to stay on the ground -- step_height's "
                     "opposite number, and worth matching it so a drop of one step is walked rather than fallen"),
            with_tip(f_float("step_smoothing", 0.0f, 0.005f, 0.0f, 2.0f),
                     "Seconds a change of level is eased into visually; 0 applies it at once. Affects only the "
                     "drawn pose, never the motion"),
            with_tip(f_float("slope_limit", 45.0f, 0.5f, 0.0f, 89.0f), "Degrees; steeper slopes are walls"),
            f_float("skin", 0.02f, 0.001f, 0.001f, 0.5f),
            f_float("move_speed", 4.0f, 0.05f, 0.0f, 1000.0f, true),
            f_float("sprint_multiplier", 1.8f, 0.01f, 0.0f, 100.0f),
            with_tip(f_float("acceleration", 30.0f, 0.1f, 0.0f, 10000.0f), "m/s^2 toward the target speed"),
            with_tip(f_float("air_control", 0.3f, 0.005f, 0.0f, 1.0f), "Fraction of the acceleration available in the air"),
            f_float("gravity_scale", 1.0f, 0.01f, 0.0f, 100.0f),
            with_tip(f_float("jump_height", 1.2f, 0.01f, 0.0f, 100.0f), "Apex height of a jump (m); 0 disables jumping"),
            f_float("coyote_time", 0.12f, 0.005f, 0.0f, 5.0f),
            f_float("jump_buffer", 0.12f, 0.005f, 0.0f, 5.0f),
            f_bool("face_movement", true),
            f_float("turn_speed", 720.0f, 1.0f, 0.0f, 100000.0f),
            f_bool("push_dynamic_bodies", true),
            with_tip(f_float("push_strength", 80.0f, 0.5f, 0.0f, 100000.0f), "Pushing mass (kg)"),
            with_tip(f_bool("use_root_motion", false), "Move by the Animator's root motion instead of input"),
            with_tip(f_bool("player_input", true), "Driven by the player's input; off for a body a Mount steers (a horse)"),
        }});
        // Defaults mirror toyengine/scene/mount.h.
        add({"Mount", "Gameplay", {
            with_tip(f_float("walk_speed", 1.45f, 0.01f, 0.0f, 100.0f, true), "m/s; the walk clip's own ground speed"),
            with_tip(f_float("trot_speed", 3.2f, 0.01f, 0.0f, 100.0f, true), "m/s; input alone trots"),
            with_tip(f_float("gallop_speed", 8.9f, 0.01f, 0.0f, 100.0f, true), "m/s; Shift gallops"),
            f_float("acceleration", 4.0f, 0.05f, 0.0f, 1000.0f),
            f_float("braking", 6.0f, 0.05f, 0.0f, 1000.0f),
            with_tip(f_float("turn_rate", 150.0f, 1.0f, 0.0f, 10000.0f), "Degrees/s standing or walking"),
            with_tip(f_float("gallop_turn_rate", 70.0f, 1.0f, 0.0f, 10000.0f), "Degrees/s at full gallop"),
            with_tip(f_float("mount_max_speed", 0.8f, 0.01f, 0.0f, 100.0f), "A rider gets on below this speed"),
            f_string("idle_state", "idle"),
            f_string("walk_state", "walk"),
            f_string("trot_state", "trot"),
            f_string("gallop_state", "gallop"),
            f_float("crossfade", 0.25f, 0.005f, 0.0f, 10.0f),
            f_float("min_anim_speed", 0.5f, 0.01f, 0.0f, 10.0f),
            f_float("max_anim_speed", 1.6f, 0.01f, 0.0f, 10.0f),
            with_tip(f_string("saddle", "body/saddle"), "Socket the rider sits on (path from this object)"),
            f_string("stirrup_l", "body/saddle/stirrup_l"),
            f_string("stirrup_r", "body/saddle/stirrup_r"),
            f_string("rein_l", "body/neck/rein_l"),
            f_string("rein_r", "body/neck/rein_r"),
            with_tip(f_string("mount_point_l", "mount_point_l"), "Ground-level spot beside the saddle to get on / off"),
            f_string("mount_point_r", "mount_point_r"),
            with_tip(f_float("camera_offset_z", 2.2f, 0.01f, -100.0f, 100.0f), "Camera target height while ridden"),
            with_tip(f_float("camera_distance", 6.5f, 0.05f, 0.0f, 1000.0f), "Camera distance while ridden"),
        }});
        // Defaults mirror toyengine/scene/rider.h.
        add({"Rider", "Gameplay", {
            with_tip(f_float("mount_radius", 1.6f, 0.01f, 0.0f, 100.0f, true), "Reach (m) from the feet to a mount point; F gets on / off"),
            f_float("mount_time", 0.8f, 0.01f, 0.0f, 10.0f),
            f_float("dismount_time", 0.6f, 0.01f, 0.0f, 10.0f),
            f_string("ride_state", "ride", true),
            f_float("crossfade", 0.3f, 0.005f, 0.0f, 10.0f),
            with_tip(f_vec3("seat_offset", glm::vec3(0.0f, -0.05f, -0.86f), 0.005f), "The rider's root from the saddle socket (saddle frame)"),
            with_tip(f_float("knee_spread", 0.45f, 0.005f, 0.0f, 5.0f), "Knee poles out from the hips (m)"),
            with_tip(f_float("lean", 0.15f, 0.005f, 0.0f, 5.0f), "Spine lean forward at full gallop (m)"),
            f_float("leg_weight", 1.0f, 0.01f, 0.0f, 1.0f),
            f_float("arm_weight", 1.0f, 0.01f, 0.0f, 1.0f),
            with_tip(f_int("priority", 100, -1000, 1000), "IK driver order: after the mount's FootIK (lower first)"),
            with_tip(f_string("mount_on_start", ""), "A Mount's object name: start the scene in its saddle"),
        }});
        // Defaults mirror toyengine/scene/scene_link.h.
        add({"SceneLink", "Gameplay", {
            with_tip(f_asset("target_scene", "scenes", ".yaml", false, false, "", true),
                     "Scene to load when the player walks in (relative to this scene, or an asset path)"),
            with_tip(f_enum("transition", {"fade", "loading_screen", "none"}, true), "What covers the scene change"),
            with_tip(f_asset("loading_screen", "ui", ".yaml", true, false, "ui/loading_screen"),
                     "UI asset shown while loading (transition: loading_screen); a 'progress' bar and 'status' text are filled in"),
            with_tip(f_string("spawn_point", ""), "Object in the target scene the player is moved to"),
            f_color("color", glm::vec3(0.0f)),
            with_tip(f_float("fade_time", 0.35f, 0.01f, 0.0f, 10.0f), "Seconds to fade out, and again to fade in"),
            with_tip(f_float("min_display_time", 0.0f, 0.01f, 0.0f, 60.0f), "The transition stays up at least this long"),
            with_tip(f_vec3("size", glm::vec3(2.0f), 0.01f, true), "Trigger box, in the object's local space"),
        }});
        // toyengine/save/saveable.h. Adding one (or duplicating its object) fills in a fresh
        // unique id -- SceneDocument::add_component() / duplicate_objects().
        add({"SaveId", "Gameplay", {
            with_tip(f_string("id", "", true), "Stable identity in saves; unique per scene (empty: the object's name path)"),
        }});
        add({"SaveDemo", "Gameplay", {
            with_tip(f_string("coin_prefix", "coin_", true), "Root objects whose names start with this are coins"),
            with_tip(f_float("pickup_radius", 1.0f, 0.01f, 0.0f, 100.0f), "Metres from the player's chest"),
        }});
        add({"CharacterAnimDriver", "Gameplay", {
            f_string("idle_state", "idle", true),
            f_string("walk_state", "walk", true),
            f_string("run_state", "run", true),
            f_string("jump_state", "jump", true),
            with_tip(f_float("walk_speed", 0.2f, 0.01f, 0.0f, 100.0f), "Speed (m/s) above which walk plays"),
            with_tip(f_float("run_speed", 5.0f, 0.01f, 0.0f, 100.0f), "Speed (m/s) above which run plays"),
            f_float("crossfade", 0.2f, 0.005f, 0.0f, 10.0f),
            f_float("air_delay", 0.15f, 0.005f, 0.0f, 10.0f),
        }});
        add({"KinematicMover", "Gameplay", {
            f_enum("mode", {"PingPong", "Orbit", "Spin"}, true),
            f_vec3("axis", glm::vec3(1.0f, 0.0f, 0.0f), 0.01f, true),
            f_float("distance", 2.0f, 0.02f),
            f_float("speed", 1.0f, 0.01f),
            f_vec3("orbit_center", glm::vec3(0.0f), 0.02f),
            f_float("orbit_radius", 2.0f, 0.02f),
            f_vec3("spin_axis", glm::vec3(0.0f, 0.0f, 1.0f), 0.01f),
            f_float("spin_speed", 90.0f, 0.5f),
        }});
        // Defaults mirror toyengine/scene/destructible.h.
        add({"Destructible", "Gameplay", {
            with_tip(f_int("pieces", 12, 1, 512), "Voronoi cells the mesh breaks into"),
            with_tip(f_int("seed", 1, 0, 1000000), "The same mesh, pieces and seed always break identically"),
            with_tip(f_float("irregularity", 0.7f, 0.01f, 0.0f, 1.0f), "0: even pieces (a cast slab). 1: a rock"),
            with_tip(f_float("impact_impulse", 800.0f, 10.0f, 0.0f, 1.0e6f),
                     "Impact that breaks it, in N*s (mass x speed it stops); 0 = only code can"),
            with_tip(f_float("scatter", 1.5f, 0.05f, 0.0f, 100.0f), "Outward speed given to each chunk (m/s)"),
            with_tip(f_float("impact_transfer", 0.35f, 0.01f, 0.0f, 10.0f), "Share of the blow's speed the chunks carry on with"),
            with_tip(f_float("chunk_lifetime", 10.0f, 0.1f, 0.0f, 600.0f), "Seconds before the debris is cleaned up; 0 keeps it"),
            with_tip(f_float("chunk_fade", 1.0f, 0.05f, 0.0f, 60.0f), "Seconds a chunk sinks out of sight for"),
            with_tip(f_float("chunk_density", 700.0f, 10.0f, 1.0f, 20000.0f), "kg/m^3 a chunk's mass comes from"),
            with_tip(f_int("max_chunks", 0, 0, 512), "Cap on chunks spawned (largest kept); 0 = all"),
            with_tip(f_float("collider_fill", 0.6f, 0.05f, 0.0f, 1.0f),
                     "A chunk filling less of its box than this collides as several boxes instead of one"),
            with_tip(f_int("max_boxes", 4, 1, 16), "Most collision boxes one chunk may use; 1 keeps it a single box"),
            with_tip(f_string("chunk_physics_material", ""), "physics_materials/<name>.yaml for the debris"),
            with_tip(f_bool("prefracture", true), "Cut the mesh up at load rather than at the moment it breaks"),
        }});
        // Defaults mirror toyengine/scene/terrain_digger.h.
        add({"TerrainDigger", "Gameplay", {
            with_tip(f_enum("dig_button", {"middle", "left", "right"}, true), "Mouse button that digs the terrain under the cursor"),
            with_tip(f_enum("raise_key", {"ctrl", "shift", "alt", ""}, true),
                     "Held, the dig raises instead of lowers; empty = dig only. Shift is already the "
                     "engine's move-down / sprint binding"),
            with_tip(f_int("radius", 1, 0, 64), "Tiles either side of the one hit: 0 digs one tile, 1 a 3x3 patch"),
            with_tip(f_int("steps", 1, 1, 64), "Terraces moved per dig"),
            with_tip(f_float("repeat_seconds", 0.12f, 0.01f, 0.0f, 10.0f), "Seconds between digs while the button is held"),
            with_tip(f_float("reach", 10000.0f, 10.0f, 1.0f, 1.0e6f), "How far the pick ray reaches (m)"),
        }});
        // Defaults mirror toyengine/scene/turntable.h.
        add({"Turntable", "Gameplay", {
            with_tip(f_float("sensitivity", 0.25f, 0.005f, 0.0f, 10.0f), "Degrees of spin per pixel of drag"),
            with_tip(f_enum("drag_button", {"left", "right", "middle", ""}, true),
                     "Hold this mouse button to spin the object; empty spins on any mouse motion"),
            with_tip(f_vec3("axis", glm::vec3(0.0f, 0.0f, 1.0f), 0.01f), "The object's pole, in its own space; horizontal drag spins about it"),
            with_tip(f_float("idle_spin_deg_per_sec", 0.0f, 0.1f, -3600.0f, 3600.0f), "Constant drift when nothing is dragging it"),
            with_tip(f_float("damping", 2.5f, 0.05f, 0.0f, 100.0f), "Decay of a fling's spin (1/s); 0 spins on for ever"),
            with_tip(f_float("min_tilt_deg", -80.0f, 0.5f, -89.0f, 89.0f), "Tilt clamp; equal to max pins the tilt"),
            with_tip(f_float("max_tilt_deg", 80.0f, 0.5f, -89.0f, 89.0f), "Tilt clamp"),
            with_tip(f_float("max_spin_deg_per_sec", 540.0f, 1.0f, 0.0f, 36000.0f), "Fastest a fling may leave it spinning"),
            f_bool("invert_x", false),
            f_bool("invert_y", false),
        }});
        // Defaults mirror toyengine/scene/xray_markers.h. Both are plain markers that cover
        // their own subtree, so there is nothing to tune beyond switching one back off.
        add({"XraySubject", "Rendering", {
            with_tip(f_bool("enabled", true),
                     "Show this object's silhouette when it is hidden behind an X-Ray Occluder. "
                     "Covers every child too; untick on a child to leave just that part out"),
        }});
        add({"XrayOccluder", "Rendering", {
            with_tip(f_bool("enabled", true),
                     "Let an X-Ray Subject be seen through this object. Covers every child too; "
                     "untick on a child to keep just that part solid"),
        }});
        // Defaults mirror toyengine/scene/view_blocker.h.
        add({"ViewBlockerFade", "Rendering", {
            with_tip(f_float("fade_target", 1.0f, 0.01f, 0.0f, 1.0f, true),
                     "How far it dissolves while in the way; 1 removes it completely"),
            with_tip(f_float("fade_in_time", 0.18f, 0.01f, 0.0f, 10.0f), "Seconds to dissolve; 0 is instant"),
            with_tip(f_float("fade_out_time", 0.35f, 0.01f, 0.0f, 10.0f), "Seconds to come back; 0 is instant"),
            with_tip(f_int("cutoff_steps", 32, 0, 256),
                     "Dissolve levels to snap to. Keeps the shadow cache from rebuilding every "
                     "frame of the fade; 0 is a continuous value at that cost"),
            with_tip(f_bool("keep_shadow", true),
                     "Keep casting the solid shadow while hidden. The wall is still there, so "
                     "losing its shadow pops the whole scene's lighting; untick to have it "
                     "vanish completely"),
        }});
        add({"FreeMover", "Gameplay", {f_float("move_speed", 5.0f, 0.05f, 0.0f, 1000.0f, true),
                                       with_tip(f_float("smoothing", 12.0f, 0.1f, 0.0f, 100.0f), "Follow rate (1/s); 0 snaps")}});
        add({"KinematicController", "Gameplay", {
            f_float("move_speed", 5.0f, 0.05f, 0.0f, 1000.0f, true),
            with_tip(f_float("smoothing", 10.0f, 0.1f, 0.0f, 100.0f), "Follow rate (1/s); 0 snaps"),
            f_bool("lock_height", true)}});
        // Defaults mirror the runtime's (toyengine/world/terrain_component.h, terrain_sampler.h):
        // an absent key shows the value the terrain actually uses.
        // The streamed tile world (toyengine/world/terrain_component.h). Laid out, labelled and
        // filtered by the TileTerrain panel (editor/app/ui/tile_terrain_panel.h); the tooltips
        // here are what it shows. `kind_tile_sets:` is a map only the panel edits.
        auto tile_terrain_fields = [] {
            return std::vector<FieldDesc>{
                with_tip(f_string("tile_set", "objects/terrain/tileset_round", true),
                         "The tile-set object the terrain is built from -- its pieces shape every edge, corner and "
                         "wall. tileset_voxel: square blocks"),
                with_tip(f_string("tiles", ""), "The stored tiles' file (scene-relative); empty: terrain/<object>.tiles. "
                                                "Baked from the generator settings (World Generator) when it does not exist"),
                with_tip(f_int("world_tiles", 0, 0, 16384), "The world's size in tiles per side: how many tiles the generated map is "
                                                            "cut into. 0: Map Cells x Tiles per Cell. Applies when the world is generated"),
                with_tip(f_int("tiles_per_grid_unit", 4, 1, 64), "Tiles across one map cell (when World Size is 0): finer coastlines and rivers, at a squared cost"),
                with_tip(f_float("tile_size", 1.0f, 0.01f, 0.01f, 100.0f), "Width of one tile (m)"),
                with_tip(f_float("height_step", 1.0f, 0.005f, 0.01f, 100.0f), "Height of one terrace step (m); equal to Tile Size keeps pieces cubic"),
                with_tip(f_float("height_scale", 16.0f, 0.05f, 0.0f, 1000.0f), "Height from sea floor to the highest peak (m)"),
                with_tip(f_float("water_inset", 0.5f, 0.01f, 0.0f, 0.95f),
                         "How far the water sits under the top of the terrace it fills (steps); the shoreline is where it "
                         "meets the bank"),
                with_tip(f_float("river_inset", 0.2f, 0.01f, 0.0f, 0.95f),
                         "The same for rivers (steps): lower sits them higher in their channels, nearer the bank tops"),
                with_tip(f_int("max_wall_steps", 24, 0, 1024), "Tallest wall drawn under a column, in steps (deep sea cliffs stop here)"),
                with_tip(f_int("soil_depth_steps", 3, 0, 1024), "Steps of soil under the surface before bare stone"),
                with_tip(f_int("chunk_size", 16, 4, 512), "Tiles per chunk side -- the streaming, drawing and collision unit"),
                with_tip(with_label(f_int("view_radius", 3, 0, 64), "View radius (chunks)"),
                         "Chunks kept loaded around the camera in each direction"),
                with_tip(f_int("max_chunk_jobs_per_frame", 16, 1, 256), "Chunk meshing jobs started per frame"),
                with_tip(f_int("max_chunks_in_flight", 0, 0, 512), "Chunks meshing at once; 0 = twice the worker count"),
                with_tip(f_float("upload_budget_ms", 1.5f, 0.1f, 0.1f, 16.0f),
                         "Milliseconds a frame may spend publishing finished chunks (at least one always lands)"),
                with_tip(f_int("dormant_chunks", 32, 0, 512),
                         "Chunks kept whole after leaving view, so turning back re-attaches instead of rebuilding"),
                with_tip(f_bool("collision", true), "Each chunk collides with exactly the surface it draws"),
                with_tip(with_label(f_int("collision_radius", 4, 0, 64), "Collision radius (chunks)"),
                         "Chunks given a collider, around the camera; 0 = every loaded chunk. Ground outside it is not solid"),
                with_tip(f_float("shadow_distance", 0.0f, 1.0f, 0.0f, 10000.0f),
                         "Metres past which a chunk stops casting shadows; 0 = no limit"),
                // `lods:` (the chunk meshes' coarser levels) is a list of mappings, which the
                // inspector has no field kind for -- it stays YAML-only, and survives a save
                // because the document keeps the component's own node (editor/core/scene_document.h).
                with_tip(f_string("physics_material", ""), "physics_materials/<name>.yaml for the colliders (friction, bounce)"),
                f_material(),
            };
        };
        // The sculptable heightmap terrain (toyengine/world/heightmap_terrain.h). Its shape and
        // paint live in PNGs beside the scene; Terrain Mode (viewport mode menu) edits them.
        // `tessellation:` and `generate:` are YAML-only maps, preserved on save.
        {
            Node grass_layer = Node::mapping();
            grass_layer["name"] = std::string("grass");
            grass_layer["tint"] = Node::mapping();
            grass_layer["tint"]["r"] = 0.42f;
            grass_layer["tint"]["g"] = 0.5f;
            grass_layer["tint"]["b"] = 0.3f;
            grass_layer["tiling"] = 6.0f;
            Node layers_default = Node::sequence();
            layers_default.as_seq().push_back(grass_layer);
            add({"HeightmapTerrain", "World", {
                with_tip(f_vec2("size", glm::vec2(256.0f), 1.0f, true), "Width (x) and depth (y) in metres"),
                with_tip(f_float("height", 64.0f, 0.1f, 0.01f, 10000.0f, true), "Metres at the heightmap's full value"),
                with_tip(f_float("chunk_size", 32.0f, 0.5f, 4.0f, 2048.0f, true), "Metres per chunk (draw / cull / collide / scatter unit)"),
                with_tip(f_int("resolution", 257, 3, 8193, true), "Height samples per side (2^n + 1: 257, 513, 1025...)"),
                f_int("splat_resolution", 512, 4, 8192),
                with_tip(f_string("heightmap", ""), "16-bit greyscale PNG (scene-relative); default terrain/<object>_height.png"),
                f_string("splat0", ""),
                f_string("splat1", ""),
                f_items("layers", {
                    f_string("name", "layer"),
                    f_asset("albedo", "textures", ".png"),
                    f_asset("normal", "textures", ".png"),
                    f_color("tint", glm::vec3(1.0f)),
                    with_tip(f_float("tiling", 8.0f, 0.05f, 0.01f, 1000.0f), "Metres per texture repeat"),
                    f_float("roughness", 0.85f, 0.005f, 0.0f, 1.0f),
                    f_float("metallic", 0.0f, 0.005f, 0.0f, 1.0f),
                    f_float("normal_strength", 1.0f, 0.01f, 0.0f, 4.0f),
                    with_tip(f_bool("triplanar", false), "Project from the sides on steep slopes (cliffs)"),
                    f_float("triplanar_sharpness", 4.0f, 0.05f, 1.0f, 16.0f),
                }, layers_default, true),
                with_tip(f_bool("deep_snow", false), "Deep snow (with trenches) piles on flat ground when it snows"),
                with_tip(f_bool("puddles", true), "Rain gathers in hollows"),
                f_bool("collision", true),
                f_string("physics_material", ""),
                f_bool("cast_shadows", true),
                with_tip(f_float("view_distance", 0.0f, 1.0f, 0.0f, 100000.0f),
                         "Chunks exist only within this distance of the camera (m). 0 = automatic: the camera's far clip, "
                         "with frustum culling doing the rest. Set it to cap the cost for close / top-down cameras or huge terrains"),
                with_tip(f_float("shadow_distance", 0.0f, 1.0f, 0.0f, 100000.0f), "Chunks farther than this cast no shadows (m); 0 = the view distance"),
                with_tip(f_int("patch_resolution", 0, 0, 64), "Quads per chunk side before tessellation; 0 = automatic (4, or 32 without tessellation)"),
                f_int("layer_texture_size", 1024, 4, 4096),
            }});
        }
        // Painted instances (toyengine/instance/). The records are NOT fields: they live in a
        // `.instances` file beside the scene, and `data:` is the only trace of them here.
        {
            // The palette starts EMPTY on purpose: a prototype has to name an object asset or a
            // mesh, and a placeholder entry naming neither is a component the engine refuses to
            // load. Add Component then Prototypes > Add is the path.
            const Node prototypes_default = Node::sequence();
            // toyengine/procgraph/: a node graph baked into an object subtree. There is
            // deliberately no `collider`/`output` row -- what a graph produces is decided by the
            // nodes in it, so those keys do not exist on the component. `inputs` is free-form and
            // validated against the graph's own interface, which is why it is a raw node here
            // rather than a typed row list.
            add({"Curve", "World", {
                with_tip(f_bool("closed", false),
                         "Join the last point back to the first, so the path is a loop"),
                with_tip(f_int("resolution", 12, 1, 64, true),
                         "Samples per SEGMENT when the curve is drawn or walked. Per segment, so "
                         "adding a point does not coarsen the rest"),
                with_tip(f_bool("always_show", false),
                         "Draw it in the viewport even when the object is not selected"),
            }});

            add({"ProceduralGraph", "World", {
                with_tip(f_asset("graph", "graphs", ".yaml", true, false),
                         "The graph asset to bake. Its own inputs appear under `inputs`"),
                with_tip(f_int("seed", 0, 0, 1000000, true),
                         "Seeds every random node, so a bake is repeatable. Change it for a "
                         "different arrangement of the same graph"),
                with_tip(f_enum("bake", {"on_load", "on_change", "manual"}, true),
                         "on_load bakes once during warm-up; on_change re-bakes whenever a bound "
                         "input moves (a few transform reads a frame); manual only on request"),
                with_tip(f_bool("refresh_on_asset_change", false),
                         "Re-bake when the graph file changes on disk. Useful while authoring; "
                         "a shipped game leaves it off"),
            }});

            add({"InstancePainter", "World", {
                with_tip(f_string("data", ""),
                         "The `.instances` file holding the painted records (scene-relative); "
                         "default instances/<object>.instances"),
                with_tip(f_float("chunk_size", 32.0f, 0.5f, 4.0f, 512.0f, true),
                         "Metres per chunk -- the draw, cull, collide and file unit. Match the terrain's chunks"),
                f_items("prototypes", {
                    with_tip(f_asset("object", "objects", ".yaml", true, false), "An object asset to paint; or use `mesh`"),
                    with_tip(f_asset("mesh", "meshes", ".yaml", true, true), "A bare mesh to paint, instead of an object asset"),
                    f_string("name", ""),
                    with_tip(f_enum("collision", {"none", "auto", "box", "sphere", "capsule", "mesh"}, true),
                             "What its instances collide as, within the painter's Collision Radius. "
                             "`auto` fits a capsule to anything taller than it is wide, else a box"),
                    with_tip(f_vec3("collision_size", glm::vec3(0.0f), 0.05f), "Explicit collider extents; 0 fits them to the bounds"),
                    with_tip(f_float("collision_radius", 0.0f, 0.01f, 0.0f, 100.0f), "Explicit collider radius; 0 fits it to the bounds"),
                    with_tip(f_asset("collision_mesh", "meshes", ".yaml", true, true),
                             "`collision: mesh` only: a simplified mesh to collide against. Empty uses the render mesh"),
                    f_string("physics_material", ""),
                    with_tip(f_vec2("scale_range", glm::vec2(1.0f), 0.01f, true), "Each instance takes a uniform scale in this range"),
                    with_tip(f_vec2("yaw_range", glm::vec2(0.0f, 360.0f), 1.0f), "Yaw about the surface normal, degrees"),
                    with_tip(f_float("align_to_normal", 0.0f, 0.01f, 0.0f, 1.0f),
                             "0 stands every instance up; 1 lies it flat on the surface. Trees want a little, rocks a lot"),
                    with_tip(f_float("sink", 0.0f, 0.005f, 0.0f, 5.0f), "Metres pushed into the surface, so nothing is perched on a slope"),
                    with_tip(f_float("draw_distance", 0.0f, 1.0f, 0.0f, 20000.0f), "Metres past which it is not drawn; 0 derives one from its height"),
                    with_tip(f_float("fade_start", 0.6f, 0.01f, 0.0f, 1.0f), "Thinning begins at this fraction of the draw distance"),
                    f_bool("cast_shadows", true),
                }, prototypes_default, true),
                with_tip(f_float("collision_radius", 64.0f, 1.0f, 0.0f, 4000.0f, true),
                         "Metres from the camera within which collidable instances have colliders. "
                         "0 turns collision off entirely, whatever the prototypes ask for"),
                f_bool("cast_shadows", true),
                with_tip(f_bool("enabled", true), "Off leaves the records untouched but draws and collides nothing"),
            }});
        }
        add({"TileTerrain", "World", tile_terrain_fields()});
        add({"Terrain", "World", tile_terrain_fields()});   // the unified name; `mode:` picks the half

        // The map a terrain is generated from (toyengine/mapgen). Generation is not part of a
        // terrain: this holds the world and APPLIES it, which is why `target` and `auto_apply`
        // come first. The mapcoopa settings themselves are a `config:` map the World Generator
        // window edits with a live preview, so they are deliberately not fields here -- there
        // are about fifty of them and a flat list would be unusable.
        add({"MapGenerator", "World", {
            with_tip(with_label(f_child("target"), "Apply To"),
                     "The terrain this world is written into. Empty: a terrain on this same object"),
            with_tip(f_enum("auto_apply", {"when_missing", "never", "always"}, true),
                     "When this applies itself without being asked.\n"
                     "When Missing: the target has no data yet -- a world with no stored .tiles is generated "
                     "on load, and once the file exists the stored world wins. This is what a scene wants.\n"
                     "Never: only the Generate button, or a call from game code.\n"
                     "Always: every load, discarding whatever the target had"),
            with_tip(f_bool("sphere", false), "Generate on a globe rather than a plane (a planet terrain sets it)"),
            with_tip(with_label(f_int("world_tiles", 0, 0, 16384), "World Tiles"),
                     "Tiles per side the map is rasterised into when it is applied to a TILED terrain. "
                     "0 derives it from the grid size"),
            with_tip(f_int("tiles_per_grid_unit", 4, 1, 64),
                     "Tiles per map cell, when World Tiles is 0. Raising it resolves coastlines and river "
                     "channels more finely, at a quadratic cost in tiles -- it does not add detail the map "
                     "does not have"),
            with_tip(with_unit(f_float("height_scale", 16.0f, 0.05f, 0.0f, 1000.0f), "m"),
                     "Height from the sea floor to the highest peak"),
            with_tip(f_float("water_inset", 0.5f, 0.01f, 0.0f, 0.95f),
                     "Steps the sea and lakes sit under the terrace they fill (tiled terrain only)"),
            with_tip(f_float("river_inset", 0.2f, 0.01f, 0.0f, 0.95f),
                     "The same for rivers, nearer their bank tops (tiled terrain only)"),
        }});
        // A rig root: `states` names its clips, `auto_play` the one played on start. A clip
        // path is arbitrary and routinely SHARED — a whole family of rigs can play one set
        // (every soft_<race> object asset names animations/soft_hero/*), so editing a clip
        // changes every rig that lists it. The Timeline is the place to author a clip; this
        // list is the place to see and re-point them.
        add({"Animator", "Animation", {
            with_tip(f_items("states", {
                f_string("name"),
                f_asset("clip", "animations", ".yaml"),
                f_enum("wrap", {"loop", "once", "pingpong", ""}),
                f_float("speed", 1.0f, 0.01f, 0.0f, 100.0f),
            }), "The rig's clips, by the state name gameplay crossfades to.\n"
                "Clip: the file played — shared with every other rig naming the same path.\n"
                "Wrap and Speed override the clip's own; (none) keeps what the clip says"),
            f_string("auto_play", "", true),
            f_float("speed", 1.0f, 0.01f, 0.0f, 100.0f),
            f_float("default_crossfade", 0.0f, 0.005f, 0.0f, 10.0f),
            with_tip(f_bool("apply_root_motion", false),
                     "Move this object by the clips' root motion (a CharacterController with "
                     "Use Root Motion takes it instead)")}});
        // IK over the animated pose (coopa/animation/ik_components.h, toyengine/scene/foot_ik.h).
        // Bone and target fields are paths from this object, like an animation track's object.
        add({"TwoBoneIK", "Animation", {
            with_tip(f_string("upper", "", true), "Hip / shoulder bone path"),
            with_tip(f_string("lower", "", true), "Knee / elbow bone path"),
            with_tip(f_string("end", "", true), "Ankle / wrist bone path (its origin reaches the target)"),
            with_tip(f_string("target", "", true), "Object to reach for"),
            with_tip(f_string("pole", ""), "Object the knee / elbow bends toward; empty keeps the animated bend"),
            f_float("weight", 1.0f, 0.01f, 0.0f, 1.0f, true),
            with_tip(f_float("soft_limit", 0.02f, 0.001f, 0.0f, 0.5f), "Fraction of the reach eased near full extension"),
            with_tip(f_bool("match_target_rotation", false), "The end bone also takes the target's rotation"),
            with_tip(f_int("priority", 0, -1000, 1000), "Solve order: lower first (ties: chains, two-bone, look-at)"),
        }});
        add({"ChainIK", "Animation", {
            with_tip(f_string("root", "", true), "First bone of the chain (it pivots in place)"),
            with_tip(f_string("tip", "", true), "Last bone, under root: its origin reaches the target"),
            with_tip(f_string("target", "", true), "Object to reach for"),
            f_float("weight", 1.0f, 0.01f, 0.0f, 1.0f, true),
            with_tip(f_int("iterations", 10, 1, 100), "FABRIK passes at most"),
            with_tip(f_float("tolerance", 0.001f, 0.0001f, 0.0f, 1.0f), "Close enough to stop (m)"),
            with_tip(f_bool("match_target_rotation", false), "The tip also takes the target's rotation"),
            with_tip(f_int("priority", 0, -1000, 1000), "Solve order: lower first (ties: chains, two-bone, look-at)"),
        }});
        add({"LookAtIK", "Animation", {
            with_tip(f_string("bone", "", true), "Bone path; empty turns this object"),
            with_tip(f_string("target", "", true), "Object to look at"),
            with_tip(f_vec3("forward_axis", glm::vec3(0.0f, 1.0f, 0.0f), 0.01f), "The bone's local looking axis"),
            with_tip(f_vec3("up_axis", glm::vec3(0.0f, 0.0f, 1.0f), 0.01f), "The bone's local up, kept from rolling"),
            with_tip(f_float("max_angle", 70.0f, 0.5f, 0.0f, 180.0f, true), "Degrees from the animated facing"),
            f_float("weight", 1.0f, 0.01f, 0.0f, 1.0f, true),
            with_tip(f_float("smoothing", 0.0f, 0.005f, 0.0f, 10.0f), "Seconds; 0 snaps"),
            with_tip(f_int("priority", 0, -1000, 1000), "Solve order: lower first (ties: chains, two-bone, look-at)"),
        }});
        add({"FootIK", "Animation", {
            f_string("pelvis", "pelvis", true),
            with_tip(f_strings("left", {"pelvis/thigh_l", "pelvis/thigh_l/shin_l", "pelvis/thigh_l/shin_l/foot_l"}, true),
                     "Left leg: thigh, shin, foot bone paths"),
            with_tip(f_strings("right", {"pelvis/thigh_r", "pelvis/thigh_r/shin_r", "pelvis/thigh_r/shin_r/foot_r"}, true),
                     "Right leg: thigh, shin, foot bone paths"),
            with_tip(f_float("ray_up", 0.5f, 0.005f, 0.0f, 10.0f), "Highest step a foot rises onto (m)"),
            with_tip(f_float("ray_down", 0.5f, 0.005f, 0.0f, 10.0f), "Deepest dip a foot follows (m)"),
            f_float("max_pelvis_drop", 0.35f, 0.005f, 0.0f, 5.0f),
            f_bool("align_feet", true),
            f_float("max_foot_angle", 30.0f, 0.5f, 0.0f, 89.0f),
            with_tip(f_float("blend_speed", 12.0f, 0.1f, 0.0f, 1000.0f), "Ease rate (1/s); 0 snaps"),
            f_float("weight", 1.0f, 0.01f, 0.0f, 1.0f, true),
            with_tip(f_enum("mode", {"auto", "solve", "targets"}),
                     "solve: FootIK bends the legs; targets: it lifts the rig's own leg IK targets; auto picks"),
            with_tip(f_bool("body_pitch", false), "Quadruped (with `legs`): raise/drop and pitch the pelvis bone between front and hind legs"),
            f_float("max_body_pitch", 20.0f, 0.5f, 0.0f, 89.0f),
        }});
        // A mesh deformed by a rig's bones: `bones:` (or, omitted, the mesh's vertex groups)
        // resolved under `rig:` (default: the nearest Animator up the hierarchy).
        add({"SkinnedMeshRenderer", "Rendering", {f_asset("mesh_path", "meshes", ".yaml", true, true, "", true), f_string("rig", ""),
                                                   f_bool("preserve_volume", false)}});
        apply_component_sections(t);
        return t;
    }();
    return table;
}

bool register_component_schema(ComponentSchema schema) {
    std::string type = schema.type;
    schema_table_()[type] = std::move(schema);
    return true;
}

const ComponentSchema* find_schema(const std::string& type) {
    auto it = schemas().find(type);
    return it == schemas().end() ? nullptr : &it->second;
}

Node default_component(const std::string& type) {
    Node c = Node::mapping();
    c["type"] = Node(type);
    const ComponentSchema* s = find_schema(type);
    if (!s) return c;
    for (const auto& f : s->fields) {
        if (!f.in_default) continue;
        const bool stringish = f.kind == FieldKind::Enum || f.kind == FieldKind::String || f.kind == FieldKind::AssetRef ||
                               f.kind == FieldKind::ChildRef;
        if (stringish && f.default_string.empty()) continue;
        c[f.key] = field_default_node(f);
    }
    return c;
}

} // namespace editor
} // namespace toy
