/**
 * @file font.h
 * @brief Loads a .ttf file and owns its per-pixel-size baked FontAtlas instances.
 *
 * Also implements word-wrap layout, shared between Text::emit() (which needs
 * per-glyph placement) and layout-sizing callers like ContentSizeFitter (which
 * only need the aggregate size) — both go through layout().
 */

#ifndef UICOOPA_TEXT_FONT_H
#define UICOOPA_TEXT_FONT_H

#include <uicoopa/text/font_atlas.h>
#include <fstream>
#include <map>
#include <string>
#include <vector>
#include <memory>
#include <cstdint>
#include <algorithm>
#include <stdexcept>

namespace coopa {
namespace ui {

/**
 * @struct TextLayout
 * @brief Per-glyph pen placements for one laid-out block of text, plus per-line widths.
 */
struct TextLayout {
    /** @struct GlyphPlacement
     *  @brief One glyph's codepoint, pen x-offset within its line, and line index. */
    struct GlyphPlacement {
        uint32_t codepoint;
        float    pen_x;
        uint32_t line;
    };

    std::vector<GlyphPlacement> glyphs;
    std::vector<float>          line_widths;
    glm::vec2                   size{0.0f}; /**< Bounding box: (max line width, line count * line_height). */
};

/**
 * @brief Greedy word-wrap over a codepoint-advance callback, decoupled from FontAtlas/Vulkan.
 *
 * This is the algorithm Font::layout() uses, factored out so it can be exercised in a
 * headless unit test with a synthetic advance function instead of a real baked font.
 * Does not set TextLayout::size.y (line-height-dependent); callers multiply that in.
 *
 * ASCII only for v1 (each byte of `text` is treated as one codepoint). A word that alone
 * exceeds wrap_width falls back to a character-level break rather than overflowing forever.
 *
 * @param text        Text to lay out; '\\n' forces a line break.
 * @param wrap_width  Wrap column in the same units as advance_of's return value; <= 0 disables wrapping.
 * @param advance_of  Callable: uint32_t codepoint -> float advance width.
 * @return Per-glyph placements and per-line widths; size.x is set, size.y is left at 0.
 */
template<typename AdvanceFn>
TextLayout layout_text(const std::string& text, float wrap_width, AdvanceFn&& advance_of) {
    TextLayout result;

    float  pen_x = 0.0f;
    size_t line = 0;
    size_t word_start_index = 0;
    float  word_start_pen_x = 0.0f;
    bool   in_word = false;

    auto end_line = [&]() {
        result.line_widths.push_back(pen_x);
        ++line;
        pen_x = 0.0f;
    };

    for (size_t i = 0; i < text.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(text[i]);
        if (c == '\n') {
            end_line();
            in_word = false;
            continue;
        }

        float advance = advance_of(static_cast<uint32_t>(c));

        if (c == ' ') {
            in_word = false;
        } else if (!in_word) {
            in_word = true;
            word_start_index = result.glyphs.size();
            word_start_pen_x = pen_x;
        }

        if (wrap_width > 0.0f && pen_x + advance > wrap_width && pen_x > 0.0f) {
            if (in_word && word_start_pen_x > 0.0f) {
                // Move the whole in-progress word to the next line rather than breaking mid-word.
                float shift = -word_start_pen_x;
                for (size_t k = word_start_index; k < result.glyphs.size(); ++k) {
                    result.glyphs[k].line += 1;
                    result.glyphs[k].pen_x += shift;
                }
                result.line_widths.push_back(word_start_pen_x);
                ++line;
                pen_x = pen_x + shift;
                // The word now starts at column 0 of its new line. word_start_index still
                // correctly points at the word's first glyph (only its .line/.pen_x moved,
                // not its array position) — but word_start_pen_x must be reset, or a second
                // wrap within this same still-too-long word would re-shift glyphs that were
                // already moved onto this line, using the pre-move offset.
                word_start_pen_x = 0.0f;
            } else {
                // No earlier break point on this line (a single word longer than wrap_width):
                // fall back to breaking right here. Reset word tracking to the new line so a
                // word needing *multiple* character-level breaks doesn't leave word_start_pen_x
                // stale — otherwise the next wrap check on this same word could believe there's
                // a valid word-move point back on the line we just finalized, and shift glyphs
                // that already belong to a previous, already-closed line.
                end_line();
                if (in_word) {
                    word_start_index = result.glyphs.size();
                    word_start_pen_x = 0.0f;
                }
            }
        }

        result.glyphs.push_back({ static_cast<uint32_t>(c), pen_x, static_cast<uint32_t>(line) });
        pen_x += advance;
    }
    result.line_widths.push_back(pen_x);

    float max_width = 0.0f;
    for (float w : result.line_widths) max_width = std::max(max_width, w);
    result.size = glm::vec2(max_width, 0.0f);
    return result;
}

/**
 * @class Font
 * @brief One loaded .ttf file, lazily baking a FontAtlas per requested pixel size.
 *
 * Usage:
 * @code
 * Font font(device, allocator, cmd_pool, "assets/fonts/Inter-Regular.ttf");
 * TextLayout layout = font.layout("Hello, world!", 24, 200.0f);
 * @endcode
 */
class Font {
public:
    Font(coopa::gfx::core::Device& device,
         coopa::gfx::memory::Allocator& allocator,
         coopa::gfx::command::CommandPool& cmd_pool,
         const std::string& ttf_path)
        : device_(device), allocator_(allocator), cmd_pool_(cmd_pool)
    {
        std::ifstream file(ttf_path, std::ios::binary | std::ios::ate);
        if (!file) {
            throw std::runtime_error("[uicoopa] Font: failed to open '" + ttf_path + "'.");
        }
        std::streamsize size = file.tellg();
        file.seekg(0, std::ios::beg);
        ttf_bytes_.resize(static_cast<size_t>(size));
        if (!file.read(reinterpret_cast<char*>(ttf_bytes_.data()), size)) {
            throw std::runtime_error("[uicoopa] Font: failed to read '" + ttf_path + "'.");
        }
    }

