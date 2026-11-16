#include <toyengine/ui/quadrant_layout.h>

#include <algorithm>
#include <limits>

namespace toy::ui {

using coopa::ui::imm::Box;

namespace {
constexpr int kTL = static_cast<int>(Slot::TopLeft);
constexpr int kTR = static_cast<int>(Slot::TopRight);
constexpr int kBL = static_cast<int>(Slot::BottomLeft);
constexpr int kBR = static_cast<int>(Slot::BottomRight);

/** @brief The four base quadrants for `area` cut at `split`, clamped away from the edges. */
std::array<Box, 4> base_quadrants(const Box& area, QuadrantSplit split) {
    const float fx = std::clamp(split.x, kMinSplit, kMaxSplit);
    const float fy = std::clamp(split.y, kMinSplit, kMaxSplit);
    const float hw = area.w * fx;
    const float hh = area.h * fy;
    return {
        Box{area.x, area.y, hw, hh},                             // TopLeft
        Box{area.x + hw, area.y, area.w - hw, hh},               // TopRight
        Box{area.x, area.y + hh, hw, area.h - hh},               // BottomLeft
        Box{area.x + hw, area.y + hh, area.w - hw, area.h - hh}, // BottomRight
    };
}
}  // namespace

std::array<Box, 4> quadrant_layout(const Box& area, const std::array<bool, 4>& open, QuadrantSplit split) {
    const std::array<Box, 4> qq = base_quadrants(area, split);
    const Box* q = qq.data();
    std::array<Box, 4> out = {q[kTL], q[kTR], q[kBL], q[kBR]};

    const int n = (open[kTL] ? 1 : 0) + (open[kTR] ? 1 : 0) + (open[kBL] ? 1 : 0) + (open[kBR] ? 1 : 0);

    // Both halves of a row/column always get the SAME box: the closed half's entry is never
    // drawn (the caller checks `open[i]` first), so setting it too is harmless and keeps this
    // function simple rather than conditional on which half is actually open.
    auto full_row = [&](int a, int b) {
        const Box r{area.x, q[a].y, area.w, q[a].h};
        out[a] = r; out[b] = r;
    };
    auto full_col = [&](int a, int b) {
        const Box c{q[a].x, area.y, q[a].w, area.h};
        out[a] = c; out[b] = c;
    };

    if (n == 1) {
        for (int i = 0; i < 4; ++i) {
            if (open[i]) out[i] = area;
        }
    } else if (n == 2) {
        if ((open[kTL] && open[kTR]) || (open[kBL] && open[kBR])) {
            // Same row open: each grows DOWN/UP into the empty cell in its own column.
            full_col(kTL, kBL);
            full_col(kTR, kBR);
        } else if ((open[kTL] && open[kBL]) || (open[kTR] && open[kBR])) {
            // Same column open: each grows sideways into the empty cell in its own row.
            full_row(kTL, kTR);
            full_row(kBL, kBR);
        } else {
            // Diagonal (TopLeft+BottomRight or TopRight+BottomLeft): each grows sideways into
            // the empty cell in its OWN row, rather than down/up into the other diagonal cell
            // -- the one choice that gives both panels a uniform full-width half instead of
            // one full-height and one untouched quadrant.
            full_row(kTL, kTR);
            full_row(kBL, kBR);
        }
    } else if (n == 3) {
        // Exactly one slot is closed. Its COLUMN-mate -- the other cell in that same column,
        // which is open, since only one slot total is closed -- is the panel "alone in its
        // column", and it absorbs the closed slot's half to span the full column height. The
        // other two open panels never had an empty neighbour, so they stay single quadrants
        // (already correct in `out` from the quadrant defaults above).
        int closed = -1;
        for (int i = 0; i < 4; ++i) {
            if (!open[i]) { closed = i; break; }
        }
        if (closed == kTL || closed == kBL) full_col(kTL, kBL);
        else full_col(kTR, kBR);
    }
    // n == 0 (nothing drawn) and n == 4 (four quadrants, unchanged) both keep the defaults.

    return out;
}

QuadrantDividers quadrant_dividers(const Box& area, const std::array<bool, 4>& open,
                                   QuadrantSplit split, float grab) {
    // Which PANEL covers each base quadrant, read back out of the layout itself rather than
    // re-deriving it from `open`. A grown panel still forms a boundary with its neighbour --
    // with three open, the tall right-hand panel borders BOTH left quadrants -- and deciding
    // that from the open flags alone got exactly that case wrong. Asking the boxes cannot
    // drift from the growth rules, because it IS the growth rules.
    const std::array<Box, 4> boxes = quadrant_layout(area, open, split);
    const std::array<Box, 4> q = base_quadrants(area, split);
    std::array<int, 4> owner{-1, -1, -1, -1};
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) {
            if (!open[j]) continue;
            if (boxes[j].contains(q[i].center())) { owner[i] = j; break; }
        }
    }
    // A boundary exists between two cells when both are covered and by DIFFERENT panels:
    // same panel means it grew across and there is nothing there to drag; uncovered means
    // there is no second panel to resize against.
    auto boundary = [&](int a, int b) { return owner[a] >= 0 && owner[b] >= 0 && owner[a] != owner[b]; };
    const bool v_top = boundary(kTL, kTR);
    const bool v_bottom = boundary(kBL, kBR);
    const bool h_left = boundary(kTL, kBL);
    const bool h_right = boundary(kTR, kBR);

    const float vx = area.x + area.w * std::clamp(split.x, kMinSplit, kMaxSplit);
    const float hy = area.y + area.h * std::clamp(split.y, kMinSplit, kMaxSplit);

    QuadrantDividers out;
    if (v_top || v_bottom) {
        const float y0 = v_top ? area.y : hy;
        const float y1 = v_bottom ? area.bottom() : hy;
        out.vertical = {vx - grab * 0.5f, y0, grab, y1 - y0};
    }
    if (h_left || h_right) {
        const float x0 = h_left ? area.x : vx;
        const float x1 = h_right ? area.right() : vx;
        out.horizontal = {x0, hy - grab * 0.5f, x1 - x0, grab};
    }
    return out;
}

Slot nearest_slot(const Box& area, glm::vec2 p, QuadrantSplit split) {
    const std::array<Box, 4> q = base_quadrants(area, split);
    int best = kTL;
    float best_d2 = std::numeric_limits<float>::max();
    for (int i = 0; i < 4; ++i) {
        const glm::vec2 d = p - q[i].center();
        const float d2 = d.x * d.x + d.y * d.y;
        if (d2 < best_d2) { best_d2 = d2; best = i; }
    }
    return static_cast<Slot>(best);
}

}  // namespace toy::ui
