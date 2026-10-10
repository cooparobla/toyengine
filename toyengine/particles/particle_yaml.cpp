#include <toyengine/particles/particle_yaml.h>

namespace toy {
namespace particles {
namespace yaml_detail {

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

float num(const fkyaml::node& n) {
    if (n.is_integer()) return static_cast<float>(n.get_value<int64_t>());
    return n.get_value<float>();
}

Range range(const fkyaml::node& n, Range fallback) {
    if (n.is_integer() || n.is_float_number()) return Range(num(n));
    if (n.is_sequence()) {
        if (n.size() == 1) return Range(num(n.at(0)));
        if (n.size() >= 2) return Range(num(n.at(0)), num(n.at(1)));
        return fallback;
    }
    if (n.is_mapping()) {
        Range r = fallback;
        if (n.contains("min")) r.min = num(n.at("min"));
        if (n.contains("max")) r.max = num(n.at("max"));
        if (n.contains("x")) r.min = num(n.at("x"));
        if (n.contains("y")) r.max = num(n.at("y"));
        return r;
    }
    return fallback;
}

glm::vec3 vec3(const fkyaml::node& n, glm::vec3 v) {
    if (n.is_sequence() && n.size() >= 3) return {num(n.at(0)), num(n.at(1)), num(n.at(2))};
    if (n.contains("x")) v.x = num(n.at("x"));
    if (n.contains("y")) v.y = num(n.at("y"));
    if (n.contains("z")) v.z = num(n.at("z"));
    return v;
}

glm::vec4 color(const fkyaml::node& n, glm::vec4 c) {
    if (n.is_sequence() && n.size() >= 3) {
        c = glm::vec4(num(n.at(0)), num(n.at(1)), num(n.at(2)), n.size() > 3 ? num(n.at(3)) : 1.0f);
        return c;
    }
    if (n.contains("r")) c.r = num(n.at("r"));
    if (n.contains("g")) c.g = num(n.at("g"));
    if (n.contains("b")) c.b = num(n.at("b"));
    if (n.contains("a")) c.a = num(n.at("a"));
    return c;
}

std::vector<glm::vec2> curve_keys(const fkyaml::node& n) {
    std::vector<glm::vec2> keys;
    if (n.is_integer() || n.is_float_number()) {
        keys.push_back({0.0f, num(n)});
        return keys;
    }
    if (!n.is_sequence()) return keys;
    for (const auto& k : n) {
        if (k.is_sequence() && k.size() >= 2) {
            keys.push_back({num(k.at(0)), num(k.at(1))});
        } else if (k.is_mapping()) {
            const float t = k.contains("t") ? num(k.at("t")) : 0.0f;
            const float v = k.contains("value") ? num(k.at("value")) : k.contains("v") ? num(k.at("v")) : 1.0f;
            keys.push_back({t, v});
        }
    }
    return keys;
}

std::vector<Gradient::Key> gradient_keys(const fkyaml::node& n) {
    std::vector<Gradient::Key> keys;
    if (!n.is_sequence()) return keys;
    for (const auto& k : n) {
        if (!k.is_mapping()) continue;
        Gradient::Key key;
        key.t = k.contains("t") ? num(k.at("t")) : 0.0f;
        key.color = k.contains("color") ? color(k.at("color"), glm::vec4(1.0f)) : color(k, glm::vec4(1.0f));
        keys.push_back(key);
    }
    return keys;
}

} // namespace yaml_detail
} // namespace particles
} // namespace toy

