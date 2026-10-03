/**
 * @file undo.h
 * @brief Snapshot undo/redo for any copyable document value.
 *
 * Every editor document is small enough to snapshot whole (a scene document holds object
 * descriptions, not mesh data; a mesh document is an EditMesh of a few thousand faces), so
 * undo is "before" and "after" values rather than per-operation inverse commands -- an
 * operation can never be undone incorrectly because its inverse was written wrong.
 *
 * Continuous edits (dragging a number, a gizmo) push with a merge key: consecutive pushes
 * with the same non-empty key extend one entry instead of flooding the stack, until
 * end_merge() (the drag ended) closes it.
 */

#ifndef TOYEDITOR_CORE_UNDO_H
#define TOYEDITOR_CORE_UNDO_H

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace toy::editor {

template <typename T>
class UndoStack {
public:
    struct Entry {
        std::string label;
        T before;
        T after;
        std::string merge_key;
    };

    explicit UndoStack(size_t limit = 200) : limit_(limit) {}

    /**
     * @brief Records an edit that took the document from `before` to `after`.
     * @param merge_key Non-empty: merges into the previous entry if it has the same open key.
     */
    void push(std::string label, T before, T after, std::string merge_key = {}) {
        redo_.clear();
        if (!merge_key.empty() && merge_open_ && !undo_.empty() && undo_.back().merge_key == merge_key) {
            undo_.back().after = std::move(after);
            return;
        }
        undo_.push_back(Entry{std::move(label), std::move(before), std::move(after), merge_key});
        merge_open_ = !merge_key.empty();
        if (undo_.size() > limit_) undo_.erase(undo_.begin());
        ++revision_;
    }

    /** @brief Closes the open merge window (a drag ended): the next push starts a new entry. */
    void end_merge() { merge_open_ = false; }

    bool can_undo() const { return !undo_.empty(); }
    bool can_redo() const { return !redo_.empty(); }
    const std::string& undo_label() const { static const std::string e; return undo_.empty() ? e : undo_.back().label; }
    const std::string& redo_label() const { static const std::string e; return redo_.empty() ? e : redo_.back().label; }

    /** @brief Pops the last edit. @return The value to restore (the edit's `before`). */
    const T* undo() {
        if (undo_.empty()) return nullptr;
        merge_open_ = false;
        redo_.push_back(std::move(undo_.back()));
        undo_.pop_back();
        ++revision_;
        return &redo_.back().before;
    }

    /** @brief Re-applies the last undone edit. @return The value to restore (its `after`). */
    const T* redo() {
        if (redo_.empty()) return nullptr;
        merge_open_ = false;
        undo_.push_back(std::move(redo_.back()));
        redo_.pop_back();
        ++revision_;
        return &undo_.back().after;
    }

    void clear() { undo_.clear(); redo_.clear(); merge_open_ = false; ++revision_; }
    size_t undo_count() const { return undo_.size(); }
    size_t redo_count() const { return redo_.size(); }
    /** @brief Bumps on every push/undo/redo -- lets a document compare against its saved revision. */
    uint64_t revision() const { return revision_; }

private:
    size_t limit_;
    std::vector<Entry> undo_;
    std::vector<Entry> redo_;
    bool merge_open_ = false;
    uint64_t revision_ = 0;
};

} // namespace toy::editor

#endif // TOYEDITOR_CORE_UNDO_H
