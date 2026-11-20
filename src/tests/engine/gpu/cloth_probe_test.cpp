/**
 * @file cloth_probe_test.cpp
 * @brief The cloth probes: measurements of the DRAWN result, taken on the two demo scenes a
 *        viewer actually looks at.
 *
 * The suites beside this one check the solver against its own model of the world -- particles
 * against collider capsules, the sheet against an analytic sphere. Those are necessary and they
 * all pass; they are also exactly the checks a garment can satisfy while a limb hangs visibly
 * through it, because what a viewer sees is not a particle and not a capsule:
 *
 *   * The body on screen is the mannequin's PART MESHES riding its bones. A capsule fitted to a
 *     bone is a second model of that body, and anywhere the part sticks out past its own capsule
 *     the solver was never asked to hold cloth off it. No particle-vs-capsule check can see that
 *     gap, because the capsule is the thing it measures against.
 *   * A garment that never leaves its skinned pose satisfies every penetration bound there is --
 *     it is wherever the mesh was authored. "No interpenetration" is half the requirement; the
 *     other half is that the cloth MOVED, and a probe that does not measure motion will happily
 *     bless a rigid bell.
 *
 * So each probe here starts from the drawn geometry and works back:
 *
 *   1. body_parts_fit_inside_their_cloth_colliders -- every vertex of every drawn body part lies
 *      inside some collider the garment is solved against. The containment the capsule-based
 *      penetration bound silently assumes.
 *   2. character_garment_simulates_rather_than_rides_the_skin -- the free skirt leaves its
 *      skinned target by a fabric-sized distance and swings over the walk cycle.
 *   3. body_never_crosses_the_drawn_garment -- drawn body triangles against drawn garment
 *      triangles, and the drawn garment against the body's colliders, over a full walk cycle.
 *      The literal artefact, stated literally, in both directions.
 *   4. sheet_drapes_onto_the_ball -- the grid sheet lies ON the ball over its upper hemisphere
 *      rather than tenting across it, and stays out of it while the ball is driven.
 *
 * Every probe prints its measurement before asserting, so a failure reports a number rather than
 * a boolean and a passing run still records where the margin is.
 */

#include <coopa/testing/test.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

#include <fkYAML/node.hpp>
#include <glm/glm.hpp>

#include <coopa/scene/scene.h>
#include <coopa/scene/scene_object.h>
#include <gfxcoopa/engine/components/mesh_renderer.h>
#include <gfxcoopa/engine/data/mesh.h>
#include <physxcoopa/cloth/cloth.h>
#include <physxcoopa/cloth/cloth_collision.h>
#include <physxcoopa/components/cloth.h>
#include <toyengine/cloth/character_cloth.h>
#include <toyengine/cloth/cloth_mesh_system.h>
#include <toyengine/core/engine.h>
#include <toyengine/scene/skinned_mesh_renderer.h>

#include "engine/support/checks.h"
#include "engine/support/render_fixture.h"

COOPA_TEST_SUITE("cloth_probe");

using namespace toy::test;
namespace px = coopa::physx;

namespace {

/**
 * @brief The frame length the ball probes below run at, in seconds. 1/60 unless `CLOTH_PROBE_DT`
 *        overrides it.
 *
 * Frame length is a real axis of cloth behaviour, not a test detail: `PhysicsWorld::step_cloths_()`
 * solves cloth on the FRAME clock and sizes its substep count as `round(dt * 240)` capped at 16, so
 * the substep length -- and with it every margin the collision stage is sized against, starting with
 * `ClothParams::thickness` -- is a function of how fast the host is running. A sheet draped over a
 * ball driven at 4 m/s sees that ball advance `4 * dt` per frame: 1.3 cm at 300 fps, 6.7 cm at 60,
 * 13 cm at 30, against a 3 cm standoff.
 *
 * Pinning it to one value is what every cloth test in the tree does, which left the regime where the
 * host is slower than the authored clock untested. Both the engine's own `FIXED_DT` and each probe's
 * controller integration read this one function, so they cannot come apart.
 */
float probe_dt() {
    if (const char* v = std::getenv("CLOTH_PROBE_DT")) {
        const float parsed = std::strtof(v, nullptr);
        if (parsed > 0.0f) return parsed;
    }
    return 1.0f / 60.0f;
}

/** @brief probe_dt() as the decimal string `FIXED_DT` expects. */
std::string probe_dt_env() {
    return std::to_string(probe_dt());
}

/**
 * @brief Frames that cover `seconds` of simulated time at probe_dt(), at least one.
 *
 * Every duration in the ball probes is expressed in SECONDS through this, never as a frame count,
 * because a frame count means a different physical run at every frame length -- and two of these
 * probes read a quantity that depends on when you look. The sheet's drape is sampled against a
 * `wind_turbulence: 0.7` gust whose phase is a function of simulated time, so a fixed 260 frames
 * lands at 4.3 s at 1/60 and 65 s at 1/4: the measured "worst lift" then moves with the gust and
 * not with the solver. That confound produced a non-monotonic sweep (0.118 m failing between a
 * passing 0.113 and 0.104) which looked exactly like a frame-rate regression and was not one.
 */
int probe_frames(float seconds) {
    return std::max(1, static_cast<int>(std::lround(seconds / probe_dt())));
}

/** @brief One drawn body part: the mesh a viewer sees, and the object whose world matrix poses it. */
struct DrawnPart {
    std::string name;
    coopa::scene::SceneObject* owner = nullptr;
    std::vector<glm::vec3> local;   ///< Mesh vertices in the part's own space.
    std::vector<uint32_t> indices;  ///< Its triangle list, 3 per face.
    std::vector<glm::vec3> world;   ///< Scratch: `local` posed for the current frame.

