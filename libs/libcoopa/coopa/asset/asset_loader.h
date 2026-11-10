/**
 * @file asset_loader.h
 * @brief Two-stage loader interface: an off-thread decode(), a main-thread finalize().
 */

#ifndef COOPA_ASSET_ASSET_LOADER_H
#define COOPA_ASSET_ASSET_LOADER_H

#include <coopa/asset/asset_id.h>

#include <memory>
#include <string>
#include <utility>

namespace coopa {
namespace asset {

class AssetSource;

/**
 * @struct LoadContext
 * @brief Everything a loader needs beyond the raw bytes to decode/finalize an asset.
 */
struct LoadContext {
    const AssetSource* source = nullptr; /**< Search-root resolver, for loading sibling files (e.g. a material's textures). */
    std::string resolved_path;           /**< Concrete filesystem path this asset was resolved to. */
};

/**
 * @class IAssetLoader
 * @brief Type-erased two-stage loader: an off-thread decode(), a main-thread finalize().
 *
 * decode() must be safe to call from a worker thread: read bytes, parse,
 * decode pixels/geometry/audio — CPU work only, never touch a GPU handle.
 * finalize() always runs on the thread driving AssetManager::update() (or,
 * for a synchronous AssetManager::load(), on the calling thread immediately
 * after decode()) and is where GPU upload / Vulkan object creation belongs.
 *
 * Prefer subclassing TypedAssetLoader<T, Intermediate> over this directly —
 * it removes the std::shared_ptr<void> casts from loader implementations.
 */
class IAssetLoader {
public:
    virtual ~IAssetLoader() = default;

    /**
     * @brief Reads and decodes raw bytes into an intermediate, GPU-free representation.
     *
     * May run on a worker thread — must not touch a Device, Allocator, or
     * any other GPU-owning object.
     *
     * @param id  Identity being loaded.
     * @param ctx Resolved path and source, for reading the file (and any sibling files it references).
     * @return Type-erased intermediate payload. Throw std::runtime_error (or similar) on failure.
     */
    virtual std::shared_ptr<void> decode(const AssetId& id, const LoadContext& ctx) = 0;

    /**
     * @brief Finalizes a decoded intermediate into the published asset payload.
     *
     * Always runs on the thread driving AssetManager::update() — safe to
     * touch the GPU here.
     *
     * @param decoded Value returned by decode().
     * @param id      Identity being loaded.
     * @param ctx     Resolved path and source.
     * @return Type-erased final payload, published into the AssetSlot.
     */
    virtual std::shared_ptr<void> finalize(std::shared_ptr<void> decoded, const AssetId& id, const LoadContext& ctx) = 0;

    /** @brief Human-readable type name, for logging and error messages. */
    virtual const char* type_name() const = 0;
};

/**
 * @class TypedAssetLoader
 * @brief Typed adapter over IAssetLoader that removes std::shared_ptr<void> casts from implementations.
 *
 * @tparam T            Final published asset type (e.g. gfx::data::Mesh).
 * @tparam Intermediate CPU-only decoded representation passed from decode_typed() to
 *                      finalize_typed(). Defaults to T for loaders with nothing to
 *                      upload (e.g. plain data assets with no GPU-side finalize step).
 *
 * @code
 * class TextureLoader : public coopa::asset::TypedAssetLoader<Texture, DecodedImage> {
 * public:
 *     std::shared_ptr<DecodedImage> decode_typed(const coopa::asset::AssetId&,
 *                                                 const coopa::asset::LoadContext&) override { ... }
 *     std::shared_ptr<Texture> finalize_typed(std::shared_ptr<DecodedImage>,
 *                                              const coopa::asset::AssetId&,
 *                                              const coopa::asset::LoadContext&) override { ... }
 *     const char* type_name() const override { return "Texture"; }
 * };
 * @endcode
 */
template <typename T, typename Intermediate = T>
class TypedAssetLoader : public IAssetLoader {
public:
    using AssetType        = T;
    using IntermediateType = Intermediate;

    /** @copydoc IAssetLoader::decode */
    virtual std::shared_ptr<Intermediate> decode_typed(const AssetId& id, const LoadContext& ctx) = 0;

    /** @copydoc IAssetLoader::finalize */
    virtual std::shared_ptr<T> finalize_typed(std::shared_ptr<Intermediate> decoded, const AssetId& id, const LoadContext& ctx) = 0;

    std::shared_ptr<void> decode(const AssetId& id, const LoadContext& ctx) override {
        return decode_typed(id, ctx);
    }

    std::shared_ptr<void> finalize(std::shared_ptr<void> decoded, const AssetId& id, const LoadContext& ctx) override {
        return finalize_typed(std::static_pointer_cast<Intermediate>(std::move(decoded)), id, ctx);
    }
};

} // namespace asset
} // namespace coopa

#endif // COOPA_ASSET_ASSET_LOADER_H
