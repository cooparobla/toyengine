/**
 * @file vertex_layout.h
 * @brief Vertex input layout description, replacing hand-built
 * VkVertexInputBindingDescription/VkVertexInputAttributeDescription arrays.
 */

#ifndef COOPA_GFX_TYPES_VERTEX_LAYOUT_H
#define COOPA_GFX_TYPES_VERTEX_LAYOUT_H

#include <cstdint>
#include <vector>

#include <gfxcoopa/types/enums.h>
#include <gfxcoopa/types/format.h>

namespace coopa {
namespace gfx {

/// @brief One vertex buffer binding slot: its stride and step rate.
struct VertexBinding {
    uint32_t   binding = 0;
    uint32_t   stride  = 0;
    VertexRate rate    = VertexRate::Vertex;
};

/// @brief One shader input attribute: its location, source binding, format,
/// and byte offset within that binding's stride.
struct VertexAttribute {
    uint32_t location = 0;
    uint32_t binding  = 0;
    Format   format   = Format::Undefined;
    uint32_t offset   = 0;
};

/**
 * @struct VertexLayout
 * @brief A pipeline's full vertex input description, built fluently.
 *
 * A vertex type declares its canonical layout as a single
 * `static VertexLayout layout()`, rather than the separate binding- and
 * attribute-description functions Vulkan's structs would otherwise need:
 *
 * @code
 * static VertexLayout layout() {
 *     return VertexLayout{}
 *         .binding(0, sizeof(MyVertex))
 *         .attribute(0, Format::RG32_Sfloat,   offsetof(MyVertex, x))
 *         .attribute(1, Format::RG32_Sfloat,   offsetof(MyVertex, u))
 *         .attribute(2, Format::RGBA8_Unorm,   offsetof(MyVertex, color));
 * }
 * @endcode
 *
 * attribute() attaches to the most recently added binding, so per-buffer
 * grouping falls out of call order without repeating the binding index.
 */
struct VertexLayout {
    std::vector<VertexBinding>   bindings;
    std::vector<VertexAttribute> attributes;

    /// @brief Adds a new vertex buffer binding slot.
    /// @param binding Binding index (matches CommandBuffer::bind_vertex_buffer's slot).
    /// @param stride Byte stride between consecutive elements.
    /// @param rate Per-vertex or per-instance step rate.
    /// @return *this, for chaining.
    VertexLayout& binding(uint32_t binding, uint32_t stride, VertexRate rate = VertexRate::Vertex) {
        bindings.push_back(VertexBinding{binding, stride, rate});
        return *this;
    }

    /// @brief Adds an attribute, attached to the most recently added binding.
    /// @param location Shader input location.
    /// @param format Attribute format (e.g. Format::RGB32_Sfloat for a vec3).
    /// @param offset Byte offset within the binding's stride.
    /// @return *this, for chaining.
    VertexLayout& attribute(uint32_t location, Format format, uint32_t offset) {
        uint32_t binding_index = bindings.empty() ? 0 : bindings.back().binding;
        attributes.push_back(VertexAttribute{location, binding_index, format, offset});
        return *this;
    }

    /// @brief An empty layout, for vertex-less fullscreen-triangle passes
    /// whose vertices are generated entirely in the vertex shader.
    static VertexLayout none() { return VertexLayout{}; }

    /// @brief Appends another layout's bindings/attributes onto this one --
    /// e.g. `Vertex::layout().append(InstanceData::layout())` for a pipeline
    /// that reads a per-vertex stream at binding 0 and a per-instance stream
    /// at binding 1. Binding/location indices are taken as-is from `other`,
    /// not renumbered, so the two layouts must already use disjoint indices.
    /// @return *this, for chaining.
    VertexLayout& append(const VertexLayout& other) {
        bindings.insert(bindings.end(), other.bindings.begin(), other.bindings.end());
        attributes.insert(attributes.end(), other.attributes.begin(), other.attributes.end());
        return *this;
    }
};

} // namespace gfx
} // namespace coopa

#endif // COOPA_GFX_TYPES_VERTEX_LAYOUT_H