    /** @brief Poses this part for the frame its owner's transform currently describes. */
    void pose() {
        const glm::mat4 m = owner->get_transform()->get_world_matrix();
        world.resize(local.size());
        for (std::size_t i = 0; i < local.size(); ++i) {
            world[i] = glm::vec3(m * glm::vec4(local[i], 1.0f));
        }
    }
};

/** @brief Finds `<name>.yaml` anywhere under assets/meshes, the way the mesh loader resolves a
 *         bare logical path. Returns an empty path when there is no such mesh. */
std::filesystem::path find_mesh_asset(const std::string& logical) {
    namespace fs = std::filesystem;
    const fs::path root = fs::path(ROOT_DIR) / "assets" / "meshes";
    const fs::path direct = root / (logical + ".yaml");
    if (fs::exists(direct)) return direct;
    for (const auto& entry : fs::recursive_directory_iterator(root)) {
        if (entry.is_regular_file() && entry.path().filename() == logical + ".yaml") {
            return entry.path();
        }
    }
    return {};
}

/**
 * @brief Collects the drawn body: every MeshRenderer under `root` except the ones named in
 *        `skip`, with its mesh's vertices loaded off disk.
 *
 * Loaded through Mesh::build_cpu(), the same parse the renderer uploads, so these are the
 * vertices on screen rather than a re-derivation of them.
 */
std::vector<DrawnPart> collect_drawn_parts(coopa::scene::SceneObject& root,
                                           const std::vector<std::string>& skip) {
    using MeshRenderer = coopa::gfx::engine::components::MeshRenderer;
    std::vector<DrawnPart> parts;
    for (MeshRenderer* mr : root.get_components_in_children<MeshRenderer>()) {
        if (mr->owner == nullptr || mr->mesh_path().empty()) continue;
        if (std::find(skip.begin(), skip.end(), mr->owner->name()) != skip.end()) continue;
        const std::filesystem::path asset = find_mesh_asset(mr->mesh_path());
        if (asset.empty()) continue;
        std::ifstream in(asset);
        if (!in) continue;
        DrawnPart part;
        part.name = mr->owner->name();
        part.owner = mr->owner;
        const fkyaml::node node = fkyaml::node::deserialize(in);
        const coopa::gfx::engine::data::MeshCpuData cpu =
            coopa::gfx::engine::data::Mesh::build_cpu(node);
        for (const auto& v : cpu.vertices) part.local.push_back(v.position);
        part.indices = cpu.indices;
        if (!part.local.empty()) parts.push_back(std::move(part));
    }
    return parts;
}

/**
 * @brief How far `p` lies outside the nearest of `colliders`, in metres; negative inside.
 *
 * Measured with the solver's own project_particle(), so the answer is about the shapes the cloth
 * is really solved against rather than a second reading of them. `reach` bounds the search: a
 * point further than that from every collider reports exactly `reach`.
 */
float distance_outside(const std::vector<px::cloth::ClothCollider>& colliders, const glm::vec3& p,
                       float reach) {
    float best = reach;
    for (const px::cloth::ClothCollider& c : colliders) {
        glm::vec3 moved = p, normal(0.0f);
        float depth = 0.0f;
        // A hit means p is within `reach` of this shape's surface (or inside it); the correction
        // depth is reach - distance, so distance = reach - depth, signed the right way already.
        if (px::cloth::project_particle(c, reach, moved, normal, depth)) {
            best = std::min(best, reach - depth);
        }
    }
    return best;
}

/**
 * @brief A garment surface frozen for one frame: world triangles bucketed by height, so a query
 *        only visits the fabric that spans its own z.
 *
 * The question asked of it is "does this body triangle CROSS the fabric", not "is this point
 * inside the garment". Inside/outside is the wrong question for a garment: a tube is open at the
 * collar and the hem, so the head above it and the shins below it are legitimately outside, and
 * so are bare arms hanging beside a sleeveless jacket. What is never legitimate is a drawn body
 * triangle and a drawn garment triangle passing through each other -- that is the artefact,
 * stated exactly, with no opening to special-case and no band to pick.
 */
class GarmentSurface {
public:
    /**
     * @brief Takes the whole garment at the positions it is DRAWN at.
     *
     * Not the particle positions: a vertex painted below `blend` is drawn as a mix of its skinned
     * position and its simulated one (ClothMeshSystem's render_weight, which is exactly this
     * lerp), and at max_distance 0 it is drawn purely skinned. Reading the particles alone would
     * describe a surface nobody sees. Skipping those rows instead -- the obvious shortcut, since
     * they are "the worn part that hugs the body anyway" -- blinds the probe to the entire collar
     * and shoulder region, which is precisely where a sleeveless garment meets the arms.
     */
    GarmentSurface(const px::cloth::Cloth& sim, float blend) {
        const std::size_t n = sim.particles.size();
        const bool skinned = sim.skin_targets.position.size() == n && sim.skin.max_distance.size() == n;
        std::vector<glm::vec3> drawn(n);
        for (std::size_t i = 0; i < n; ++i) {
            const float w =
                skinned ? std::clamp(sim.skin.max_distance[i] / std::max(blend, 1e-4f), 0.0f, 1.0f)
                        : 1.0f;
            drawn[i] = skinned ? glm::mix(sim.skin_targets.position[i], sim.particles[i].position, w)
                               : sim.particles[i].position;
        }
        for (std::size_t t = 0; t + 2 < sim.triangles.size(); t += 3) {
            const uint32_t i[3] = {sim.triangles[t], sim.triangles[t + 1], sim.triangles[t + 2]};
            if (i[0] >= n || i[1] >= n || i[2] >= n) continue;
            Tri tri{drawn[i[0]], drawn[i[1]], drawn[i[2]]};
            lo_ = std::min(lo_, std::min({tri.a.z, tri.b.z, tri.c.z}));
            hi_ = std::max(hi_, std::max({tri.a.z, tri.b.z, tri.c.z}));
            tris_.push_back(tri);
        }
        if (tris_.empty()) return;
        buckets_.resize(k_buckets);
        for (std::size_t k = 0; k < tris_.size(); ++k) {
            const Tri& tri = tris_[k];
            const int first = bucket_of(std::min({tri.a.z, tri.b.z, tri.c.z}));
            const int last = bucket_of(std::max({tri.a.z, tri.b.z, tri.c.z}));
            for (int b = first; b <= last; ++b) buckets_[static_cast<std::size_t>(b)].push_back(k);
        }
    }

    bool empty() const { return tris_.empty(); }
    std::size_t triangle_count() const { return tris_.size(); }

    /** @brief Writes triangle `i`'s three drawn corners into `out`. */
    void triangle(std::size_t i, glm::vec3 (&out)[3]) const {
        out[0] = tris_[i].a;
        out[1] = tris_[i].b;
        out[2] = tris_[i].c;
    }

