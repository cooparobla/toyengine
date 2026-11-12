/**
 * @file image_desc.h
 * @brief What an image IS -- shape, format, every role it may be used in -- and what a view
 *        over part of it selects.
 *
 * memory::Image's older constructors take a width, a height and a mip count, which describes
 * the single-layer 2D image that is most of what a renderer allocates. An ImageDesc describes
 * the rest as well: a 2D array (a terrain's layer atlas), a 3D volume (a cloud noise field), a
 * cube (a reflection probe). One struct rather than six more constructor parameters, because
 * the fields are not independent -- `depth` means something only to a 3D image, `layers` only
 * to an array or a cube -- and a named factory can say which combination it is building.
 *
 * An ImageViewDesc is the other half: an image may need more than one view over it, and they
 * differ in which mips and layers they select. A compute mip chain is the clearest case -- the
 * downsample writes level N through a single-mip storage view while the shader samples every
 * level through one view over the whole chain.
 */

#ifndef COOPA_GFX_TYPES_IMAGE_DESC_H
#define COOPA_GFX_TYPES_IMAGE_DESC_H

#include <cstdint>

#include <gfxcoopa/types/barrier.h>
#include <gfxcoopa/types/enums.h>
#include <gfxcoopa/types/format.h>

namespace coopa {
namespace gfx {

/**
 * @enum ImageKind
 * @brief An image's dimensionality, and for a view, how it reads one.
 *
 * An image and a view over it do not have to agree: a 2D-array image can be viewed as a single
 * 2D layer, and a cube is an array of six underneath. The pairs Vulkan rejects (a 3D image
 * viewed as an array, say) are rejected by validation rather than by this type.
 */
enum class ImageKind : uint8_t {
    Image2D,       ///< One 2D layer. The default, and most render targets.
    Image2DArray,  ///< `layers` 2D slices, indexed in the shader.
    Image3D,       ///< A `width x height x depth` volume, sampled trilinearly.
    Cube,          ///< Six 2D faces (`layers` must be 6, or 6*N for a cube array).
};

/**
 * @struct ImageDesc
 * @brief Everything memory::Image needs to allocate an image of any shape.
 *
 * Fields that do not apply to the chosen `kind` are ignored rather than checked: `depth` is
 * read only for Image3D, `layers` only for Image2DArray and Cube. Use the factories below
 * where one fits -- each sets exactly the fields its shape uses.
 */
struct ImageDesc {
    uint32_t        width = 0;        ///< Level-0 width in texels.
    uint32_t        height = 0;       ///< Level-0 height in texels.
    uint32_t        depth = 1;        ///< Level-0 depth in texels (Image3D only).
    uint32_t        layers = 1;       ///< Array layers / cube faces (Image2DArray, Cube).
    uint32_t        mip_levels = 1;   ///< Mip chain length; 1 means no chain.
    ImageKind       kind = ImageKind::Image2D;
    Format          format = Format::Undefined;
    ImageUsage      usage = ImageUsage::None;        ///< Every role, for the image's whole life.
    MemoryResidency residency = MemoryResidency::GpuOnly;
    SampleCount     samples = SampleCount::X1;

    /** @brief A plain 2D texture or render target. */
    static ImageDesc texture_2d(uint32_t width, uint32_t height, Format format, ImageUsage usage,
                                uint32_t mip_levels = 1) {
        ImageDesc d;
        d.width = width;
        d.height = height;
        d.mip_levels = mip_levels;
        d.format = format;
        d.usage = usage;
        return d;
    }

    /** @brief `layers` square 2D slices of `size` texels -- a layer atlas sampled by index. */
    static ImageDesc array_2d(uint32_t size, uint32_t layers, Format format, ImageUsage usage,
                              uint32_t mip_levels = 1) {
        ImageDesc d = texture_2d(size, size, format, usage, mip_levels);
        d.kind = ImageKind::Image2DArray;
        d.layers = layers;
        return d;
    }

    /** @brief A cubic `size^3` volume -- a noise field sampled in three dimensions. */
    static ImageDesc volume(uint32_t size, Format format, ImageUsage usage,
                            uint32_t mip_levels = 1) {
        ImageDesc d = texture_2d(size, size, format, usage, mip_levels);
        d.kind = ImageKind::Image3D;
        d.depth = size;
        return d;
    }

    /** @brief The mip chain length that reaches 1 texel on every axis this `kind` has. */
    uint32_t full_mip_count() const {
        uint32_t largest = width > height ? width : height;
        if (kind == ImageKind::Image3D && depth > largest) largest = depth;
        uint32_t levels = 1;
        for (uint32_t extent = largest; extent > 1; extent >>= 1) ++levels;
        return levels;
    }
};

/**
 * @struct ImageViewDesc
 * @brief Which part of an image a view selects, and how it reads it.
 *
 * The default is the whole image read as a plain 2D texture, which is what memory::Image's own
 * view is. `format` left Undefined means the image's own.
 */
struct ImageViewDesc {
    ImageKind        kind = ImageKind::Image2D;   ///< How the view reads the image.
    SubresourceRange range{};                     ///< Which mips and layers; default is all.
    ImageAspect      aspect = ImageAspect::Color;
    Format           format = Format::Undefined;  ///< Undefined: the image's own format.

    /** @brief One mip level, read as the image's own kind -- a compute storage view. */
    static ImageViewDesc mip(uint32_t level, ImageKind kind = ImageKind::Image2D) {
        ImageViewDesc d;
        d.kind = kind;
        d.range = SubresourceRange::mip(level);
        return d;
    }

    /** @brief Every mip level, read as `kind` -- what a sampler reads a chain through. */
    static ImageViewDesc chain(ImageKind kind, uint32_t layers = 1) {
        ImageViewDesc d;
        d.kind = kind;
        d.range = SubresourceRange::layers(0, layers);
        return d;
    }
};

/**
 * @struct BufferImageRegion
 * @brief One region of a buffer-to-image copy: where in the buffer, and which subresource.
 *
 * A tightly packed upload of several mips or layers is one copy with many of these, not many
 * copies -- which is what the staging buffer of a layered texture is built to be.
 */
struct BufferImageRegion {
    uint64_t    buffer_offset = 0;   ///< Byte offset of this region's texels in the buffer.
    int32_t     x = 0;               ///< Region origin in the image, in texels.
    int32_t     y = 0;
    int32_t     z = 0;               ///< 0 unless the image is 3D.
    uint32_t    width = 0;           ///< Region extent in texels.
    uint32_t    height = 0;
    uint32_t    depth = 1;           ///< 1 for a 2D image or one slice of an array.
    uint32_t    mip = 0;             ///< Destination mip level.
    uint32_t    layer = 0;           ///< Destination base array layer.
    uint32_t    layer_count = 1;     ///< How many layers this region fills.
    ImageAspect aspect = ImageAspect::Color;
};

} // namespace gfx
} // namespace coopa

#endif // COOPA_GFX_TYPES_IMAGE_DESC_H
