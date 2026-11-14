"""The typed asset builders, against the shapes the committed assets use."""

import pytest

from toyproject import Material, Mesh, Obj, ObjectAsset, Project, Scene, rgb, vec3
from toyproject.assets import write_asset


@pytest.fixture
def project(tmp_path):
    (tmp_path / "demo.toy").write_text("target: demo\n")
    return Project(tmp_path)


class TestVectors:
    def test_a_vec3_is_an_inline_xyz_mapping(self):
        from toyproject.yamlwrite import flow
        assert flow(vec3((0.0, 1.5, -2.0))) == "{ x: 0.0, y: 1.5, z: -2.0 }"

    def test_a_colour_is_an_inline_rgb_mapping(self):
        from toyproject.yamlwrite import flow
        assert flow(rgb((0.85, 0.15, 0.12))) == "{ r: 0.85, g: 0.15, b: 0.12 }"

    def test_a_fourth_colour_component_becomes_alpha(self):
        from toyproject.yamlwrite import flow
        assert flow(rgb((1, 1, 1, 0.5))) == "{ r: 1.0, g: 1.0, b: 1.0, a: 0.5 }"

    @pytest.mark.parametrize("bad", [(1, 2), (1, 2, 3, 4)])
    def test_a_vec3_needs_exactly_three_numbers(self, bad):
        with pytest.raises(ValueError, match="three numbers"):
            vec3(bad)

    def test_both_helpers_accept_their_own_output(self):
        # A caller may hand either form to any method taking a vector or a colour; the helpers
        # being public and not accepting what they return is the obvious way to trip over.
        assert vec3(vec3((1, 2, 3))).value == vec3((1, 2, 3)).value
        assert rgb(rgb((1, 0, 0))).value == rgb((1, 0, 0)).value

    def test_a_prebuilt_colour_reaches_a_convenience_method(self):
        got = Obj("sun").directional_light(color=rgb((1, 0.95, 0.9))).to_yaml()
        assert got["components"][0]["color"].value == {"r": 1.0, "g": 0.95, "b": 0.9}

    def test_a_prebuilt_vector_reaches_a_convenience_method(self):
        got = Obj("x").transform(position=vec3((1, 2, 3))).to_yaml()
        assert got["components"][0]["position"].value == {"x": 1.0, "y": 2.0, "z": 3.0}


class TestObj:
    def test_transform_writes_only_the_keys_given(self):
        got = Obj("x").transform(position=(1, 2, 3)).to_yaml()
        assert list(got["components"][0]) == ["type", "position"]

    def test_an_empty_transform_is_refused(self):
        with pytest.raises(ValueError, match="at least one of"):
            Obj("x").transform()

    def test_children_are_written_explicitly_as_an_empty_list(self):
        assert Obj("x").to_yaml()["children"] == []

    def test_a_shared_material_is_a_plain_path(self):
        got = Obj("x").mesh("cube", material="materials/plastic").to_yaml()
        assert got["components"][0]["material"] == "materials/plastic"

    def test_an_inline_material_mapping_stays_on_one_line(self):
        from toyproject.yamlwrite import Flow
        got = Obj("x").mesh("cube", material={"base": "materials/plastic"}).to_yaml()
        assert isinstance(got["components"][0]["material"], Flow)

    def test_component_passes_any_type_and_keys_through(self):
        got = Obj("x").component("SnowDeformer", radius=0.35, falloff=0.5).to_yaml()
        assert got["components"][0] == {"type": "SnowDeformer", "radius": 0.35,
                                        "falloff": 0.5}

    def test_a_prefab_instance_matches_the_committed_shape(self):
        # name, prefab, then the overriding components -- no `active`, no empty `children`.
        got = Obj("campfire", prefab="objects/campfire").transform(
            position=(0, 0, 0)).to_yaml()
        assert list(got) == ["name", "prefab", "components"]
        assert got["prefab"] == "objects/campfire"

    def test_a_prefab_instance_keeps_children_when_it_has_them(self):
        got = Obj("a", prefab="objects/x").child(Obj("b")).to_yaml()
        assert "children" in got

    def test_the_builder_methods_chain(self):
        obj = Obj("x").transform(position=(0, 0, 0)).mesh("cube").child(Obj("y"))
        assert len(obj.components) == 2 and len(obj.children) == 1