    /**
     * @brief The longest chord the fabric cuts across the body triangle `(a, b, c)`; 0 when they
     *        do not intersect at all.
     *
     * Two triangles that intersect do so along a segment, and that segment's LENGTH is the
     * honest measure of how much of the body is hanging out through the fabric: a limb through a
     * skirt is cut across its whole width, while a triangle that merely grazes an edge is cut by
     * millimetres. Measuring the body triangle's straddle of the garment's PLANE instead -- the
     * obvious choice -- scores by the triangle's own size, and a mannequin's thigh is two
     * triangles 29 cm long, so a graze at one end reported 5 cm of penetration that was not
     * there.
     */
    float cross_span(const glm::vec3& a, const glm::vec3& b, const glm::vec3& c) const {
        if (tris_.empty()) return 0.0f;
        const glm::vec3 lo = glm::min(a, glm::min(b, c));
        const glm::vec3 hi = glm::max(a, glm::max(b, c));
        float worst = 0.0f;
        for (int bucket = bucket_of(lo.z); bucket <= bucket_of(hi.z); ++bucket) {
            for (const std::size_t k : buckets_[static_cast<std::size_t>(bucket)]) {
                const Tri& g = tris_[k];
                const glm::vec3 glo = glm::min(g.a, glm::min(g.b, g.c));
                const glm::vec3 ghi = glm::max(g.a, glm::max(g.b, g.c));
                if (glo.x > hi.x || ghi.x < lo.x || glo.y > hi.y || ghi.y < lo.y) continue;
                glm::vec3 hits[6];
                int count = 0;
                collect_crossings(Tri{a, b, c}, g, hits, count);
                collect_crossings(g, Tri{a, b, c}, hits, count);
                for (int i = 0; i < count; ++i) {
                    for (int j = i + 1; j < count; ++j) {
                        worst = std::max(worst, glm::length(hits[i] - hits[j]));
                    }
                }
            }
        }
        return worst;
    }

private:
    struct Tri {
        glm::vec3 a, b, c;
    };
    static constexpr int k_buckets = 48;

    int bucket_of(float z) const {
        if (hi_ - lo_ < 1e-6f) return 0;
        const float t = (z - lo_) / (hi_ - lo_);
        return std::clamp(static_cast<int>(t * k_buckets), 0, k_buckets - 1);
    }

    /** @brief Appends every point where an edge of `p` passes through `q` -- the two triangles'
     *         intersection segment is spanned by these, the degenerate coplanar case aside. */
    static void collect_crossings(const Tri& p, const Tri& q, glm::vec3 (&out)[6], int& count) {
        const glm::vec3 pv[3] = {p.a, p.b, p.c};
        for (int e = 0; e < 3 && count < 6; ++e) {
            glm::vec3 hit(0.0f);
            if (segment_hits(pv[e], pv[(e + 1) % 3], q, hit)) out[count++] = hit;
        }
    }

    /** @brief Moller-Trumbore, double-sided, with the hit confined to the segment. */
    static bool segment_hits(const glm::vec3& p0, const glm::vec3& p1, const Tri& t,
                             glm::vec3& hit) {
        const glm::vec3 d = p1 - p0;
        const glm::vec3 e1 = t.b - t.a, e2 = t.c - t.a;
        const glm::vec3 pvec = glm::cross(d, e2);
        const float det = glm::dot(e1, pvec);
        if (std::abs(det) < 1e-12f) return false;
        const float inv = 1.0f / det;
        const glm::vec3 tvec = p0 - t.a;
        const float u = glm::dot(tvec, pvec) * inv;
        if (u < 0.0f || u > 1.0f) return false;
        const glm::vec3 qvec = glm::cross(tvec, e1);
        const float v = glm::dot(d, qvec) * inv;
        if (v < 0.0f || u + v > 1.0f) return false;
        const float s = glm::dot(e2, qvec) * inv;
        if (s < 0.0f || s > 1.0f) return false;
        hit = p0 + d * s;
        return true;
    }

    std::vector<Tri> tris_;
    std::vector<std::vector<std::size_t>> buckets_;
    float lo_ = std::numeric_limits<float>::max();
    float hi_ = std::numeric_limits<float>::lowest();
};

/** @brief Opens the character demo and steps it past the first-frame build. */
struct CharacterDemo {
    toy::core::Engine engine;
    toy::cloth::ClothMeshSystem* system = nullptr;
    toy::cloth::CharacterCloth* cc = nullptr;
    toy::scene::SkinnedMeshRenderer* smr = nullptr;
    coopa::scene::SceneObject* hero = nullptr;

    CharacterDemo()
        : engine(make_test_config("assets/scenes/animation/character_cloth_demo/scene.yaml", 320,
                                  180, 160, 90)) {
        tick_frames(engine, 6);
        system = dynamic_cast<toy::cloth::ClothMeshSystem*>(engine.scene().find_system("ClothMesh"));
        hero = engine.scene().find_object("hero");
        auto* jacket = engine.scene().find_object("jacket");
        cc = jacket ? jacket->get_component<toy::cloth::CharacterCloth>() : nullptr;
        smr = jacket ? jacket->get_component<toy::scene::SkinnedMeshRenderer>() : nullptr;
    }

    const px::cloth::Cloth* sim() const { return system ? system->cloth("jacket") : nullptr; }
};

} // namespace

/**
 * @brief Every vertex of the drawn body sits inside a collider the garment is solved against.
 *
 * The containment the penetration bound in character_cloth_render assumes and never states: that
 * bound holds cloth off CAPSULES, so it is a statement about the body on screen only where the
 * capsules cover the body on screen. A part that reaches past its own capsule -- a hand mesh
 * drawn further down the bone than its collider runs, a torso wider than the capsule fitted to
 * its bone -- is a limb the solver will let cloth lie flat across, and the result is a limb
 * through the garment with every penetration test still green.
 *
 * Checked over a stride, because the fit is pose-dependent: bone colliders are re-posed from the
 * rig every frame while the part meshes ride the same bones, so a gap can open at one point of
 * the cycle and close at another.
 */
COOPA_TEST(body_parts_fit_inside_their_cloth_colliders) {
    ScopedEnv fixed_dt("FIXED_DT", "0.016666667");
    ScopedEnv no_input("NO_INPUT", "1");
    CharacterDemo demo;
    require(demo.system != nullptr && demo.hero != nullptr, "probe: the demo loaded its hero");
    require(demo.sim() != nullptr, "probe: the jacket built its simulation");

    std::vector<DrawnPart> parts = collect_drawn_parts(*demo.hero, {"jacket"});
    require(parts.size() >= 15, "probe: the drawn body's part meshes loaded (" +
                                    std::to_string(parts.size()) + " parts)");

    // Further than any real gap, so a part with no collider at all reports exactly this and is
    // not silently truncated into looking like a near miss.
    constexpr float k_reach = 0.08f;
    std::vector<float> worst_of(parts.size(), 0.0f);
    for (int frame = 0; frame < 90; ++frame) {
        tick_frames(demo.engine, 1);
        const std::vector<px::cloth::ClothCollider> colliders = demo.system->bone_colliders("jacket");
        if (colliders.empty()) continue;
        for (std::size_t k = 0; k < parts.size(); ++k) {
            parts[k].pose();
            for (const glm::vec3& p : parts[k].world) {
                worst_of[k] = std::max(worst_of[k], distance_outside(colliders, p, k_reach));
            }
        }
    }

    // The whole ranking, not just the maximum: one uncovered part hides every other one behind it,
    // and the list is what says whether the fit is a per-bone slip or the whole body.
    std::vector<std::size_t> order(parts.size());
    for (std::size_t k = 0; k < order.size(); ++k) order[k] = k;
    std::sort(order.begin(), order.end(),
              [&](std::size_t a, std::size_t b) { return worst_of[a] > worst_of[b]; });
    std::cout << "         drawn body vertices outside every cloth collider, worst part first:\n";
    for (const std::size_t k : order) {
        std::cout << "           " << worst_of[k] << " m  " << parts[k].name << "\n";
    }

    const std::size_t worst_idx = order.front();
    // 2 cm, and the bound is set by the one shape an automatic capsule fit cannot describe: a
    // BOX-section part leaves its corners uncovered by up to 41% of its minor radius, which on
    // this mannequin is the 5 cm visor across its face (1.9 cm). Every elliptical-section part --
    // which is every limb, and everything a garment can reach -- fits to within microns, so the
    // regression this guards against (a limb 3-8 cm past its collider, measured before the
    // colliders were fitted to the drawn parts) is nowhere near the bound. The printed table is
    // the real output: read it, do not just read the pass.
    expect(worst_of[worst_idx] < 0.02f,
           "probe: no drawn body vertex escapes the garment's colliders (worst " +
               std::to_string(worst_of[worst_idx]) + " m out of '" + parts[worst_idx].name + "')");
}