namespace toy {
namespace particles {

void parse_particle_settings(const fkyaml::node& node, ParticleSettings& s) {
    using namespace yaml_detail;
    using render::ParticleRenderMode;
    using render::ParticleSprite;
    auto f = [&node](const char* key, float& out) { if (node.contains(key)) out = num(node.at(key)); };
    auto b = [&node](const char* key, bool& out) { if (node.contains(key)) out = node.at(key).get_value<bool>(); };
    auto r = [&node](const char* key, Range& out) { if (node.contains(key)) out = range(node.at(key), out); };
    auto str = [&node](const char* key) { return node.at(key).get_value<std::string>(); };

    // --- Main ---
    if (node.contains("mode")) {
        s.mode = enum_of<ParticleMode>(str("mode"), {{"emitter", ParticleMode::Emitter}, {"scatter", ParticleMode::Scatter},
                                                     {"hair", ParticleMode::Scatter}}, "mode");
    }
    f("duration", s.duration);
    b("looping", s.looping);
    b("prewarm", s.prewarm);
    f("start_delay", s.start_delay);
    b("play_on_start", s.play_on_start);
    r("start_lifetime", s.start_lifetime);
    r("start_speed", s.start_speed);
    r("start_size", s.start_size);
    r("start_rotation", s.start_rotation);
    if (node.contains("start_color")) {
        s.start_color = color(node.at("start_color"), s.start_color);
        s.start_color_b = s.start_color;
    }
    if (node.contains("start_color_b")) s.start_color_b = color(node.at("start_color_b"), s.start_color_b);
    f("gravity", s.gravity);
    if (node.contains("simulation_space")) {
        s.space = enum_of<SimulationSpace>(str("simulation_space"),
                                           {{"world", SimulationSpace::World}, {"local", SimulationSpace::Local}}, "simulation_space");
    }
    if (node.contains("simulation")) {
        s.simulation = enum_of<SimulationMode>(str("simulation"), {{"cpu", SimulationMode::Cpu}, {"gpu", SimulationMode::Gpu}}, "simulation");
    }
    if (node.contains("max_particles")) s.max_particles = static_cast<uint32_t>(std::max<int64_t>(0, node.at("max_particles").get_value<int64_t>()));
    if (node.contains("seed")) s.seed = static_cast<uint32_t>(node.at("seed").get_value<int64_t>());
    f("time_scale", s.time_scale);

    // --- Emission ---
    f("rate", s.rate);
    f("rate_over_distance", s.rate_over_distance);
    if (node.contains("count")) s.count = static_cast<uint32_t>(std::max<int64_t>(0, node.at("count").get_value<int64_t>()));
    if (node.contains("bursts") && node.at("bursts").is_sequence()) {
        s.bursts.clear();
        for (const auto& bn : node.at("bursts")) {
            Burst br;
            if (bn.contains("time")) br.time = num(bn.at("time"));
            if (bn.contains("count")) br.count = range(bn.at("count"), br.count);
            if (bn.contains("cycles")) br.cycles = static_cast<int>(bn.at("cycles").get_value<int64_t>());
            if (bn.contains("interval")) br.interval = num(bn.at("interval"));
            if (bn.contains("probability")) br.probability = num(bn.at("probability"));
            s.bursts.push_back(br);
        }
    }

    // --- Shape ---
    ShapeSettings& sh = s.shape;
    if (node.contains("shape")) {
        sh.type = enum_of<EmitShape>(str("shape"), {{"point", EmitShape::Point}, {"sphere", EmitShape::Sphere},
                                                   {"hemisphere", EmitShape::Hemisphere}, {"cone", EmitShape::Cone},
                                                   {"box", EmitShape::Box}, {"circle", EmitShape::Circle},
                                                   {"edge", EmitShape::Edge}, {"mesh", EmitShape::Mesh}}, "shape");
    }
    f("radius", sh.radius);
    f("radius_thickness", sh.radius_thickness);
    f("angle", sh.angle_deg);
    f("arc", sh.arc_deg);
    if (node.contains("box")) sh.box = vec3(node.at("box"), sh.box);
    f("length", sh.length);
    if (node.contains("shape_offset")) sh.offset = vec3(node.at("shape_offset"), sh.offset);
    f("random_direction", sh.random_direction);
    if (node.contains("mesh_path")) sh.mesh_path = str("mesh_path");
    if (node.contains("emit_from")) {
        sh.emit_from = enum_of<MeshEmitFrom>(str("emit_from"), {{"faces", MeshEmitFrom::Faces}, {"vertices", MeshEmitFrom::Vertices},
                                                               {"verts", MeshEmitFrom::Vertices}, {"edges", MeshEmitFrom::Edges}}, "emit_from");
    }
    if (node.contains("distribution")) {
        sh.distribution = enum_of<MeshDistribution>(str("distribution"), {{"random", MeshDistribution::Random},
                                                                          {"even", MeshDistribution::Even},
                                                                          {"jittered", MeshDistribution::Even}}, "distribution");
    }
    f("normal_offset", sh.normal_offset);
    b("align_to_normal", s.align_to_normal);
    b("random_spin", s.random_spin);
    f("inherit_velocity", s.inherit_velocity);

    // --- Velocity / forces / noise ---
    if (node.contains("velocity")) s.velocity = vec3(node.at("velocity"), s.velocity);
    if (node.contains("force")) s.force = vec3(node.at("force"), s.force);
    f("drag", s.drag);
    f("orbital", s.orbital);
    f("radial", s.radial);
    f("tumble", s.tumble);
    r("angular_velocity", s.angular_velocity);
    f("noise_strength", s.noise_strength);
    f("noise_frequency", s.noise_frequency);
    f("noise_scroll", s.noise_scroll);
    if (node.contains("noise_octaves")) s.noise_octaves = static_cast<int>(node.at("noise_octaves").get_value<int64_t>());

    // --- Over lifetime ---
    if (node.contains("color_over_life")) s.color_over_life.set_keys(gradient_keys(node.at("color_over_life")));
    if (node.contains("size_over_life")) s.size_over_life.set_keys(curve_keys(node.at("size_over_life")));
    if (node.contains("alpha_over_life")) s.alpha_over_life.set_keys(curve_keys(node.at("alpha_over_life")));

    // --- Collision ---
    b("collide", s.collide);
    f("ground_height", s.ground_height);
    f("bounce", s.bounce);
    f("collision_friction", s.collision_friction);
    b("kill_on_collide", s.kill_on_collide);
    b("on_death_collision_only", s.on_death_collision_only);

    // --- Wrap ---
    if (node.contains("wrap_box")) s.wrap_box = vec3(node.at("wrap_box"), s.wrap_box);
    f("wrap_fade", s.wrap_fade);

    // --- Sub emitters ---
    if (node.contains("on_death") && node.at("on_death").is_sequence()) {
        s.on_death.clear();
        for (const auto& sn : node.at("on_death")) {
            SubEmitter sub;
            if (sn.contains("target")) sub.target = sn.at("target").get_value<std::string>();
            if (sn.contains("count")) sub.count = range(sn.at("count"), sub.count);
            if (sn.contains("inherit_velocity")) sub.inherit_velocity = num(sn.at("inherit_velocity"));
            if (!sub.target.empty()) s.on_death.push_back(sub);
        }
    }

    // --- Renderer ---
    render::ParticleLook& look = s.look;
    if (node.contains("render_mode")) {
        look.mode = enum_of<ParticleRenderMode>(str("render_mode"), {
            {"billboard", ParticleRenderMode::Billboard}, {"stretched", ParticleRenderMode::Stretched},
            {"horizontal", ParticleRenderMode::Horizontal}, {"vertical", ParticleRenderMode::Vertical},
            {"aligned", ParticleRenderMode::Aligned}, {"mesh", ParticleRenderMode::Mesh},
            {"none", ParticleRenderMode::None}}, "render_mode");
    }
    if (node.contains("sprite")) {
        look.sprite = enum_of<ParticleSprite>(str("sprite"), {
            {"soft", ParticleSprite::Soft}, {"circle", ParticleSprite::Circle}, {"puff", ParticleSprite::Puff},
            {"flame", ParticleSprite::Flame}, {"spark", ParticleSprite::Spark}, {"ring", ParticleSprite::Ring},
            {"star", ParticleSprite::Star}, {"leaf", ParticleSprite::Leaf}, {"texture", ParticleSprite::Texture}}, "sprite");
    }
    if (node.contains("blend")) {
        const std::string bl = lower(str("blend"));
        if (bl == "additive" || bl == "add") look.additive = 1.0f;
        else if (bl == "alpha") look.additive = 0.0f;
        else throw std::runtime_error("[ParticleSystem] unknown blend '" + bl + "' (alpha | additive; or `additive: 0..1`)");
    }
    f("additive", look.additive);
    f("lit", look.lit);
    f("toon_bands", look.toon_bands);
    f("emissive", look.emissive);
    f("softness", look.softness);
    f("soft_distance", look.soft_distance);
    f("camera_fade", look.camera_fade);
    b("receive_shadows", look.receive_shadows);
    f("scatter", look.scatter);
    f("scatter_anisotropy", look.scatter_anisotropy);
    f("reactive", look.reactive);
    f("aspect", look.aspect);
    f("stretch_speed", look.stretch_speed);
    f("stretch_length", look.stretch_length);
    f("distortion", look.distortion);
    f("opacity", look.opacity);
    f("pivot", look.pivot_z);
    if (node.contains("flipbook")) {
        const auto& fb = node.at("flipbook");
        if (fb.is_sequence() && fb.size() >= 2) look.flipbook = {num(fb.at(0)), num(fb.at(1))};
        else {
            if (fb.contains("x")) look.flipbook.x = num(fb.at("x"));
            if (fb.contains("y")) look.flipbook.y = num(fb.at("y"));
        }
        look.flipbook = glm::max(look.flipbook, glm::vec2(1.0f));
    }
    if (node.contains("flipbook_mode")) {
        s.flipbook_mode = enum_of<FlipbookMode>(str("flipbook_mode"), {{"lifetime", FlipbookMode::Lifetime},
                                                                       {"random", FlipbookMode::Random},
                                                                       {"fps", FlipbookMode::Fps}}, "flipbook_mode");
    }
    f("flipbook_fps", s.flipbook_fps);
    f("flipbook_cycles", s.flipbook_cycles);
    if (node.contains("sort")) {
        s.sort = enum_of<SortMode>(str("sort"), {{"distance", SortMode::Distance}, {"none", SortMode::None},
                                                 {"oldest", SortMode::OldestFirst}, {"oldest_first", SortMode::OldestFirst},
                                                 {"youngest", SortMode::YoungestFirst}, {"youngest_first", SortMode::YoungestFirst}}, "sort");
    }
    f("max_draw_distance", s.max_draw_distance);
}

std::shared_ptr<const MeshSurface> load_mesh_surface(coopa::asset::AssetManager& assets, const std::string& key,
                                                            const std::string& base_dir) {
    static std::mutex mutex;
    static std::unordered_map<std::string, std::weak_ptr<const MeshSurface>> cache;
    const std::string path = assets.source().resolve("meshes/" + key + ".yaml", base_dir);
    std::lock_guard<std::mutex> lock(mutex);
    if (auto it = cache.find(path); it != cache.end()) {
        if (auto alive = it->second.lock()) return alive;
    }
    try {
        const auto src = coopa::gfx::engine::data::SkinnedMeshSource::from_node(coopa::yaml::load_document(path));
        std::vector<glm::vec3> p, n, t;
        p.reserve(src.vertices.size());
        n.reserve(src.vertices.size());
        t.reserve(src.vertices.size());
        for (const auto& v : src.vertices) {
            p.push_back(v.position);
            n.push_back(v.normal);
            t.push_back(glm::vec3(v.tangent));
        }
        auto surface = std::make_shared<MeshSurface>();
        surface->build(p, n, t, src.indices);
        cache[path] = surface;
        return surface;
    } catch (const std::exception& e) {
        std::cerr << "[ParticleSystem] emitter mesh '" << key << "' could not be read: " << e.what() << "\n";
        return nullptr;
    }
}

void register_particle_components(coopa::asset::AssetManager& assets) {
    using coopa::scene::SceneLoader;
    using coopa::scene::SceneObject;

    SceneLoader::register_component_parser("ParticleSystem",
        [&assets](const fkyaml::node& node, SceneObject& obj, const SceneLoader::ParseContext& ctx) {
            auto* ps = obj.add_component<ParticleSystem>();
            parse_particle_settings(node, ps->settings);

            const std::string base_dir = ctx.base_dir();
            ps->load_surface = [&assets, base_dir](const std::string& key) {
                return load_mesh_surface(assets, key, base_dir);
            };
            if (ps->settings.shape.type == EmitShape::Mesh && !ps->settings.shape.mesh_path.empty()) {
                ps->set_shape_surface(ps->load_surface(ps->settings.shape.mesh_path));
            }

            // Mesh render mode: the instanced mesh and its (opaque / cutout) material.
            if (ps->settings.look.mode == render::ParticleRenderMode::Mesh) {
                auto& proxy = ps->mesh_proxy();
                proxy.lods_enabled = false;
                if (node.contains("render_mesh")) {
                    const std::string key = node.at("render_mesh").get_value<std::string>();
                    proxy.set_mesh_path(key);
                    if (!key.empty()) {
                        proxy.set_mesh(assets.load_async<coopa::gfx::engine::data::Mesh>("meshes/" + key + ".yaml", base_dir));
                    }
                }
                if (node.contains("material")) {
                    coopa::gfx::engine::components::parse_material_value_(node.at("material"), proxy.material, assets, ctx);
                }
                if (proxy.material.is_blended()) {
                    // Instanced particle meshes join the opaque G-buffer batches only.
                    proxy.material.alpha_mode = coopa::gfx::engine::components::AlphaMode::Opaque;
                }
            }

            // Sprite texture (albedo slot) -- shared with the material system's loader, so it
            // gets the same sRGB declaration and async path as any other albedo map.
            if (node.contains("texture")) {
                fkyaml::node m = fkyaml::node::mapping();
                m["texture_albedo"] = node.at("texture");
                coopa::gfx::engine::components::parse_pbr_material_(m, ps->texture_material, assets, ctx);
                if (!node.contains("sprite")) ps->settings.look.sprite = render::ParticleSprite::Texture;
            }
        });

    SceneLoader::register_component_parser("LightFlicker",
        [](const fkyaml::node& node, SceneObject& obj, const SceneLoader::ParseContext&) {
            using yaml_detail::num;
            auto* lf = obj.add_component<LightFlicker>();
            if (node.contains("amount")) lf->amount = num(node.at("amount"));
            if (node.contains("speed")) lf->speed = num(node.at("speed"));
            if (node.contains("wobble")) lf->wobble = num(node.at("wobble"));
            if (node.contains("color_shift")) {
                const glm::vec4 c = yaml_detail::color(node.at("color_shift"), glm::vec4(0.0f));
                lf->color_shift = glm::vec3(c);
            }
            if (node.contains("seed")) lf->seed = static_cast<uint32_t>(node.at("seed").get_value<int64_t>());
        });
}

} // namespace particles
} // namespace toy
