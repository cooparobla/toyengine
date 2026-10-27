/**
 * @file ui_tunables_test.cpp
 * @brief A screen's draggable placement: code defaults, file overrides, the designer's
 *        read-back path, and the round trip through disk.
 *
 * Headless -- a tunables file is YAML and anchor arithmetic.
 */

#include <coopa/testing/test.h>

#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

#include <toyengine/ui/ui_tunables.h>
#include <uicoopa/immediate/imm.h>
#include <uicoopa/render/draw_list.h>
// Before font_defaults.h, which reaches a uicoopa header that dereferences a SceneObject it
// only forward-declares.
#include <coopa/scene/scene_object.h>
#include <uicoopa/text/font_defaults.h>

COOPA_TEST_SUITE("ui_tunables");

namespace {

using coopa::ui::imm::Anchor;
using coopa::ui::imm::Box;
using coopa::ui::imm::Context;

/// A scratch asset root, published the way the engine publishes its own.
std::filesystem::path scratch_root() {
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / "coopa_tests" / "ui_tunables";
    std::filesystem::create_directories(root / "ui");
    coopa::ui::FontDefaults::search_dirs = {root.string()};
    return root;
}

/**
 * @brief Puts FontDefaults::search_dirs back on the way out.
 *
 * It is process-wide and every suite runs in one binary, so a test that left it pointing at a
 * scratch directory would quietly change what a later suite resolves.
 */
struct RootGuard {
    std::vector<std::string> saved = coopa::ui::FontDefaults::search_dirs;
    ~RootGuard() { coopa::ui::FontDefaults::search_dirs = saved; }
};

void write_tune(const std::string& name, const std::string& text) {
    std::ofstream(scratch_root() / "ui" / (name + ".tune.yaml")) << text;
}

/// One imm frame at a fixed canvas size; tunables resolve against it.
struct Frame {
    Context ctx;
    coopa::ui::DrawList dl;
    glm::vec2 size{800.0f, 600.0f};

    void run(const std::function<void(Context&)>& fn) {
        coopa::ui::imm::FrameInput in;
        dl.begin(coopa::ui::Rect{glm::vec2(0.0f), size});
        ctx.begin_frame(dl, in, size);
        fn(ctx);
        ctx.end_frame();
    }
};

}  // namespace

COOPA_TEST(a_missing_file_is_not_an_error_and_code_defaults_apply) {
    RootGuard guard;
    scratch_root();
    toy::ui::Tunables t;
    // Every screen must run with no placement file: the code default is the real layout, not
    // a fallback for a broken one.
    ASSERT_TRUE(t.load("ui/does_not_exist"));
    ASSERT_TRUE(t.declared().empty());

    Frame f;
    f.run([&](Context& c) {
        const Box b = t.box(c, "health", Anchor::TopLeft, {20.0f, 20.0f}, {260.0f, 28.0f});
        ASSERT_NEAR(b.x, 20.0f, 1e-4f);
        ASSERT_NEAR(b.y, 20.0f, 1e-4f);
        ASSERT_NEAR(b.w, 260.0f, 1e-4f);
    });

    // Asking for it registered it, so a designer lists a panel the first time it is drawn
    // rather than only after someone has already moved it.
    ASSERT_EQ(t.declared().size(), size_t(1));
    ASSERT_EQ(t.declared()[0].name, std::string("health"));
    ASSERT_TRUE(t.find("health") != nullptr);
    // And save() has somewhere to go even though nothing was loaded.
    ASSERT_TRUE(!t.path().empty());
}

COOPA_TEST(the_file_overrides_the_code_default_in_both_vector_spellings) {
    RootGuard guard;
    write_tune("over", R"(health: { anchor: TopRight, pos: [-24, 18], size: [280, 30] }
hotbar: { anchor: BottomCenter, pos: {x: 0, y: -40}, size: {x: 520, y: 64} }
)");
    toy::ui::Tunables t;
    ASSERT_TRUE(t.load("ui/over"));
    // The file's entries are from_file(); declared() fills as the code asks for each box.
    ASSERT_EQ(t.from_file().size(), size_t(2));
    ASSERT_TRUE(t.declared().empty());

    Frame f;
    f.run([&](Context& c) {
        // The code default is ignored entirely when the file states one.
        const Box hp = t.box(c, "health", Anchor::TopLeft, {20.0f, 20.0f}, {260.0f, 28.0f});
        ASSERT_NEAR(hp.right(), f.size.x - 24.0f, 1e-4f);
        ASSERT_NEAR(hp.y, 18.0f, 1e-4f);
        ASSERT_NEAR(hp.w, 280.0f, 1e-4f);

        // {x:, y:} reads the same as [x, y].
        const Box hb = t.box(c, "hotbar", Anchor::TopLeft, {0.0f, 0.0f}, {10.0f, 10.0f});
        ASSERT_NEAR(hb.bottom(), f.size.y - 40.0f, 1e-4f);
        ASSERT_NEAR(hb.w, 520.0f, 1e-4f);
    });
}