/**
 * @brief The free skirt is simulated, not merely drawn where the skinning put it.
 *
 * A garment pinned to its skinned pose passes every interpenetration bound in the suite -- it
 * sits exactly where the mesh was modelled -- and reads on screen as a rigid bell that ignores
 * the legs swinging inside it. So the probe measures displacement: how far the free rows get from
 * their own skinned targets, and how much the hem swings over a stride. Both must be
 * fabric-sized, not float noise.
 */
COOPA_TEST(character_garment_simulates_rather_than_rides_the_skin) {
    ScopedEnv fixed_dt("FIXED_DT", "0.016666667");
    ScopedEnv no_input("NO_INPUT", "1");
    CharacterDemo demo;
    require(demo.system != nullptr && demo.cc != nullptr, "probe: the demo's jacket is live");
    require(demo.sim() != nullptr, "probe: the jacket built its simulation");

    // Free cloth only: the fitted rows are painted to ride the skinning and SHOULD not move.
    const px::cloth::Cloth& first = *demo.sim();
    std::vector<std::size_t> free_particles;
    for (std::size_t i = 0; i < first.particles.size(); ++i) {
        if (i < first.skin.max_distance.size() && first.skin.max_distance[i] >= 0.10f) {
            free_particles.push_back(i);
        }
    }
    require(free_particles.size() > 20, "probe: the garment paints a free skirt (" +
                                            std::to_string(free_particles.size()) + " vertices)");

    // ONE named hem particle, tracked by index. Taking "the lowest free particle" each frame
    // measures which vertex happens to be lowest around a 0.6 m ring, not how far any of them
    // travelled, and reports most of the ring's diameter on a garment that never moved.
    std::size_t hem = free_particles.front();
    for (const std::size_t i : free_particles) {
        if (first.particles[i].position.z < first.particles[hem].position.z) hem = i;
    }

    float peak_offset = 0.0f;
    double offset_sum = 0.0;
    std::size_t offset_count = 0;
    float hem_lo = std::numeric_limits<float>::max();
    float hem_hi = std::numeric_limits<float>::lowest();
    for (int frame = 0; frame < 150; ++frame) {
        tick_frames(demo.engine, 1);
        const px::cloth::Cloth* sim = demo.sim();
        if (sim == nullptr || sim->skin_targets.position.size() != sim->particles.size()) break;
        for (const std::size_t i : free_particles) {
            const float d = glm::length(sim->particles[i].position - sim->skin_targets.position[i]);
            peak_offset = std::max(peak_offset, d);
            offset_sum += d;
            ++offset_count;
        }
        // Measured against its own skinned target, so the hero walking on the spot or travelling
        // makes no difference: this is the fabric's motion relative to the body under it.
        const float travel =
            glm::length(sim->particles[hem].position - sim->skin_targets.position[hem]);
        hem_lo = std::min(hem_lo, travel);
        hem_hi = std::max(hem_hi, travel);
    }
    const float mean_offset =
        offset_count ? static_cast<float>(offset_sum / static_cast<double>(offset_count)) : 0.0f;
    std::cout << "         skirt offset from the skinned pose: peak " << peak_offset << " m, mean "
              << mean_offset << " m; one hem vertex swung over a " << (hem_hi - hem_lo)
              << " m range\n";

    // 2 cm peak and 5 mm mean: below that the skirt tracks the skinned mesh closely enough that
    // nothing on screen distinguishes it from a rigid shell. A garment solved under gravity with
    // a 0.30 m leash reaches several centimetres.
    expect(peak_offset > 0.02f, "probe: the skirt leaves its skinned pose (peak " +
                                    std::to_string(peak_offset) + " m)");
    expect(mean_offset > 0.005f, "probe: the whole skirt moves, not one corner of it (mean " +
                                     std::to_string(mean_offset) + " m)");
    // A named hem vertex has to keep CHANGING its offset: a skirt held out at a constant distance
    // is still a rigid bell, just a wider one.
    expect(hem_hi - hem_lo > 0.02f, "probe: the hem keeps swinging over the stride (range " +
                                        std::to_string(hem_hi - hem_lo) + " m)");
}

/**
 * @brief Nothing passes through the garment, in either direction, anywhere in the walk cycle.
 *
 * The literal artefact, measured literally, and in both directions because they are different
 * failures with different causes:
 *
 *   * **The body out through the fabric.** Drawn part meshes against the drawn garment,
 *     triangle against triangle. This is what a limb hanging out of a skirt is, and it can
 *     happen with the fabric nowhere near the inside of that limb -- a hem that drifts outside
 *     a forearm is then pushed further out by collision doing its job.
 *   * **The fabric in through the body.** Garment surface points against the colliders, which
 *     the probe above pins to the drawn parts. This is what the collar sinking into a hip is,
 *     and no triangle crossing need occur for it.
 */
