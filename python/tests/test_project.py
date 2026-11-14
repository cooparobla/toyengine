"""Project discovery and asset path resolution."""

import pytest

from toyproject import Project


@pytest.fixture
def project(tmp_path):
    """A minimal project: a .toy file is all that makes a directory one."""
    (tmp_path / "demo.toy").write_text("target: demo\n")
    return Project(tmp_path)


class TestFind:
    def test_it_finds_the_project_from_a_nested_directory(self, project):
        deep = project.root / "assets" / "scenes" / "area"
        deep.mkdir(parents=True)
        assert Project.find(deep).root == project.root

    def test_it_accepts_a_file_and_searches_from_its_directory(self, project):
        script = project.root / "gen.py"
        script.write_text("")
        assert Project.find(script).root == project.root

    def test_it_reports_where_it_looked_when_there_is_no_project(self, tmp_path):
        with pytest.raises(FileNotFoundError, match="no .toy project file"):
            Project.find(tmp_path)

    def test_a_directory_is_required(self, tmp_path):
        missing = tmp_path / "nope"
        with pytest.raises(NotADirectoryError):
            Project(missing)


class TestLayout:
    def test_the_name_is_the_folder_s_name(self, project):
        assert project.name == project.root.name

    def test_a_scene_is_a_directory_with_scene_yaml_inside(self, project):
        got = project.path_for("scene", "effects/my_demo")
        assert got == project.assets / "scenes/effects/my_demo/scene.yaml"

    def test_other_kinds_are_single_files_and_gain_their_suffix(self, project):
        assert project.path_for("material", "basic/plastic") \
            == project.assets / "materials/basic/plastic.yaml"
        assert project.path_for("texture", "grid") == project.assets / "textures/grid.png"

    def test_an_explicit_suffix_is_not_doubled(self, project):
        assert project.path_for("material", "basic/plastic.yaml") \
            == project.assets / "materials/basic/plastic.yaml"

    def test_a_ui_theme_nests_under_ui(self, project):
        assert project.path_for("ui_theme", "dark") == project.assets / "ui/themes/dark.yaml"

    def test_an_unknown_kind_lists_the_known_ones(self, project):
        with pytest.raises(KeyError, match="unknown asset kind"):
            project.dir_for("sprite")

    @pytest.mark.parametrize("bad", ["", "/absolute", "../escape", "a/../../escape"])
    def test_a_name_may_not_escape_the_assets_tree(self, project, bad):
        with pytest.raises(ValueError):
            project.path_for("material", bad)


class TestEngineDir:
    def test_it_follows_the_libs_symlink(self, project, tmp_path):
        engine = tmp_path / "engine_checkout"
        (engine / "src" / "toyengine").mkdir(parents=True)
        libs = project.root / ".libs"
        libs.mkdir()
        (libs / "toyengine").symlink_to(engine)
        assert project.engine_dir == engine.resolve()

    def test_it_is_none_when_setup_has_not_run(self, project):
        assert project.engine_dir is None

    def test_an_engine_checkout_is_its_own_engine_dir(self, tmp_path):
        (tmp_path / "src" / "toyengine").mkdir(parents=True)
        assert Project(tmp_path).engine_dir == tmp_path.resolve()


class TestWriting:
    def test_write_text_creates_parents_and_records_the_path(self, project):
        written = project.write_text("assets/materials/basic/x.yaml", "albedo: 1\n")
        assert written.read_text() == "albedo: 1\n"
        assert project.written == [written]

    def test_the_report_is_relative_to_the_project_root(self, project):
        project.write_text("assets/materials/basic/x.yaml", "")
        assert project.report() == "wrote assets/materials/basic/x.yaml"

    def test_the_written_list_is_a_copy(self, project):
        project.write_text("a.yaml", "")
        project.written.append("not recorded")
        assert len(project.written) == 1
