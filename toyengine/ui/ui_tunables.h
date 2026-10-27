/**
 * @file ui_tunables.h
 * @brief The handful of numbers a UI author wants to drag: where each panel of a screen sits.
 *
 * An immediate-mode screen states its layout in code, which is what makes it short and keeps
 * each control beside the value it edits. The one thing that is genuinely better dragged than
 * typed is *placement* -- is the health bar 20 or 28 pixels in from the corner, and does the
 * hotbar clear the chat box. So a screen declares each panel's box with a default in code, and
 * a sidecar file overrides it:
 *
 * @code
 * // combat_hud.cpp
 * const Box hp = tune_.box(ctx, "health", Anchor::TopLeft, {20, 20}, {260, 28});
 * ctx.stat_bar("hp", hp, "Health", health, max);
 * @endcode
 * @code{.yaml}
 * # assets/ui/combat_hud.tune.yaml
 * health: { anchor: TopLeft,     pos: [24, 18], size: [280, 30] }
 * hotbar: { anchor: BottomCenter, pos: [0, -40], size: [520, 64] }
 * @endcode
 *
 * The default in code is load-bearing, not a fallback for broken files: a screen runs with no
 * tunables file at all, and a panel added in code shows up in the editor with the position its
 * author gave it rather than at the origin. The file only ever contains what someone moved.
 *
 * ## How the editor sees it
 *
 * `declared()` is what the editor's Placement tab lists, and it holds what the CODE asked for
 * -- not what the file happened to contain -- so a box is editable the first time it is drawn
 * and a name the code has stopped using is never offered. The tab edits each box as numbers
 * (anchor, position, size) through set(), and save() writes the whole set.
 *
 * There is deliberately no gizmo: a box is placed by typing its numbers beside a live preview,
 * which is why nothing here reports geometry back to a picking tool.
 */

#ifndef TOYENGINE_UI_UI_TUNABLES_H
#define TOYENGINE_UI_UI_TUNABLES_H

#include <string>
#include <vector>

#include <glm/glm.hpp>

#include <uicoopa/immediate/imm.h>

namespace toy::ui {

/**
 * @struct Tunable
 * @brief One named, positionable box.
 *
 * `anchor`, `pos` and `size` are exactly Context::anchor()'s arguments: the box is pinned to
 * a part of the canvas and then moved by `pos`, so a screen keeps its corners at any
 * resolution rather than at one authored size.
 */
struct Tunable {
    std::string name;
    coopa::ui::imm::Anchor anchor = coopa::ui::imm::Anchor::TopLeft;
    glm::vec2 pos{0.0f};
    glm::vec2 size{100.0f, 24.0f};
};

/** @brief Spelling of an Anchor in a tunables file ("TopLeft"); empty for an unknown value. */
const char* anchor_name(coopa::ui::imm::Anchor a);

/** @brief Parses an Anchor spelling. Returns false (leaving `out`) for an unknown one. */
bool parse_anchor(const std::string& s, coopa::ui::imm::Anchor& out);

/** @brief Every Anchor spelling, for an editor offering them as a dropdown. */
const std::vector<std::string>& anchor_names();

/**
 * @class Tunables
 * @brief A screen's placement file: loaded once, read per frame, written by an editor.
 */
class Tunables {
public:
    /**
     * @brief Loads `<ref>.tune.yaml` from the asset roots. A missing file is not an error.
     *
     * Every screen is expected to run without one -- that is the point of the code defaults --
     * so this returns false only for a file that exists and could not be parsed, which IS
     * worth telling someone about.
     *
     * @param ref Asset-relative, extension optional ("ui/combat_hud").
     */
    bool load(const std::string& ref);

    /** @brief Where load() read from, or would write to. Empty until load() is called. */
    const std::string& path() const { return path_; }

    /**
     * @brief The box named `name`, from the file if it states one, else the code default.
     *
     * Declaring it is also what puts it in declared(), so a freshly added panel is editable
     * the first time the screen draws rather than only once someone has seeded a file by hand.
     */
    coopa::ui::imm::Box box(coopa::ui::imm::Context& ctx, std::string_view name,
                            coopa::ui::imm::Anchor anchor, glm::vec2 pos, glm::vec2 size);

    /** @brief Resolves a tunable without recording it -- for layout maths outside a frame. */
    coopa::ui::imm::Box resolve(const coopa::ui::imm::Context& ctx, const Tunable& t) const;

    /**
     * @brief Every box this screen has asked for, in first-asked order.
     *
     * The code's declarations only -- NOT the file's entries. A file may name a box the code
     * no longer draws (someone renamed it), and listing those would offer an editor panels
     * that do not exist and keep re-writing names nothing can ever remove.
     */
    const std::vector<Tunable>& declared() const { return declared_; }

    /** @brief Entries the file stated, whether or not the code has asked for them. */
    const std::vector<Tunable>& from_file() const { return file_; }

    /** @brief The tunable named `name`, or null. */
    const Tunable* find(std::string_view name) const;

    /**
     * @brief Moves or resizes a tunable, creating the entry if the file did not have one.
     *
     * What a designer's gizmo calls. Marks the set dirty; nothing is written until save().
     */
    void set(std::string_view name, coopa::ui::imm::Anchor anchor, glm::vec2 pos, glm::vec2 size);

    /**
     * @brief Drops `name`'s entry, so the screen's own default applies again.
     *
     * What "reset" means here: an override cannot be expressed as a value, only by its absence,
     * since the default lives in the code. The box reappears in declared() -- with the code's
     * arguments -- the next time the screen draws it.
     */
    void forget(std::string_view name);

    /** @brief True if set() or forget() has been called since the last load()/save(). */
    bool dirty() const { return dirty_; }

    /**
     * @brief Writes every declared box to path(), creating the file if needed.
     *
     * Writes the whole set rather than a diff, so a file is always a complete statement of
     * where a screen's panels are -- a half-stated file would silently take code defaults for
     * the rest and move on the next edit. A box the code has stopped declaring therefore
     * drops out, which is how a rename cleans up after itself.
     *
     * Fails (writing nothing) when the screen has declared no boxes, so a tool that saves
     * before the screen has drawn cannot blank an existing file.
     */
    bool save() const;

private:
    std::vector<Tunable> declared_;   ///< What the code asked for, with file overrides applied.
    std::vector<Tunable> file_;       ///< What the file stated; the override source for box().
    std::string path_;
    mutable bool dirty_ = false;
};

}  // namespace toy::ui

#endif  // TOYENGINE_UI_UI_TUNABLES_H