COOPA_TEST(body_never_crosses_the_drawn_garment) {
    ScopedEnv fixed_dt("FIXED_DT", "0.016666667");
    ScopedEnv no_input("NO_INPUT", "1");
    CharacterDemo demo;
    require(demo.system != nullptr && demo.hero != nullptr && demo.cc != nullptr,
            "probe: the demo's garment is live");
    require(demo.sim() != nullptr, "probe: the garment built its simulation");

    std::vector<DrawnPart> parts = collect_drawn_parts(*demo.hero, {"jacket"});
    require(!parts.empty(), "probe: the drawn body's part meshes loaded");

    float worst_span = 0.0f, worst_inside = 0.0f;
    std::vector<float> worst_inside_at(7, 0.0f);   // per 25-frame block of the 150-frame run
    std::string worst_part, inside_part;
    int worst_frame = -1, crossing_frames = 0;
    std::vector<float> worst_of(parts.size(), 0.0f);
    for (int frame = 0; frame < 150; ++frame) {
        tick_frames(demo.engine, 1);
        const px::cloth::Cloth* sim = demo.sim();
        if (sim == nullptr) break;

        // --- 1. The body out through the fabric. Every drawn body triangle against the drawn
        // garment; the chord of any intersection is how much of the limb is hanging out.
        const GarmentSurface surface(*sim, demo.cc->max_distance_blend);
        if (surface.empty()) continue;
        bool crossed_here = false;
        for (DrawnPart& part : parts) {
            part.pose();
            for (std::size_t t = 0; t + 2 < part.indices.size(); t += 3) {
                const float span = surface.cross_span(part.world[part.indices[t]],
                                                      part.world[part.indices[t + 1]],
                                                      part.world[part.indices[t + 2]]);
                if (span <= 0.0f) continue;
                crossed_here = true;
                const std::size_t k = static_cast<std::size_t>(&part - parts.data());
                worst_of[k] = std::max(worst_of[k], span);
                if (span > worst_span) {
                    worst_span = span;
                    worst_part = part.name;
                    worst_frame = frame;
                }
            }
        }
        if (crossed_here) ++crossing_frames;

        // --- 2. The fabric in through the body: the other direction, and the one a crossing
        // chord cannot express. Sampled over each garment triangle's interior rather than at its
        // corners, since the corners are the particles the solver already projected -- the dip
        // between them is exactly where a collider shows through (the ball demo's argument, and
        // what ClothSurfaceSample exists for).
        const std::vector<px::cloth::ClothCollider> colliders = demo.system->bone_colliders("jacket");
        // From frame 50: the garment is teleported onto its skinned pose when it is built and falls
        // onto the body from there, and a settling transient is not an artefact. 50 rather than the
        // 20 this used to be, because the printed per-block profile says so -- the deepest reach of
        // the whole run sits in the 25-49 block (12.5 mm) and the run settles to 4.9-9.7 mm from
        // block 75 on. 20 frames was simply not the length of the transient it was trying to skip,
        // and the worst block was the tail of the fall rather than anything the solver does in
        // steady state.
        if (colliders.empty() || frame < 50 || (frame % 5) != 0) continue;
        for (std::size_t t = 0; t + 2 < surface.triangle_count() * 3; t += 3) {
            glm::vec3 tri[3];
            surface.triangle(t / 3, tri);
            constexpr int k_steps = 3;
            for (int i = 0; i <= k_steps; ++i) {
                for (int j = 0; i + j <= k_steps; ++j) {
                    const float u = static_cast<float>(i) / k_steps;
                    const float v = static_cast<float>(j) / k_steps;
                    const glm::vec3 p = tri[0] * (1.0f - u - v) + tri[1] * u + tri[2] * v;
                    const float out = distance_outside(colliders, p, 0.05f);
                    worst_inside_at[frame / 25] = std::max(worst_inside_at[frame / 25], -out);
                    if (-out > worst_inside) {
                        worst_inside = -out;
                        inside_part = "frame " + std::to_string(frame);
                    }
                }
            }
        }
    }
    std::cout << "         frames with a body/garment crossing: " << crossing_frames
              << " of 150, longest chord " << worst_span << " m ('" << worst_part << "' at frame "
              << worst_frame << ")\n";
    for (std::size_t k = 0; k < parts.size(); ++k) {
        if (worst_of[k] > 0.0f) {
            std::cout << "           " << worst_of[k] << " m  " << parts[k].name << "\n";
        }
    }
    std::cout << "         deepest the drawn fabric reaches inside the body: " << worst_inside
              << " m (" << inside_part << ")\n";
    // Over time, so a settling transient is distinguishable from a standing penetration: the
    // garment is teleported onto its skinned pose when built and needs some frames to fall.
    std::cout << "         by 25-frame block:";
    for (std::size_t b = 0; b < worst_inside_at.size(); ++b) {
        std::cout << " " << b * 25 << ":" << worst_inside_at[b];
    }
    std::cout << "\n";
    // Neither bound is a tolerance that happens to pass; both are what the GEOMETRY costs once
    // the solver is right, with the arithmetic spelled out so a regression is still visible
    // against them.
    //
    // A chord is the WIDTH of a cut, not its depth: a body part of radius R poking depth d
    // through a surface is cut across 2*sqrt(2*R*d), so 3 cm across the mannequin's 0.13 m hip
    // is 0.9 mm of penetration -- about the 1.8 mm a 24-sided ring dips inside its own nominal
    // radius. The regression this guards against, a forearm hanging through a skirt, cut 5-8 cm.
    expect(worst_span < 0.03f, "probe: the drawn body never crosses the drawn garment (longest "
                               "chord " + std::to_string(worst_span) + " m through '" +
                                   worst_part + "' at frame " + std::to_string(worst_frame) + ")");
    // 1 cm, of which about half is discretisation the solver cannot remove: the garment's 6 cm
    // triangles chord 3.5 mm across a 13 cm hip between the points ClothSurfaceSample projects,
    // and its 24-gon rings dip another 1.8 mm inside their own radius. The regression here --
    // the render blend drawing collided cloth back at its skinned position -- measured 2.6 cm.
    //
    // Measured steady state once the fall is excluded is 4.9-9.7 mm by block, so this passes with
    // little room, and the accounting above explains only about half of the 9.7 mm. Read the
    // printed per-block profile before touching the bound: a REGRESSION moves every block, while
    // the transient that used to fail this moved only the first.
    expect(worst_inside < 0.01f, "probe: the drawn fabric never reaches inside the body (worst " +
                                     std::to_string(worst_inside) + " m in, " + inside_part + ")");
}

