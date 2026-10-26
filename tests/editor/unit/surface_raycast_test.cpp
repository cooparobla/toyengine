/**
 * @file surface_raycast_test.cpp
 * @brief Finding the surface under a ray (editor/viewport/surface_raycast.h): nearest wins,
 *        normals always face the viewer, a terrain is marched in either mode, and a mesh's
 *        transform (including a non-uniform scale) is respected.
 *
 * Pure geometry: the raycaster is offered candidates rather than walking a scene, so none of
 * this needs an editor, a document or a device.
 */

#include <coopa/testing/test.h>

#include <cmath>
#include <memory>

#include <glm/gtc/matrix_transform.hpp>

#include "editor/mesh/primitives.h"
#include "editor/viewport/surface_raycast.h"

#include <toyengine/world/terrain_field.h>

COOPA_TEST_SUITE("surface_raycast");

namespace toy::editor::testing {

using coopa::test::expect;
using coopa::test::expect_near;
using coopa::test::require;

namespace {

/** @brief A 10 x 10 m plane at z = 0, with its BVH, ready to offer to a raycaster. */
struct Plane {
    EditMesh mesh;
    TriangleBVH bvh;
    Plane() {
        mesh = make_plane(10.0f, 2);
        bvh.build(mesh);
    }
};

glm::mat4 translate(glm::vec3 p) { return glm::translate(glm::mat4(1.0f), p); }

} // namespace

COOPA_TEST(a_ray_down_onto_a_plane_hits_it_with_an_upward_normal) {
    Plane plane;
    SurfaceRaycaster rc(glm::vec3(1.0f, 2.0f, 50.0f), glm::vec3(0.0f, 0.0f, -1.0f));
    rc.consider_mesh(7, plane.mesh, plane.bvh, glm::mat4(1.0f));
    const SurfaceHit* hit = rc.best();
    require(hit != nullptr, "surface_raycast: a ray down onto a plane finds it");
    expect_near(hit->point.z, 0.0f, 1e-4f, "surface_raycast: ...at the plane's height");
    expect_near(hit->point.x, 1.0f, 1e-4f, "surface_raycast: ...under the ray, not somewhere else");
    expect_near(hit->normal.z, 1.0f, 1e-4f,
                "surface_raycast: the normal faces back along the ray, so a brush never plants "
                "anything upside down on a back face");
    expect(hit->object == 7, "surface_raycast: the hit carries the object it belongs to");
    expect_near(hit->t, 50.0f, 1e-3f, "surface_raycast: t is the distance along the ray");
}

COOPA_TEST(the_nearest_surface_wins_whatever_order_they_are_offered_in) {
    Plane plane;
    SurfaceRaycaster rc(glm::vec3(0.0f, 0.0f, 50.0f), glm::vec3(0.0f, 0.0f, -1.0f));
    // Far one first, then the near one.
    rc.consider_mesh(1, plane.mesh, plane.bvh, translate(glm::vec3(0.0f, 0.0f, -5.0f)));
    rc.consider_mesh(2, plane.mesh, plane.bvh, translate(glm::vec3(0.0f, 0.0f, 10.0f)));
    require(rc.best() != nullptr, "surface_raycast: something was hit");
    expect(rc.best()->object == 2,
           "surface_raycast: the nearer surface wins even though it was offered second");
    expect_near(rc.best()->point.z, 10.0f, 1e-4f, "surface_raycast: ...and the hit is on it");

    // And the other way round, to prove it is not just "last wins".
    SurfaceRaycaster rc2(glm::vec3(0.0f, 0.0f, 50.0f), glm::vec3(0.0f, 0.0f, -1.0f));
    rc2.consider_mesh(2, plane.mesh, plane.bvh, translate(glm::vec3(0.0f, 0.0f, 10.0f)));
    rc2.consider_mesh(1, plane.mesh, plane.bvh, translate(glm::vec3(0.0f, 0.0f, -5.0f)));
    require(rc2.best() != nullptr, "surface_raycast: something was hit the other way round too");
    expect(rc2.best()->object == 2, "surface_raycast: the nearer one still wins");
}

COOPA_TEST(a_ray_that_misses_everything_reports_no_hit) {
    Plane plane;
    // The plane is 10 m across, centred on the origin; 100 m out is well clear of it.
    SurfaceRaycaster rc(glm::vec3(100.0f, 100.0f, 50.0f), glm::vec3(0.0f, 0.0f, -1.0f));
    rc.consider_mesh(1, plane.mesh, plane.bvh, glm::mat4(1.0f));
    expect(rc.best() == nullptr,
           "surface_raycast: a ray beside the geometry reports nothing rather than a nearest-ish guess");
}

COOPA_TEST(a_degenerate_transform_is_skipped_instead_of_inverted) {
    Plane plane;
    SurfaceRaycaster rc(glm::vec3(0.0f, 0.0f, 50.0f), glm::vec3(0.0f, 0.0f, -1.0f));
    const glm::mat4 flat = glm::scale(glm::mat4(1.0f), glm::vec3(1.0f, 1.0f, 0.0f));
    rc.consider_mesh(1, plane.mesh, plane.bvh, flat);
    expect(rc.best() == nullptr,
           "surface_raycast: an object scaled to nothing has no surface, and inverting its matrix "
           "would produce NaNs rather than a miss");
}

COOPA_TEST(a_non_uniform_scale_does_not_shear_the_normal_off_the_surface) {
    // A plane tilted 45 degrees, then squashed in z: the face normal and the inverse-transpose
    // normal disagree, and only the latter is still perpendicular to the surface.
    EditMesh mesh = make_plane(10.0f, 1);
    TriangleBVH bvh;
    bvh.build(mesh);
    const glm::mat4 world = glm::scale(glm::mat4(1.0f), glm::vec3(1.0f, 1.0f, 0.25f)) *
                            glm::rotate(glm::mat4(1.0f), glm::radians(45.0f), glm::vec3(1.0f, 0.0f, 0.0f));
    SurfaceRaycaster rc(glm::vec3(0.0f, 0.0f, 50.0f), glm::vec3(0.0f, 0.0f, -1.0f));
    rc.consider_mesh(1, mesh, bvh, world);
    const SurfaceHit* hit = rc.best();
    require(hit != nullptr, "surface_raycast: the tilted, squashed plane is hit");
    expect_near(glm::length(hit->normal), 1.0f, 1e-4f, "surface_raycast: the normal is unit length");

    // Walk a little along the surface and check the normal is still perpendicular to the step.
    SurfaceRaycaster near(glm::vec3(0.0f, 0.5f, 50.0f), glm::vec3(0.0f, 0.0f, -1.0f));
    near.consider_mesh(1, mesh, bvh, world);
    require(near.best() != nullptr, "surface_raycast: a nearby ray hits the same surface");
    const glm::vec3 along = glm::normalize(near.best()->point - hit->point);
    expect_near(glm::dot(hit->normal, along), 0.0f, 1e-3f,
                "surface_raycast: the normal stays perpendicular to the surface under a "
                "non-uniform scale -- the inverse transpose, not the matrix");
}

COOPA_TEST(a_heightmap_terrain_is_marched_and_bisected_to_its_surface) {
    using toy::world::TerrainField;
    using toy::world::TerrainFieldGeometry;
    using toy::world::TerrainFieldView;

    TerrainFieldGeometry g;
    g.mode = toy::world::TerrainMode::Heightmap;
    g.tiles_x = 16;
    g.tiles_y = 16;
    g.size = glm::vec2(64.0f);
    g.height_sub = 2;
    g.height = 20.0f;
    TerrainField field;
    field.reset(g);
    require(field.heights() != nullptr, "surface_raycast: a Heightmap field owns a height plane");
    // A ramp climbing in +x, so the hit height depends on where the ray comes down.
    toy::world::HeightmapData& plane_data = *field.heights();
    const int n = plane_data.nx();
    for (int y = 0; y < plane_data.ny(); ++y) {
        for (int x = 0; x < n; ++x) {
            plane_data.at(x, y) = 10.0f * static_cast<float>(x) / static_cast<float>(n - 1);
        }
    }
    plane_data.mark_all_dirty();

    TerrainFieldView view{&field, glm::vec3(0.0f)};
    const glm::vec2 probe(40.0f, 30.0f);
    const float expected = view.height(glm::vec3(probe, 0.0f));

    SurfaceRaycaster rc(glm::vec3(probe, 100.0f), glm::vec3(0.0f, 0.0f, -1.0f));
    rc.consider_terrain(5, view);
    const SurfaceHit* hit = rc.best();
    require(hit != nullptr, "surface_raycast: a ray down onto a heightmap terrain finds its ground");
    expect_near(hit->point.z, expected, 0.01f,
                "surface_raycast: the marched-and-bisected hit agrees with the field's own height "
                "to a centimetre -- the field has no raycast of its own, so this is how a brush "
                "lands on either kind of terrain");
    expect_near(hit->point.x, probe.x, 1e-3f, "surface_raycast: ...directly under the ray");
    expect(hit->normal.z > 0.0f, "surface_raycast: the terrain normal points up");
    expect(hit->normal.x < -0.01f,
           "surface_raycast: on a slope climbing in +x the normal leans back down it");
    expect(hit->object == 5, "surface_raycast: the hit names the terrain's object");
}

COOPA_TEST(a_ray_outside_the_terrains_footprint_misses_it) {
    using toy::world::TerrainField;
    using toy::world::TerrainFieldGeometry;
    using toy::world::TerrainFieldView;

    TerrainFieldGeometry g;
    g.mode = toy::world::TerrainMode::Heightmap;
    g.tiles_x = 4;
    g.tiles_y = 4;
    g.size = glm::vec2(16.0f);
    g.height_sub = 1;
    g.height = 10.0f;
    TerrainField field;
    field.reset(g);
    TerrainFieldView view{&field, glm::vec3(0.0f)};

    SurfaceRaycaster rc(glm::vec3(500.0f, 500.0f, 100.0f), glm::vec3(0.0f, 0.0f, -1.0f));
    rc.consider_terrain(1, view);
    expect(rc.best() == nullptr,
           "surface_raycast: beyond the terrain's edge there is no ground, so painting there "
           "places nothing rather than clamping to the border");
}

COOPA_TEST(a_terrain_and_a_mesh_above_it_resolve_by_distance) {
    using toy::world::TerrainField;
    using toy::world::TerrainFieldGeometry;
    using toy::world::TerrainFieldView;

    TerrainFieldGeometry g;
    g.mode = toy::world::TerrainMode::Heightmap;
    g.tiles_x = 8;
    g.tiles_y = 8;
    g.size = glm::vec2(32.0f);
    g.height_sub = 1;
    g.height = 10.0f;
    TerrainField field;
    field.reset(g);   // flat at 0
    TerrainFieldView view{&field, glm::vec3(0.0f)};

    Plane plane;   // a rock, 3 m up over the middle of it
    SurfaceRaycaster rc(glm::vec3(16.0f, 16.0f, 50.0f), glm::vec3(0.0f, 0.0f, -1.0f));
    rc.consider_terrain(1, view);
    rc.consider_mesh(2, plane.mesh, plane.bvh, translate(glm::vec3(16.0f, 16.0f, 3.0f)));
    require(rc.best() != nullptr, "surface_raycast: one of the two was hit");
    expect(rc.best()->object == 2,
           "surface_raycast: a mesh standing on the terrain is what the brush paints onto -- one "
           "stroke can run off a hillside and up a rock without changing tools");
    expect_near(rc.best()->point.z, 3.0f, 1e-3f, "surface_raycast: ...at the mesh's height");
}

COOPA_TEST(the_plane_fallback_answers_only_in_front_of_the_ray) {
    const auto down = ray_plane(glm::vec3(2.0f, 3.0f, 10.0f), glm::vec3(0.0f, 0.0f, -1.0f), 0.0f);
    require(down.has_value(), "surface_raycast: a downward ray meets the z = 0 plane");
    expect_near(down->z, 0.0f, 1e-5f, "surface_raycast: ...at z = 0");
    expect_near(down->x, 2.0f, 1e-5f, "surface_raycast: ...under where it started");
    expect(!ray_plane(glm::vec3(0.0f, 0.0f, 10.0f), glm::vec3(0.0f, 0.0f, 1.0f), 0.0f).has_value(),
           "surface_raycast: a ray pointing away from the plane has no hit in front of it");
    expect(!ray_plane(glm::vec3(0.0f, 0.0f, 10.0f), glm::vec3(1.0f, 0.0f, 0.0f), 0.0f).has_value(),
           "surface_raycast: a ray parallel to the plane never meets it");
}

} // namespace toy::editor::testing