    /**
     * @brief Returns the atlas baked at pixel_height, creating (and uploading) it on first use.
     *
     * @param pixel_height Bake size in pixels.
     * @param oversample   stb pack oversampling -- see FontAtlas's ctor doc. Part of the cache
     *   key, not just a construction detail: a caller drawing at 1:1 wants 2 and a caller
     *   supersampling wants 1, and the same font/size can legitimately be asked for both in one
     *   frame (Font::measure() at the authored size alongside Text::emit()'s scaled bake). The
     *   two agree on metrics regardless -- advances and vertical metrics are oversample-
     *   independent -- so mixing them is safe.
     *
     * Oversampling is HORIZONTAL only. Vertical oversampling would let a glyph sit at a
     * sub-pixel height, but every renderer here snaps the baseline to a pixel, so all it did was
     * give each glyph a half-pixel vertical offset of its own -- blurred differently from its
     * neighbours, so a word like "Empty" looked out of line (its 'p' and 'y' a hair off).
     */
    FontAtlas& atlas_for_size(uint32_t pixel_height, uint32_t oversample = 2) {
        const AtlasKey key{pixel_height, oversample};
        auto it = atlases_.find(key);
        if (it != atlases_.end()) return *it->second;
        auto atlas = std::make_unique<FontAtlas>(device_, allocator_, cmd_pool_,
                                                  ttf_bytes_.data(), ttf_bytes_.size(),
                                                  static_cast<float>(pixel_height),
                                                  oversample, /*v_oversample=*/1);
        FontAtlas& ref = *atlas;
        atlases_[key] = std::move(atlas);
        return ref;
    }

    /**
     * @brief Lays out text at the given pixel size, with greedy word-wrap.
     *
     * ASCII only for v1 (each byte of `text` is treated as one codepoint). A word
     * that alone exceeds wrap_width falls back to a character-level break rather
     * than overflowing indefinitely.
     *
     * @param text        Text to lay out; '\\n' forces a line break.
     * @param pixel_size  Pixel height to bake/reuse a FontAtlas at.
     * @param wrap_width  Wrap column in pixels; <= 0 disables wrapping. In the SAME space as
     *   pixel_size -- a caller laying out at a supersampled size must scale its wrap width to
     *   match, or the text wraps at the wrong column.
     * @param oversample  Forwarded to atlas_for_size(); see its doc.
     * @return Per-glyph placements, per-line widths, and the overall bounding size, all in
     *   pixel_size's space.
     */
    TextLayout layout(const std::string& text, uint32_t pixel_size, float wrap_width = -1.0f,
                      uint32_t oversample = 2) {
        FontAtlas& atlas = atlas_for_size(pixel_size, oversample);
        TextLayout result = layout_text(text, wrap_width, [&](uint32_t codepoint) {
            const GlyphInfo* g = atlas.glyph(codepoint);
            return g ? g->advance : 0.0f;
        });
        result.size.y = atlas.line_height() * static_cast<float>(result.line_widths.size());
        return result;
    }

    /** @brief Convenience wrapper around layout() for callers that only need the bounding size. */
    glm::vec2 measure(const std::string& text, uint32_t pixel_size, float wrap_width = -1.0f) {
        return layout(text, pixel_size, wrap_width).size;
    }

private:
    coopa::gfx::core::Device& device_;
    coopa::gfx::memory::Allocator& allocator_;
    coopa::gfx::command::CommandPool& cmd_pool_;
    std::vector<unsigned char> ttf_bytes_;

    /// Cache key: one atlas per (bake size, oversampling). Nothing evicts -- see Text::emit()'s
    /// size quantization, which is what bounds how many distinct sizes can ever be asked for.
    struct AtlasKey {
        uint32_t pixel_height;
        uint32_t oversample;
        bool operator<(const AtlasKey& o) const {
            return pixel_height != o.pixel_height ? pixel_height < o.pixel_height
                                                  : oversample < o.oversample;
        }
    };
    std::map<AtlasKey, std::unique_ptr<FontAtlas>> atlases_;
};

}  // namespace ui
}  // namespace coopa

#endif  // UICOOPA_TEXT_FONT_H