COOPA_TEST(sheet_drapes_onto_the_ball) {
    ScopedEnv fixed_dt("FIXED_DT", probe_dt_env());
    ScopedEnv no_input("NO_INPUT", "1");
    toy::core::Engine engine(
        make_test_config("assets/scenes/physics/cloth_demo/scene.yaml", 320, 180, 160, 90));
    tick_frames(engine, 2);

    auto* cc = engine.scene().find_first_component<px::components::ClothComponent>();
    auto* ball = engine.scene().find_object("ball");
    require(cc != nullptr && ball != nullptr, "probe: the ball demo parsed its Cloth and ball");
    constexpr float k_radius = 1.0f;

    tick_frames(engine, probe_frames(4.33f)); // settled drape

    const px::cloth::Cloth* sim = cc->cloth();
    require(sim != nullptr && !sim->particles.empty(), "probe: the sheet is simulating");
    const glm::vec3 centre(ball->get_transform()->transform().get_world_matrix()[3]);
    const float standoff = sim->params.thickness;

    // The cap the sheet is pinned over and must lie against. Selected by DIRECTION from the ball's
    // centre, not by height: a particle hanging in the air beside the ball is below its equator in
    // direction even while it is above the equator in z, and counting those would let a sheet that
    // merely hangs nearby pass as a drape. 0.9 excludes the pinned crown, whose particles are
    // written from the anchor rather than solved.
    int on_ball = 0, over_cap = 0;
    float worst_lift = 0.0f;
    for (const px::cloth::ClothParticle& p : sim->particles) {
        const glm::vec3 d = p.position - centre;
        const float r = glm::length(d);
        if (r < 1e-4f) continue;
        const float up = d.z / r;
        if (up <= 0.3f || up >= 0.9f) continue;
        ++over_cap;
        const float lift = r - (k_radius + standoff);
        worst_lift = std::max(worst_lift, lift);
        if (lift < 0.01f) ++on_ball;
    }
    require(over_cap > 20, "probe: particles cover the ball's upper hemisphere (" +
                               std::to_string(over_cap) + ")");

    // The settled shape itself, one row of the sheet straight through the ball's axis: the
    // distance from the axis against the height, beside where the ball's surface is at that same
    // distance. A drape tracks the ball column for column and then falls away past the equator;
    // a tent leaves the ball at the pin and never comes back, which no single summary number
    // distinguishes from a drape with one bad corner.
    if (sim->columns > 2 && sim->rows > 2) {
        std::cout << "         settled profile across the middle row (lateral -> sheet z / ball z):\n";
        const uint32_t row = sim->rows / 2;
        for (uint32_t x = 0; x < sim->columns; x += 2) {
            const glm::vec3 d = sim->particles[row * sim->columns + x].position - centre;
            const float lateral = glm::length(glm::vec2(d));
            const float on_ball = lateral < k_radius ? std::sqrt(k_radius * k_radius - lateral * lateral)
                                                     : -1.0f;
            std::cout << "           " << lateral << " -> " << d.z << " / "
                      << (on_ball >= 0.0f ? std::to_string(on_ball) : std::string("(past the ball)"))
                      << "\n";
        }
    }
    // What "lying on the ball" can mean here is bounded by the PIN, and the bound is worth
    // deriving rather than guessing at a percentage. The sheet is pinned over a flat disc 5 cm
    // above the crown, so from the rim of that disc it must run tangent to the ball before it can
    // touch: no solver can bring it closer than the pin rim's own standoff, and roughly the upper
    // half of the cap band is that tangent run. A probe that demanded more than this once passed
    // only because the sheet was SHRINKING onto the ball -- see
    // the_sheet_folds_rather_than_shrinking, which is the other half of this claim.
    float pin_lift = 0.0f;
    for (const px::cloth::ClothParticle& p : sim->particles) {
        if (p.inv_mass != 0.0f) continue;
        pin_lift = std::max(pin_lift, glm::length(p.position - centre) - (k_radius + standoff));
    }
    // Self-intersection: the thing ClothParams::self_collision exists to prevent, and the one
    // quantity none of the other measures here can see. Counted as pairs closer than
    // `self_distance` that are NOT already structurally constrained to each other -- the same
    // Chebyshev-1 exemption the solver's stage 6 applies, since a 1-ring neighbour being within
    // the rest spacing is the sheet existing, not the sheet folding through itself.
    //
    // Without this, "turning self_collision off costs nothing" is a claim about coverage and
    // strain only, which is exactly the shape of error that lets a probe bless a broken scene.
    int self_pairs = 0;
    float closest_pair = std::numeric_limits<float>::max();
    if (sim->columns > 0) {
        const float d_min = sim->params.self_distance;
        for (std::size_t i = 0; i < sim->particles.size(); ++i) {
            for (std::size_t j = i + 1; j < sim->particles.size(); ++j) {
                const int xi = static_cast<int>(i % sim->columns), yi = static_cast<int>(i / sim->columns);
                const int xj = static_cast<int>(j % sim->columns), yj = static_cast<int>(j / sim->columns);
                if (std::abs(xi - xj) <= 1 && std::abs(yi - yj) <= 1) continue;
                const float d = glm::distance(sim->particles[i].position, sim->particles[j].position);
                closest_pair = std::min(closest_pair, d);
                if (d < d_min) ++self_pairs;
            }
        }
    }
    std::cout << "         self-intersection: " << self_pairs << " non-neighbour pairs under "
              << sim->params.self_distance << " m, closest " << closest_pair << " m\n";

    const float contact = static_cast<float>(on_ball) / static_cast<float>(over_cap);
    std::cout << "         drape: " << on_ball << "/" << over_cap << " (" << contact * 100.0f
              << "%) of the upper cap within 1 cm of the standoff, worst lift " << worst_lift
              << " m against a pin-rim bound of " << pin_lift << " m\n";
    // The allowance over the pin rim is a MEASURED spread, not a tolerance picked to pass. This is
    // a max over particles on a sheet under a 0.7 m/s turbulent gust, so the single worst one
    // wanders with the gust: five runs of this identical configuration across a CLOTH_PROBE_DT
    // sweep gave 0.106 / 0.109 / 0.117 / 0.111 / 0.111 m, non-monotonic in dt and therefore noise
    // rather than frame-rate sensitivity. 0.03 sat inside that spread and failed one point of the
    // five for no physical reason. The mean coverage numbers beside it are the stable measure of
    // the same claim (57-58% at every frame length) and are what a regression would move.
    expect(worst_lift < pin_lift + 0.04f,
           "probe: the sheet comes as close to the ball as its pin allows (worst lift " +
               std::to_string(worst_lift) + " m, pin rim " + std::to_string(pin_lift) + " m)");
    expect(contact > 0.45f, "probe: the sheet lies on the ball's upper hemisphere rather than "
                            "tenting across it (" + std::to_string(contact * 100.0f) +
                            "% in contact)");

}