COOPA_TEST(an_unparseable_file_reports_failure_and_falls_back_to_code) {
    // A missing file is routine; a corrupt one is worth telling someone about, because the
    // screen will silently look wrong otherwise.
    write_tune("broken", "health: { anchor: TopLeft, pos: [1, 2\nthis is not yaml: : :\n");
    toy::ui::Tunables t;
    ASSERT_TRUE(!t.load("ui/broken"));
    ASSERT_TRUE(t.declared().empty());
    ASSERT_TRUE(t.from_file().empty());

    Frame f;
    f.run([&](Context& c) {
        const Box b = t.box(c, "health", Anchor::Center, {0.0f, 0.0f}, {100.0f, 20.0f});
        ASSERT_VEC_NEAR(b.center(), f.size * 0.5f, 1e-4f);
    });
}

COOPA_TEST(an_unknown_anchor_keeps_the_entrys_default_rather_than_failing) {
    RootGuard guard;
    write_tune("badanchor", "health: { anchor: Nowhere, pos: [5, 6], size: [50, 10] }\n");
    toy::ui::Tunables t;
    ASSERT_TRUE(t.load("ui/badanchor"));
    ASSERT_EQ(t.from_file().size(), size_t(1));
    // The anchor stays TopLeft (Tunable's own default) and pos/size still apply -- one
    // mistyped word should cost that word, not the whole entry.
    ASSERT_TRUE(t.from_file()[0].anchor == Anchor::TopLeft);
    ASSERT_VEC_NEAR(t.from_file()[0].pos, glm::vec2(5.0f, 6.0f), 1e-4f);
}

/**
 * @brief declared() lists what the CODE asked for, never what the file happened to contain.
 *
 * The distinction is what an editor's panel list is built from. A file that names a box the
 * code no longer draws -- someone renamed it -- must not offer that box for editing, and must
 * not keep re-writing a name nothing can remove.
 */
COOPA_TEST(declared_is_the_codes_boxes_and_the_file_only_overrides_them) {
    RootGuard guard;
    scratch_root();
    write_tune("probe_me",
               "health: { anchor: BottomLeft, pos: [7, -9], size: [300, 40] }\n"
               "gone:   { anchor: Center, pos: [0, 0], size: [10, 10] }\n");

    toy::ui::Tunables t;
    t.load("ui/probe_me");

    // Before the screen draws, nothing is declared -- the file alone declares nothing.
    ASSERT_TRUE(t.declared().empty());
    ASSERT_EQ(t.from_file().size(), size_t(2));

    Frame f;
    f.run([&](Context& c) {
        t.box(c, "health", Anchor::TopLeft, {20.0f, 20.0f}, {260.0f, 28.0f});
        t.box(c, "hotbar", Anchor::BottomCenter, {0.0f, -40.0f}, {520.0f, 64.0f});
    });

    // Exactly the two the code drew, in the order it drew them. `gone` is in the file and is
    // NOT offered, because no code declares it.
    ASSERT_EQ(t.declared().size(), size_t(2));
    ASSERT_EQ(t.declared()[0].name, std::string("health"));
    ASSERT_EQ(t.declared()[1].name, std::string("hotbar"));
    // `gone` is still KNOWN (find() sees the file, which is what an inspector reads) but it is
    // not DECLARED, so an editor's panel list never offers it.
    ASSERT_TRUE(t.find("gone") != nullptr);
    bool declared_gone = false;
    for (const toy::ui::Tunable& d : t.declared()) declared_gone = declared_gone || d.name == "gone";
    ASSERT_TRUE(!declared_gone);

    // The file wins for the box it states, and the code default stands for the one it does not.
    ASSERT_TRUE(t.declared()[0].anchor == Anchor::BottomLeft);
    ASSERT_VEC_NEAR(t.declared()[0].pos, glm::vec2(7.0f, -9.0f), 1e-4f);
    ASSERT_TRUE(t.declared()[1].anchor == Anchor::BottomCenter);
    ASSERT_VEC_NEAR(t.declared()[1].pos, glm::vec2(0.0f, -40.0f), 1e-4f);

    // Saving drops `gone`, which is how a rename cleans up after itself.
    ASSERT_TRUE(t.save());
    toy::ui::Tunables again;
    again.load("ui/probe_me");
    ASSERT_EQ(again.from_file().size(), size_t(2));
    bool has_gone = false;
    for (const toy::ui::Tunable& e : again.from_file()) has_gone = has_gone || e.name == "gone";
    ASSERT_TRUE(!has_gone);
}

