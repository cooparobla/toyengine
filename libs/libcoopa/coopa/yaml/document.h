/**
 * @file document.h
 * @brief One entry point for reading YAML documents from disk, plain or encoded.
 *
 * Every loader in the coopa libraries reads its YAML through read_text() /
 * load_document() instead of opening an ifstream and calling
 * fkyaml::node::deserialize() itself. That gives one seam where encoded
 * containers (e.g. caml's compressed + encrypted ".caml" files) are recognised
 * and decoded, without libcoopa depending on the decoder:
 *
 *   - An application registers a BinaryDecoder for a 4-byte magic + extension
 *     (toyengine does this for caml in toyengine/core/caml_codec.h).
 *   - read_text() sniffs the first four bytes of every file. A registered magic
 *     is decoded whatever the file is called; anything else is returned as text.
 *   - A file carrying a registered extension (".caml") whose decoder was never
 *     installed fails with a message naming the missing registration, rather
 *     than fkYAML choking on binary.
 *
 * resolve_variant() is the matching path-side rule: a reference written as
 * "x.yaml" finds "x.caml" when only the encoded file exists (and vice versa), so
 * scenes, mesh references and sidecars need no rewriting when assets/ is
 * packaged. coopa::asset::AssetSource::resolve() applies it automatically.
 */

#ifndef COOPA_YAML_DOCUMENT_H
#define COOPA_YAML_DOCUMENT_H

#include <fkYAML/node.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

namespace coopa {
namespace yaml {

/// Turns an encoded file's full contents into YAML text. `path` is for error messages.
using BinaryDecoder = std::function<std::string(const std::vector<uint8_t>& bytes, const std::string& path)>;

namespace detail {

struct DecoderEntry {
    std::array<char, 4> magic{};
    std::string         ext;  ///< lower-case, with the dot, e.g. ".caml"
    BinaryDecoder       decode;
};

struct DecoderRegistry {
    std::shared_mutex          mutex;
    std::vector<DecoderEntry>  entries;
    /// Extensions known to be encoded even before a decoder is registered, so
    /// the "not installed" error is precise.
    std::vector<std::string>   known_encoded_exts{".caml"};
};

inline DecoderRegistry& registry() {
    static DecoderRegistry r;
    return r;
}

inline std::string lower_ext(const std::filesystem::path& p) {
    std::string e = p.extension().string();
    for (char& c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return e;
}

} // namespace detail

/**
 * @brief Registers (or replaces) the decoder for a magic/extension pair.
 * @param magic  First four bytes of every encoded file, e.g. {'C','A','M','L'}.
 * @param ext    Extension encoded files use, with the dot, e.g. ".caml".
 * @param decode Bytes -> YAML text.
 */
inline void register_decoder(std::array<char, 4> magic, std::string ext, BinaryDecoder decode) {
    auto& r = detail::registry();
    std::unique_lock lock(r.mutex);
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (auto& e : r.entries) {
        if (e.magic == magic) {
            e.ext = ext;
            e.decode = std::move(decode);
            return;
        }
    }
    r.entries.push_back({magic, ext, std::move(decode)});
    if (std::find(r.known_encoded_exts.begin(), r.known_encoded_exts.end(), ext) == r.known_encoded_exts.end()) {
        r.known_encoded_exts.push_back(ext);
    }
}

/** @brief Removes every registered decoder (tests). */
inline void clear_decoders() {
    auto& r = detail::registry();
    std::unique_lock lock(r.mutex);
    r.entries.clear();
}

/** @brief True for extensions this module treats as YAML documents (.yaml, .yml, encoded). */
inline bool is_document_ext(const std::filesystem::path& p) {
    const std::string e = detail::lower_ext(p);
    if (e == ".yaml" || e == ".yml") return true;
    auto& r = detail::registry();
    std::shared_lock lock(r.mutex);
    return std::find(r.known_encoded_exts.begin(), r.known_encoded_exts.end(), e) != r.known_encoded_exts.end();
}

/** @brief True if `p` carries an encoded-document extension (e.g. ".caml"). */
inline bool is_encoded_ext(const std::filesystem::path& p) {
    const std::string e = detail::lower_ext(p);
    auto& r = detail::registry();
    std::shared_lock lock(r.mutex);
    return std::find(r.known_encoded_exts.begin(), r.known_encoded_exts.end(), e) != r.known_encoded_exts.end();
}

/**
 * @brief Decodes an in-memory document: registered magic -> decoder, otherwise text.
 * @param bytes Full file contents.
 * @param path  Used for error messages and the extension check only.
 */
inline std::string decode_bytes(const std::vector<uint8_t>& bytes, const std::string& path = "") {
    auto& r = detail::registry();
    if (bytes.size() >= 4) {
        BinaryDecoder decode;
        {
            std::shared_lock lock(r.mutex);
            for (const auto& e : r.entries) {
                if (std::memcmp(bytes.data(), e.magic.data(), 4) == 0) {
                    decode = e.decode;
                    break;
                }
            }
        }
        if (decode) return decode(bytes, path);
    }
    if (!path.empty() && is_encoded_ext(path)) {
        throw std::runtime_error("[coopa::yaml] '" + path + "' is an encoded document but no decoder is "
                                 "registered for it (toyengine: call toy::core::install_caml_codec() at startup)");
    }
    return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

/**
 * @brief Reads a document's YAML text, decoding it if it is an encoded container.
 * @throws std::runtime_error if the file can't be opened or decoding fails.
 */
inline std::string read_text(const std::filesystem::path& path) {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) {
        throw std::runtime_error("[coopa::yaml] Failed to open '" + path.string() + "'");
    }
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    return decode_bytes(bytes, path.string());
}

/**
 * @brief Reads and parses a document (plain or encoded).
 * @throws std::runtime_error on open/decode failure; fkYAML's exception on a parse error.
 */
inline fkyaml::node load_document(const std::filesystem::path& path) {
    return fkyaml::node::deserialize(read_text(path));
}

/** @brief load_document() that returns nullopt when the file can't be opened (still throws on bad content). */
inline std::optional<fkyaml::node> try_load_document(const std::filesystem::path& path) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) return std::nullopt;
    return load_document(path);
}

/**
 * @brief Finds the plain/encoded twin of a document path when the path itself doesn't exist.
 *
 * "dir/x.yaml" -> "dir/x.caml" (and every other registered encoded extension), and
 * "dir/x.caml" -> "dir/x.yaml" / "dir/x.yml". Multi-dot names keep their stem, so
 * "sphere.000.lod.yaml" -> "sphere.000.lod.caml".
 *
 * @return `path` itself if it exists or no twin exists; otherwise the first twin found.
 */
inline std::filesystem::path resolve_variant(const std::filesystem::path& path) {
    std::error_code ec;
    if (std::filesystem::exists(path, ec)) return path;
    if (!is_document_ext(path)) return path;

    std::vector<std::string> candidates;
    if (is_encoded_ext(path)) {
        candidates = {".yaml", ".yml"};
    } else {
        auto& r = detail::registry();
        std::shared_lock lock(r.mutex);
        candidates = r.known_encoded_exts;
    }
    for (const auto& ext : candidates) {
        std::filesystem::path twin = path;
        twin.replace_extension(ext);
        if (std::filesystem::exists(twin, ec)) return twin;
    }
    return path;
}

/** @brief True if `path` or its plain/encoded twin exists. */
inline bool document_exists(const std::filesystem::path& path) {
    std::error_code ec;
    return std::filesystem::exists(resolve_variant(path), ec);
}

} // namespace yaml
} // namespace coopa

#endif // COOPA_YAML_DOCUMENT_H