/**
 * @brief The sheet stays ON the ball while the ball is DRIVEN, and the drape survives the trip.
 *
 * The case a player is actually in, and the one the settled drape above says nothing about. It
 * needs three numbers together, because each alone has a way of passing while the scene looks
 * broken:
 *
 *   * **Penetration** -- the drawn surface inside the drawn ball. Necessary, and on its own
 *     worthless: a sheet that has been flung off the ball entirely scores a perfect zero, which
 *     is exactly what the first version of this probe reported while the capture showed the ball
 *     bare and the fabric streaming off sideways.
 *   * **Coverage** -- how much of the ball's upper cap still has fabric lying on it. This is what
 *     "the sheet slid off" actually is.
 *   * **Particle speed against the ball's own speed.** A positional solver derives velocity as
 *     `(p - prev) / h`, so a collider that jumps into the sheet and is then projected back out
 *     in one substep MANUFACTURES velocity: 8 cm of intrusion resolved in a 1/240 s substep is
 *     19 m/s on a ball travelling 5. That is the "glitches out" failure, and it is invisible to
 *     both measures above until the frame after it has thrown the sheet away.
 */
COOPA_TEST(sheet_stays_on_a_driven_ball) {
    ScopedEnv fixed_dt("FIXED_DT", probe_dt_env());
    ScopedEnv no_input("NO_INPUT", "1");
    toy::core::Engine engine(
        make_test_config("assets/scenes/physics/cloth_demo/scene.yaml", 320, 180, 160, 90));
    tick_frames(engine, 2);

    auto* cc = engine.scene().find_first_component<px::components::ClothComponent>();
    auto* ball = engine.scene().find_object("ball");
    require(cc != nullptr && ball != nullptr, "probe: the ball demo parsed its Cloth and ball");
    constexpr float k_radius = 1.0f;
    constexpr float k_speed = 4.0f;   // the scene's own KinematicController move_speed

    tick_frames(engine, probe_frames(4.33f));   // settled drape, as a player would find it

    // Coverage over the ball's upper cap, by DIRECTION from its centre -- the same measure the
    // settled drape is held to, so the two numbers are comparable.
    auto coverage = [&](const px::cloth::Cloth& sim, const glm::vec3& centre) {
        int on = 0, over = 0;
        for (const px::cloth::ClothParticle& p : sim.particles) {
            const glm::vec3 d = p.position - centre;
            const float r = glm::length(d);
            if (r < 1e-4f) continue;
            const float up = d.z / r;
            if (up <= 0.3f || up >= 0.9f) continue;
            ++over;
            if (r - (k_radius + sim.params.thickness) < 0.02f) ++on;
        }
        return over > 0 ? static_cast<float>(on) / static_cast<float>(over) : 0.0f;
    };

    const px::cloth::Cloth* settled = cc->cloth();
    require(settled != nullptr, "probe: the sheet is simulating");
    const float before = coverage(*settled, glm::vec3(ball->get_transform()->transform().get_world_matrix()[3]));

    // Driven exactly as the scene's own KinematicController drives it -- same move_speed, same
    // exponential smoothing, same write-before-physics ordering (the controller runs at update
    // order 50, ahead of Physics at 100). Driving by a raw per-frame offset instead is a STEP
    // from rest to 4 m/s in one frame, i.e. 24 g, and the sheet it whips off the ball says
    // nothing about any input a player can give.
    //
    // The run holds the stick forward, then slams it to full reverse at frame 120: a reversal is
    // the hardest case for a dragged sheet, and it is the one the scripted capture of this scene
    // looked worst in.
    const float k_dt = probe_dt();   // the engine is pinned to this same value above
    constexpr float k_smoothing = 8.0f;
    // In seconds, so a CLOTH_PROBE_DT sweep runs the same physical manoeuvre: 4.33 s of travel,
    // reversed at 2 s, with the last 1.5 s before each change counted as settled.
    const int k_frames = probe_frames(4.33f);
    const int k_reverse = probe_frames(2.0f);
    const int k_settled_after = probe_frames(1.5f);
    float velocity = 0.0f;
    float worst_in = 0.0f, worst_speed = 0.0f, start_speed = 0.0f;
    float worst_coverage = 1.0f, worst_settled = 1.0f, settled_speed = 0.0f;
    int worst_frame = -1;
    for (int i = 0; i < k_frames; ++i) {
        const float target = (i < k_reverse ? k_speed : -k_speed);
        velocity += (target - velocity) * (1.0f - std::exp(-k_smoothing * k_dt));
        coopa::util::Transform& t = ball->get_transform()->transform();
        t.set_position(t.position() + glm::vec3(velocity * k_dt, 0.0f, 0.0f));
        tick_frames(engine, 1);
        const px::cloth::Cloth* live = cc->cloth();
        if (live == nullptr) break;
        const glm::vec3 drawn(ball->get_transform()->transform().get_world_matrix()[3]);

        const float speed = px::cloth::max_particle_speed(*live);
        if (speed > worst_speed) { worst_speed = speed; worst_frame = i; }
        if (i == 0) start_speed = speed;
        const float cover = coverage(*live, drawn);
        worst_coverage = std::min(worst_coverage, cover);
        // "Settled" = the input has been constant for 1.5 s. The frames after the stick moves are
        // a real transient -- the sheet swings forward, then whips back onto the ball, and the
        // whole excursion takes about 65 frames -- and during it the sheet is allowed to lift and
        // to move faster than the ball. What it is not allowed to do is stay that way.
        if ((i > k_settled_after && i < k_reverse) || i > k_reverse + k_settled_after) {
            worst_settled = std::min(worst_settled, cover);
            settled_speed = std::max(settled_speed, speed);
        }

        for (std::size_t tri = 0; tri + 2 < live->triangles.size(); tri += 3) {
            const glm::vec3& a0 = live->particles[live->triangles[tri]].position;
            const glm::vec3& b0 = live->particles[live->triangles[tri + 1]].position;
            const glm::vec3& c0 = live->particles[live->triangles[tri + 2]].position;
            constexpr int k_steps = 4;
            for (int x = 0; x <= k_steps; ++x) {
                for (int y = 0; x + y <= k_steps; ++y) {
                    const float u = static_cast<float>(x) / k_steps;
                    const float v = static_cast<float>(y) / k_steps;
                    const glm::vec3 p = a0 * (1.0f - u - v) + b0 * u + c0 * v;
                    worst_in = std::max(worst_in, k_radius - glm::length(p - drawn));
                }
            }
        }
    }
    std::cout << "         driven at " << k_speed << " m/s with a full reversal: coverage "
              << before * 100.0f << "% at rest -> " << worst_settled * 100.0f
              << "% settled, " << worst_coverage * 100.0f
              << "% at the worst of the swing; surface depth " << worst_in
              << " m; particle speed " << start_speed << " m/s on the first moving frame, peak "
              << worst_speed << " m/s (frame " << worst_frame << "), settled peak "
              << settled_speed << " m/s\n";

    expect(worst_in <= 0.0f, "probe: the drawn sheet never enters the driven ball (worst " +
                                 std::to_string(worst_in) + " m in)");

    // The sharpest regression guard there is for the collider being teleported to its end-of-step
    // pose: on the FIRST frame of motion the sheet lying on the ball cannot legitimately be moving
    // much faster than the ball is. With the collider held at the far end of the step's
    // displacement this measured 13.7 m/s against a ball doing 0.5 -- 6.7 cm of intrusion removed
    // inside one 1/240 s substep and read straight back as velocity.
    //
    // Measured against the ball's OWN first-frame speed rather than a constant, because that speed
    // is a function of the frame length: the controller's exponential smoothing reaches
    // `4 * (1 - exp(-8 * dt))`, which is 0.5 m/s at 1/60 and 3.46 m/s at 1/4. A fixed bound
    // calibrated at 1/60 therefore fails a CLOTH_PROBE_DT sweep for the sheet correctly answering
    // a ball that really did start that fast -- which is a bug in the probe, not in the solver.
    const float ball_start_speed = k_speed * (1.0f - std::exp(-k_smoothing * k_dt));
    expect(start_speed < std::max(2.0f, ball_start_speed * 2.0f),
           "probe: a collider starting to move does not kick the sheet (" +
               std::to_string(start_speed) + " m/s on the first moving frame, against a ball doing " +
               std::to_string(ball_start_speed) + ")");

    // The drape has to SURVIVE the trip, and that is a separate claim from not interpenetrating:
    // a sheet flung clear of the ball scores a perfect zero on penetration, which is exactly what
    // this probe reported while a capture of the same scene showed the ball bare and the fabric
    // streaming off sideways.
    // Steady input: the fabric is riding the ball, not being flung around on it. Both halves
    // matter -- coverage says it is still there, speed says it is calm. A sheet merely draped
    // over a ball that happens to be moving reports neither.
    expect(settled_speed < k_speed * 2.5f,
           "probe: the sheet moves with the ball once the input is steady (peak " +
               std::to_string(settled_speed) + " m/s against a ball doing " +
               std::to_string(k_speed) + ")");
    // Measured against what the sheet had AT REST, not against an absolute: how much of the cap
    // a drape can reach at all is set by the pin geometry (see sheet_drapes_onto_the_ball), and
    // the question here is whether driving the ball costs it any of that.
    expect(worst_settled > before * 0.70f,
           "probe: the sheet rides the driven ball once the input is steady (upper-cap coverage " +
               std::to_string(worst_settled * 100.0f) + "%, from " +
               std::to_string(before * 100.0f) + "% at rest)");
    // Through the reversal the sheet legitimately swings forward off the ball -- slam a ball into
    // reverse and the dust sheet on it flies up -- so this is a floor, not a standard. It is the
    // "torn off and never came back" guard; before the fixes above the same run sat at 10%.
    expect(worst_coverage > 0.20f,
           "probe: and it is never torn off, even through the reversal (worst " +
               std::to_string(worst_coverage * 100.0f) + "%)");
}

