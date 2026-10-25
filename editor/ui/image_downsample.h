/**
 * @file image_downsample.h
 * @brief Shrinking an RGBA8 image on the CPU, for the editor's thumbnails.
 *
 * The UI pass binds its own sampler with no mip range (uicoopa's UiPass), so a texture drawn
 * small in the UI is point-ish sampled from its full-size image and crawls with aliasing. The
 * fix is to shrink it before upload, which also keeps a folder of 4K textures from filling
 * video memory with pictures nobody can see at 64 pixels.
 */

#ifndef TOYEDITOR_UI_IMAGE_DOWNSAMPLE_H
#define TOYEDITOR_UI_IMAGE_DOWNSAMPLE_H

#include <cstdint>
#include <vector>

namespace toy::editor::ui {

/** @brief An RGBA8 image: `pixels` is `width * height * 4` bytes, row-major from the top. */
struct Rgba8Image {
    std::vector<uint8_t> pixels;
    int width = 0;
    int height = 0;

    bool empty() const { return width <= 0 || height <= 0 || pixels.empty(); }
};

/**
 * @brief Box-filters `src` down so neither side exceeds `max_side`, keeping its aspect ratio.
 *
 * Every destination pixel averages the source pixels it covers, so nothing is dropped -- a
 * one-pixel grid line survives as a faint one rather than disappearing between taps, which is
 * what nearest-neighbour sampling would do. An image already small enough is returned as is.
 *
 * @param src    Tightly packed RGBA8, `width * height * 4` bytes.
 * @param width  Source width; 0 or less yields an empty image.
 * @param height Source height.
 * @param max_side Longest side the result may have; 0 or less yields an empty image.
 */
Rgba8Image downsample_rgba8(const uint8_t* src, int width, int height, int max_side);

} // namespace toy::editor::ui

#endif // TOYEDITOR_UI_IMAGE_DOWNSAMPLE_H
