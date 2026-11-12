/**
 * @file sprite_sheet.h
 * @brief A named atlas of Sprites over one Texture, plus its GPU-free descriptor format.
 *
 * Split into GPU-free parts (SpriteSheetEntry/SpriteSheetDesc, parse_sprite_sheet_desc(),
 * pixel_rect_to_uv(), build_sprite_table()) and the GPU-owning SpriteSheet class, the
 * same discipline as layout/rect.h: the interesting math -- pixel-rect-to-UV conversion,
 * descriptor parsing -- is exercised by headless unit tests with no Device/Allocator at
 * all, and SpriteSheetLoader (sprite_sheet_loader.h) is a thin coopa::asset wrapper
 * around it.
 *
 * A sheet with no sidecar descriptor (just a bare image file) is the degenerate one-entry
 * case: whole_image_desc() names a single sprite after the file stem, covering the whole
 * texture -- "load a PNG as a sprite" with zero YAML required.
 */

#ifndef UICOOPA_RENDER_SPRITE_SHEET_H
#define UICOOPA_RENDER_SPRITE_SHEET_H

#include <gfxcoopa/engine/data/texture.h>
#include <uicoopa/layout/rect.h>
#include <uicoopa/render/sprite.h>
#include <fkYAML/node.hpp>
#include <glm/glm.hpp>

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace coopa {
namespace ui {

/**
 * @struct SpriteSheetEntry
 * @brief One named sub-rect of a sheet, in source-image pixels (top-left origin, +Y down --
 *        the convention an image editor / the generator script both use).
 */
struct SpriteSheetEntry {
    std::string name;
    uint32_t    x = 0, y = 0, w = 0, h = 0;
    glm::vec4   border{0.0f}; /**< Nine-slice border in source pixels: (left, bottom, right, top). */
};

/**
 * @struct SpriteSheetDesc
 * @brief GPU-free parsed form of a sheet descriptor (icons.yaml and friends).
 */
struct SpriteSheetDesc {
    std::string                    image;           /**< Image path, relative to the descriptor file's directory. */
    uint32_t                       width = 0;       /**< Declared sheet size; 0 means "trust the decoded PNG". */
    uint32_t                       height = 0;
    std::vector<SpriteSheetEntry>  sprites;
};

/**
 * @brief Parses a sheet descriptor from YAML text. No filesystem access, no GPU --
 *        safe to call from a headless test or a loader's off-thread decode() stage.
 *
 * Expected shape:
 * @code
 * image: icons.png
 * width: 256
 * height: 256
 * sprites:
 *   - { name: arrow_left, x: 0, y: 0, w: 32, h: 32 }
 *   - { name: panel_frame, x: 0, y: 96, w: 48, h: 48,
 *       border: { left: 8, bottom: 8, right: 8, top: 8 } }
 * @endcode
 * @param yaml_text Full contents of the descriptor file.
 * @return The parsed descriptor.
 * @throws std::runtime_error if `image` is missing, an entry has no `name`, or two
 *         entries share a name.
 */
inline SpriteSheetDesc parse_sprite_sheet_desc(const std::string& yaml_text) {
    fkyaml::node root = fkyaml::node::deserialize(yaml_text);

    SpriteSheetDesc desc;
    if (!root.contains("image")) {
        throw std::runtime_error("[uicoopa] parse_sprite_sheet_desc: missing required 'image' key.");
    }
    desc.image = root.at("image").get_value<std::string>();
    if (root.contains("width"))  desc.width  = static_cast<uint32_t>(root.at("width").get_value<int>());
    if (root.contains("height")) desc.height = static_cast<uint32_t>(root.at("height").get_value<int>());

    if (root.contains("sprites") && root.at("sprites").is_sequence()) {
        std::unordered_map<std::string, bool> seen;
        for (const auto& n : root.at("sprites")) {
            if (!n.contains("name")) {
                throw std::runtime_error("[uicoopa] parse_sprite_sheet_desc: a sprite entry is missing 'name'.");
            }
            SpriteSheetEntry e;
            e.name = n.at("name").get_value<std::string>();
            if (seen.count(e.name)) {
                throw std::runtime_error("[uicoopa] parse_sprite_sheet_desc: duplicate sprite name '" + e.name + "'.");
            }
            seen[e.name] = true;
            if (n.contains("x")) e.x = static_cast<uint32_t>(n.at("x").get_value<int>());
            if (n.contains("y")) e.y = static_cast<uint32_t>(n.at("y").get_value<int>());
            if (n.contains("w")) e.w = static_cast<uint32_t>(n.at("w").get_value<int>());
            if (n.contains("h")) e.h = static_cast<uint32_t>(n.at("h").get_value<int>());
            if (n.contains("border")) {
                const auto& b = n.at("border");
                if (b.contains("left"))   e.border.x = b.at("left").get_value<float>();
                if (b.contains("bottom")) e.border.y = b.at("bottom").get_value<float>();
                if (b.contains("right"))  e.border.z = b.at("right").get_value<float>();
                if (b.contains("top"))    e.border.w = b.at("top").get_value<float>();
            }
            desc.sprites.push_back(std::move(e));
        }
    }
    return desc;
}

/**
 * @brief Builds the degenerate one-entry descriptor for a bare image with no sidecar
 *        YAML: a single sprite, named after the file stem, covering the whole texture.
 * @param name Entry name, conventionally the file's stem (no directory, no extension).
 * @param w Image width in pixels.
 * @param h Image height in pixels.
 * @return A single-entry SpriteSheetDesc.
 */
inline SpriteSheetDesc whole_image_desc(const std::string& name, uint32_t w, uint32_t h) {
    SpriteSheetDesc desc;
    desc.width = w;
    desc.height = h;
    SpriteSheetEntry e;
    e.name = name;
    e.x = 0; e.y = 0; e.w = w; e.h = h;
    desc.sprites.push_back(std::move(e));
    return desc;
}

/**
 * @brief Converts an image-space pixel sub-rect into a Sprite UV rect.
 *
 * Image space has a top-left origin with +Y down (how every image editor and the
 * generator script describe a rect); canvas space is bottom-left origin with +Y up, and
 * DrawList::add_quad() pairs pos.min<->uv.min, pos.max<->uv.max directly. So uv.min must
 * be the sub-rect's BOTTOM edge in texture space, which is its higher V coordinate --
 * the result therefore has uv.min.y > uv.max.y, exactly the convention FontAtlas uses
 * (see font_atlas.h's g.uv construction) and required for add_nine_slice() to slice the
 * right way (see draw_list.h's nine_slice_axis_()).
 *
 * @param x,y,w,h Sub-rect in source pixels, top-left origin.
 * @param tex_w,tex_h Full texture size in pixels.
 * @param half_texel_inset Shrinks the rect inward on all four edges by half a texel.
 *        Leave false for a sheet with a transparent gutter between cells (the default
 *        icon sheet): exact texel-boundary UVs keep a pixel-aligned quad a 1:1 texel-to-
 *        pixel mapping, while a half-texel inset would blur it. Pass true only for a
 *        gutter-less sheet where bilinear bleed from a neighboring cell is otherwise
 *        unavoidable.
 * @return UV rect with uv.min.y > uv.max.y.
 */
inline Rect pixel_rect_to_uv(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                             uint32_t tex_w, uint32_t tex_h, bool half_texel_inset = false) {
    float fw = tex_w > 0 ? static_cast<float>(tex_w) : 1.0f;
    float fh = tex_h > 0 ? static_cast<float>(tex_h) : 1.0f;

    float u_min = static_cast<float>(x) / fw;
    float u_max = static_cast<float>(x + w) / fw;
    float v_min = static_cast<float>(y + h) / fh; // bottom edge -> larger v
    float v_max = static_cast<float>(y) / fh;     // top edge    -> smaller v

    if (half_texel_inset) {
        float du = 0.5f / fw;
        float dv = 0.5f / fh;
        u_min += du; u_max -= du;
        v_min -= dv; v_max += dv; // v_min > v_max, so shrinking inward means v_min -= dv, v_max += dv
    }

    return Rect{ glm::vec2(u_min, v_min), glm::vec2(u_max, v_max) };
}

/**
 * @brief Builds the name -> Sprite table for a descriptor. Node-based (std::unordered_map),
 *        so every Sprite stays at a stable address as more entries are looked up or the
 *        map otherwise grows -- callers may safely hand out `&table.at(name)`.
 * @param desc Parsed descriptor.
 * @param texture Texture every Sprite will point at; may be null for headless tests that
 *        only care about the UV/border math.
 * @param tex_w,tex_h Actual decoded texture size (falls back to desc.width/height if a
 *        caller has no decoded image handy, e.g. a headless test).
 * @param half_texel_inset Forwarded to pixel_rect_to_uv().
 * @return name -> Sprite map, one entry per SpriteSheetDesc::sprites element.
 */
inline std::unordered_map<std::string, Sprite> build_sprite_table(
    const SpriteSheetDesc& desc, coopa::gfx::engine::data::Texture* texture,
    uint32_t tex_w, uint32_t tex_h, bool half_texel_inset = false)
{
    std::unordered_map<std::string, Sprite> table;
    table.reserve(desc.sprites.size());
    for (const auto& e : desc.sprites) {
        Sprite s;
        s.texture = texture;
        s.uv = pixel_rect_to_uv(e.x, e.y, e.w, e.h, tex_w, tex_h, half_texel_inset);
        s.border = e.border;
        table.emplace(e.name, s);
    }
    return table;
}

/**
 * @class SpriteSheet
 * @brief An owned atlas Texture plus its pointer-stable named Sprite table.
 *
 * This is the payload type coopa::asset::AssetManager publishes for a SpriteSheet load
 * (see sprite_sheet_loader.h) -- one Texture, one TextureView, one descriptor set, one
 * DrawList batch per sheet regardless of how many icons it contains.
 */
class SpriteSheet {
public:
    /**
     * @param texture Uploaded atlas texture; SpriteSheet takes ownership.
     * @param desc Parsed descriptor whose entries index into `texture`.
     * @param half_texel_inset See pixel_rect_to_uv(); forwarded as-is.
     */
    SpriteSheet(std::unique_ptr<coopa::gfx::engine::data::Texture> texture,
               const SpriteSheetDesc& desc, bool half_texel_inset = false)
        : texture_(std::move(texture))
    {
        uint32_t tex_w = texture_ ? texture_->width()  : desc.width;
        uint32_t tex_h = texture_ ? texture_->height() : desc.height;
        sprites_ = build_sprite_table(desc, texture_.get(), tex_w, tex_h, half_texel_inset);
    }

    /** @brief Looks up a sprite by name. @return The sprite, or nullptr if absent. */
    const Sprite* find(const std::string& name) const {
        auto it = sprites_.find(name);
        return it != sprites_.end() ? &it->second : nullptr;
    }

    const std::unordered_map<std::string, Sprite>& sprites() const { return sprites_; }

    coopa::gfx::engine::data::Texture& texture() const { return *texture_; }

private:
    std::unique_ptr<coopa::gfx::engine::data::Texture> texture_;
    std::unordered_map<std::string, Sprite>             sprites_;
};

}  // namespace ui
}  // namespace coopa

#endif  // UICOOPA_RENDER_SPRITE_SHEET_H