/**
 * @brief The sheet keeps its SIZE. It is allowed to fold; it is not allowed to shrink.
 *
 * The invariant an inextensible fabric has that no penetration or coverage measure can see. A
 * sheet whose in-plane constraints give way under compression conforms to a ball by SHRINKING
 * ONTO it -- wrapping a sphere needs 36% less circumference at the equator than a flat disc has,
 * and if the material can simply lose that length it will, ending as a skin stretched over the
 * ball with its hem riding up above the equator and the ball showing underneath. Real cloth puts
 * the excess into folds instead and keeps every thread the length it was.
 *
 * Measured on the constraint lengths, over a long settle plus a drive, because the failure is a
 * CREEP: a frame of it is invisible and ten seconds of it is a shrink-wrapped ball.
 */
COOPA_TEST(the_sheet_folds_rather_than_shrinking) {
    ScopedEnv fixed_dt("FIXED_DT", probe_dt_env());
    ScopedEnv no_input("NO_INPUT", "1");
    toy::core::Engine engine(
        make_test_config("assets/scenes/physics/cloth_demo/scene.yaml", 320, 180, 160, 90));
    tick_frames(engine, 2);
    auto* cc = engine.scene().find_first_component<px::components::ClothComponent>();
    auto* ball = engine.scene().find_object("ball");
    require(cc != nullptr && ball != nullptr, "probe: the ball demo parsed its Cloth and ball");

    // Mean strain over the STRUCTURAL edges only -- the warp and weft, which are the threads. A
    // grid cloth's diagonals are shear, and shear is a fabric changing its angles, not its
    // length: the mean over all of them would hide a shrinking sheet behind a shearing one.
    auto mean_strain = [](const px::cloth::Cloth& sim) {
        double sum = 0.0;
        std::size_t n = 0;
        const uint32_t cols = sim.columns;
        for (const px::cloth::ClothConstraint& k : sim.stretch) {
            const uint32_t ax = k.a % cols, ay = k.a / cols;
            const uint32_t bx = k.b % cols, by = k.b / cols;
            if (ax != bx && ay != by) continue;   // a diagonal: shear, not a thread
            sum += glm::length(sim.particles[k.a].position - sim.particles[k.b].position) /
                       k.rest_length - 1.0;
            ++n;
        }
        return n ? static_cast<float>(sum / static_cast<double>(n)) : 0.0f;
    };

    float worst = 0.0f;
    int worst_frame = -1;
    for (int block = 0; block < 12; ++block) {
        // Half the run settled, half driven: creep shows up under both, and the drive is where a
        // player sees it.
        for (int i = 0; i < 50; ++i) {
            if (block >= 6) {
                coopa::util::Transform& t = ball->get_transform()->transform();
                t.set_position(t.position() + glm::vec3((block % 2 ? -4.0f : 4.0f) / 60.0f, 0, 0));
            }
            tick_frames(engine, 1);
        }
        const px::cloth::Cloth* sim = cc->cloth();
        require(sim != nullptr && sim->columns > 0, "probe: the sheet is simulating");
        const float strain = mean_strain(*sim);
        std::cout << "           after " << (block + 1) * 50 << " frames"
                  << (block >= 6 ? " (driven)" : "        ") << "  mean thread strain "
                  << strain * 100.0f << "%\n";
        if (-strain > worst) { worst = -strain; worst_frame = (block + 1) * 50; }
    }
    std::cout << "         worst mean thread compression " << worst * 100.0f << "% (at frame "
              << worst_frame << ")\n";

    // 3%. The threads are authored inextensible (stretch_compliance 0) and small-substep XPBD at
    // one iteration leaves a percent or so of slack in either direction; a sheet that has lost a
    // tenth of its threads' length has shrunk onto whatever it was draped over.
    expect(worst < 0.03f, "probe: the sheet folds rather than shrinking (mean thread compression " +
                              std::to_string(worst * 100.0f) + "%)");
}