class TestScene:
    def test_it_writes_the_committed_top_level_shape(self, project):
        scene = Scene("demo").add(Obj("ground").mesh("plane"))
        text = scene.to_text()
        assert text.startswith("scene:\n  scene_name: demo\n  root_objects:\n")
        assert "    - name: ground" in text

    def test_root_objects_are_separated_by_a_blank_line(self, project):
        scene = Scene("demo").add(Obj("a").mesh("plane")).add(Obj("b").mesh("cube"))
        assert "\n\n    - name: b" in scene.to_text()

    def test_settings_are_passed_through_under_settings(self):
        from toyproject.yamlwrite import Flow
        scene = Scene("demo", settings={"loading": Flow({"min_display_time": 4.0})})
        scene.add(Obj("a").mesh("plane"))
        assert "  settings:\n    loading: { min_display_time: 4.0 }" in scene.to_text()

    def test_an_empty_scene_is_refused(self):
        with pytest.raises(ValueError, match="no root objects"):
            Scene("demo").to_text()

    def test_write_puts_it_at_scenes_name_scene_yaml(self, project):
        scene = Scene("demo").add(Obj("a").mesh("plane"))
        written = scene.write(project, "effects/demo")
        assert written == project.assets / "scenes/effects/demo/scene.yaml"
        assert written.read_text().startswith("scene:")

    def test_the_header_becomes_the_leading_comment(self):
        scene = Scene("demo", header="What this is.").add(Obj("a").mesh("plane"))
        assert scene.to_text().startswith("# What this is.\nscene:")


class TestMaterial:
    def test_the_four_core_keys_lead_in_committed_order(self):
        text = Material().to_text()
        assert text.split("\n")[0].startswith("albedo:")
        assert [line.split(":")[0] for line in text.strip().split("\n")] \
            == ["albedo", "metallic", "roughness", "ao"]

    def test_extra_keys_follow_in_the_order_given(self):
        text = Material(shader="foliage", alpha_mode="BLEND").to_text()
        keys = [line.split(":")[0] for line in text.strip().split("\n")]
        assert keys[4:] == ["shader", "alpha_mode"]

    def test_a_list_key_is_written_as_a_flow_sequence(self):
        text = Material(shader="foliage", shader_params=[0.10, 0.7, 4.5, 0]).to_text()
        assert "shader_params: [0.1, 0.7, 4.5, 0]" in text

    def test_a_tint_key_is_spelled_as_a_colour(self):
        text = Material(refraction_tint=(0.92, 0.96, 1.0)).to_text()
        assert "refraction_tint: { r: 0.92, g: 0.96, b: 1.0 }" in text

    def test_write_lands_under_materials(self, project):
        written = Material().write(project, "basic/x")
        assert written == project.assets / "materials/basic/x.yaml"


class TestObjectAsset:
    def test_it_wraps_the_root_under_object(self, project):
        text = ObjectAsset(Obj("TreeBranch").mesh("props/branch")).to_text()
        assert text.startswith("object:\n  name: TreeBranch\n")

    def test_write_lands_under_objects(self, project):
        written = ObjectAsset(Obj("X")).write(project, "props/x")
        assert written == project.assets / "objects/props/x.yaml"


class TestGenericWriter:
    def test_it_writes_a_kind_with_no_typed_builder(self, project):
        written = write_asset(project, "physics_material", "ice",
                              {"friction": 0.05, "bounciness": 0.1})
        assert written == project.assets / "physics_materials/ice.yaml"
        assert written.read_text() == "friction: 0.05\nbounciness: 0.1\n"

    @pytest.mark.parametrize("binary", ["texture", "palette", "audio", "font"])
    def test_a_binary_kind_is_refused_rather_than_written_as_yaml(self, project, binary):
        with pytest.raises(ValueError, match="not a YAML asset"):
            write_asset(project, binary, "x", {"a": 1})


class TestMeshValidation:
    def test_parallel_arrays_must_match_the_vertex_count(self):
        with pytest.raises(ValueError, match="parallel"):
            Mesh([[0, 0, 0], [1, 0, 0], [0, 1, 0]], [[0, 1, 2]], uvs=[[0, 0]])

    def test_a_face_may_not_index_a_missing_vertex(self):
        with pytest.raises(ValueError, match="outside"):
            Mesh([[0, 0, 0]], [[0, 1, 2]], uvs=[[0, 0]])

    def test_a_face_needs_at_least_three_vertices(self):
        verts = [[0, 0, 0], [1, 0, 0], [0, 1, 0]]
        with pytest.raises(ValueError, match="at least 3"):
            Mesh(verts, [[0, 1]], uvs=[[0, 0]] * 3)