/** @brief save() refuses to blank a file when the screen has not drawn. */
COOPA_TEST(save_before_the_screen_draws_writes_nothing) {
    RootGuard guard;
    scratch_root();
    write_tune("keep_me", "health: { anchor: Center, pos: [1, 2], size: [3, 4] }\n");

    toy::ui::Tunables t;
    t.load("ui/keep_me");
    ASSERT_TRUE(!t.save());                       // nothing declared -> nothing written
    toy::ui::Tunables again;
    again.load("ui/keep_me");
    ASSERT_EQ(again.from_file().size(), size_t(1));   // the file survived
}

COOPA_TEST(set_then_save_round_trips_through_disk) {
    RootGuard guard;
    scratch_root();
    toy::ui::Tunables t;
    t.load("ui/round_trip");
    ASSERT_TRUE(!t.dirty());

    Frame f;
    f.run([&](Context& c) {
        t.box(c, "health", Anchor::TopLeft, {20.0f, 20.0f}, {260.0f, 28.0f});
        t.box(c, "hotbar", Anchor::BottomCenter, {0.0f, -40.0f}, {520.0f, 64.0f});
    });

    // A designer drags the health bar.
    t.set("health", Anchor::TopRight, glm::vec2(-30.0f, 12.0f), glm::vec2(300.0f, 32.0f));
    ASSERT_TRUE(t.dirty());
    ASSERT_TRUE(t.save());
    ASSERT_TRUE(!t.dirty());

    // Saving writes the WHOLE set, not just what moved: a half-stated file would take code
    // defaults for the rest and shift again on the next edit.
    toy::ui::Tunables reloaded;
    ASSERT_TRUE(reloaded.load("ui/round_trip"));
    ASSERT_EQ(reloaded.from_file().size(), size_t(2));
    const toy::ui::Tunable* hp = reloaded.find("health");
    ASSERT_TRUE(hp != nullptr);
    ASSERT_TRUE(hp->anchor == Anchor::TopRight);
    ASSERT_VEC_NEAR(hp->pos, glm::vec2(-30.0f, 12.0f), 1e-3f);
    ASSERT_VEC_NEAR(hp->size, glm::vec2(300.0f, 32.0f), 1e-3f);
    const toy::ui::Tunable* hb = reloaded.find("hotbar");
    ASSERT_TRUE(hb != nullptr);
    ASSERT_TRUE(hb->anchor == Anchor::BottomCenter);

    // set() on a name nobody declared creates it, so a designer can place a panel a screen
    // has not drawn yet without the file losing it.
    t.set("minimap", Anchor::TopRight, glm::vec2(-10.0f, 10.0f), glm::vec2(160.0f, 160.0f));
    ASSERT_TRUE(t.find("minimap") != nullptr);
}

COOPA_TEST(every_anchor_spelling_round_trips) {
    // anchor_name() and parse_anchor() walk one list, so this catches a value that is
    // spellable but not parseable (or the reverse) the moment one is added.
    const std::vector<std::string>& names = toy::ui::anchor_names();
    ASSERT_EQ(names.size(), size_t(9));
    for (const std::string& n : names) {
        Anchor a = Anchor::Center;
        ASSERT_TRUE(toy::ui::parse_anchor(n, a));
        ASSERT_EQ(std::string(toy::ui::anchor_name(a)), n);
    }
    Anchor unused = Anchor::TopLeft;
    ASSERT_TRUE(!toy::ui::parse_anchor("NotAnAnchor", unused));
}
