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
 *
 * Every entry carries a sequence number from one counter shared by ALL stacks (any T), so an
 * editor can merge several documents' histories into one Ctrl+Z timeline: undo the newest top
 * across stacks, redo the oldest-undone one. redo_fresh() tells whether a stack's redo is still
 * valid in that timeline -- any push anywhere since its last undo invalidates it, as a new edit
 * does in a single stack.
 *
 * History is bounded by a step count and, optionally, a memory budget (set_budget()): big
 * snapshots (a dense mesh) trim the oldest steps first, never below a minimum kept.
 */

#ifndef TOYEDITOR_CORE_UNDO_H
#define TOYEDITOR_CORE_UNDO_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace toy::editor {

/// Shared by every UndoStack: bumps on every push (a merge included). See the file doc.
inline uint64_t g_undo_sequence = 0;

template <typename T>
class UndoStack {
public:
    struct Entry {
        std::string label;
        T before;
        T after;
        std::string merge_key;
        uint64_t seq = 0;      ///< g_undo_sequence at the (last merged) push.
        size_t   bytes = 0;    ///< Estimated memory, when a budget is set.
    };

    explicit UndoStack(size_t limit = 200) : limit_(limit) {}

    /**
     * @brief Bounds history by memory as well as count: once the estimated size of every entry
     *        (`sizer` of before + after) exceeds `bytes`, the oldest steps go -- but at least
     *        `min_keep` always stay.
     */
    void set_budget(size_t bytes, std::function<size_t(const T&)> sizer, size_t min_keep = 16) {
        budget_ = bytes;
        sizer_ = std::move(sizer);
        min_keep_ = min_keep;
    }
    /** @brief Estimated memory of the whole history (0 without a budget). */
    size_t bytes() const { return bytes_; }

    /**
     * @brief Records an edit that took the document from `before` to `after`.
     * @param merge_key Non-empty: merges into the previous entry if it has the same open key.
     */
    void push(std::string label, T before, T after, std::string merge_key = {}) {
        for (const Entry& e : redo_) bytes_ -= e.bytes;
        redo_.clear();
        if (!merge_key.empty() && merge_open_ && !undo_.empty() && undo_.back().merge_key == merge_key) {
            Entry& e = undo_.back();
            e.after = std::move(after);
            e.seq = ++g_undo_sequence;
            if (sizer_) {
                bytes_ -= e.bytes;
                e.bytes = sizer_(e.before) + sizer_(e.after);
                bytes_ += e.bytes;
            }
            return;
        }
        Entry e{std::move(label), std::move(before), std::move(after), merge_key, ++g_undo_sequence, 0};
        if (sizer_) e.bytes = sizer_(e.before) + sizer_(e.after);
        bytes_ += e.bytes;
        undo_.push_back(std::move(e));
        merge_open_ = !merge_key.empty();
        while (undo_.size() > limit_ || (sizer_ && bytes_ > budget_ && undo_.size() > min_keep_)) {
            bytes_ -= undo_.front().bytes;
            undo_.erase(undo_.begin());
        }
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
        redo_epoch_ = g_undo_sequence;
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

    void clear() { undo_.clear(); redo_.clear(); merge_open_ = false; bytes_ = 0; ++revision_; }
    size_t undo_count() const { return undo_.size(); }

    /** @brief Sequence number of the next step undo() would take (0 if none). */
    uint64_t top_undo_seq() const { return undo_.empty() ? 0 : undo_.back().seq; }
    /** @brief Sequence number of the next step redo() would take (0 if none). */
    uint64_t top_redo_seq() const { return redo_.empty() ? 0 : redo_.back().seq; }
    /** @brief Redo is still meaningful in a merged timeline: no push anywhere since our last undo. */
    bool redo_fresh() const { return !redo_.empty() && redo_epoch_ == g_undo_sequence; }
    size_t redo_count() const { return redo_.size(); }
    /** @brief Bumps on every push/undo/redo -- lets a document compare against its saved revision. */
    uint64_t revision() const { return revision_; }

private:
    size_t limit_;
    std::vector<Entry> undo_;
    std::vector<Entry> redo_;
    bool merge_open_ = false;
    uint64_t revision_ = 0;
    uint64_t redo_epoch_ = 0;
    size_t budget_ = 0;
    size_t min_keep_ = 16;
    size_t bytes_ = 0;
    std::function<size_t(const T&)> sizer_;
};

} // namespace toy::editor

#endif // TOYEDITOR_CORE_UNDO_H
