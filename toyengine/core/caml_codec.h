/**
 * @file caml_codec.h
 * @brief Plugs caml's compressed + encrypted ".caml" documents into coopa::yaml.
 *
 * libcoopa reads every YAML document through coopa::yaml::read_text(), which
 * decodes any container whose magic has a registered decoder. This header is
 * the one place toyengine includes caml.h (and so the one place that needs
 * OpenSSL and zstd): install_caml_codec() registers the "CAML" magic, after
 * which config, scenes, meshes, LOD sidecars, physics materials, themes, maps
 * and every other YAML asset load identically from .yaml or .caml.
 *
 * Key: TOY_CAML_KEY env var if set, else caml::DEFAULT_PASSPHRASE. The
 * passphrase -> key PBKDF2 step (10k SHA-256 rounds) runs once here and is
 * cached; caml::decode_file() would redo it per file. The key is baked into
 * every shipped binary, so .caml is packaging/obfuscation, not secrecy.
 */

#ifndef TOYENGINE_CORE_CAML_CODEC_H
#define TOYENGINE_CORE_CAML_CODEC_H

#include <caml/caml.h>
#include <coopa/yaml/document.h>

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

namespace toy {
namespace core {

namespace detail {

struct CamlKeyState {
    std::mutex            mutex;
    std::vector<uint8_t>  key;
    std::string           passphrase;
};

inline CamlKeyState& caml_key_state() {
    static CamlKeyState s;
    return s;
}

/**
 * @brief The passphrase a game decodes with: one compiled in at build time
 *        (-DTOY_CAML_KEY_BAKED=..., what a shipping build with a custom key uses, since a
 *        player's machine has no TOY_CAML_KEY), else TOY_CAML_KEY (never read in a shipping
 *        build), else caml's default.
 */
inline std::string default_caml_passphrase() {
#ifdef TOY_CAML_KEY_BAKED
    if (const char* baked = TOY_CAML_KEY_BAKED; baked && *baked) return std::string(baked);
#endif
#ifndef TOY_SHIPPING
    const char* v = std::getenv("TOY_CAML_KEY");
    if (v && *v) return std::string(v);
#endif
    return std::string(caml::DEFAULT_PASSPHRASE);
}

/** @brief The cached 32-byte key for `passphrase`, derived on first use or when it changes. */
inline std::vector<uint8_t> caml_key(const std::string& passphrase) {
    auto& s = caml_key_state();
    std::lock_guard lock(s.mutex);
    if (s.key.empty() || s.passphrase != passphrase) {
        s.key = caml::derive_key(passphrase);
        s.passphrase = passphrase;
    }
    return s.key;
}

} // namespace detail

/**
 * @brief Registers the caml decoder with coopa::yaml. Idempotent; call before loading any config.
 * @param passphrase Empty selects TOY_CAML_KEY / caml::DEFAULT_PASSPHRASE.
 */
inline void install_caml_codec(std::string passphrase = "") {
    if (passphrase.empty()) passphrase = detail::default_caml_passphrase();
    const std::vector<uint8_t> key = detail::caml_key(passphrase);
    coopa::yaml::register_decoder(
        {caml::MAGIC[0], caml::MAGIC[1], caml::MAGIC[2], caml::MAGIC[3]}, ".caml",
        [key](const std::vector<uint8_t>& bytes, const std::string& path) {
            try {
                return caml::decode(bytes, key);
            } catch (const std::exception& e) {
                throw std::runtime_error("[caml] Failed to decode '" + path + "': " + e.what());
            }
        });
}

/**
 * @brief Encodes YAML text to .caml bytes with the cached key.
 * @param passphrase Empty selects TOY_CAML_KEY / caml::DEFAULT_PASSPHRASE.
 */
inline std::vector<uint8_t> encode_caml_text(const std::string& yaml_text, const std::string& passphrase = "") {
    const std::string p = passphrase.empty() ? detail::default_caml_passphrase() : passphrase;
    return caml::encode(yaml_text, detail::caml_key(p));
}

/**
 * @brief Encodes a YAML (or already-.caml) file to a .caml file.
 * @param src  Source document; decoded first if it is already encoded.
 * @param dst  Destination path, written whole (parent directory must exist).
 */
inline void encode_caml_file(const std::filesystem::path& src, const std::filesystem::path& dst,
                             const std::string& passphrase = "") {
    const std::vector<uint8_t> bytes = encode_caml_text(coopa::yaml::read_text(src), passphrase);
    std::ofstream out(dst, std::ios::binary | std::ios::trunc);
    if (!out) throw std::runtime_error("[caml] Failed to open '" + dst.string() + "' for writing");
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!out) throw std::runtime_error("[caml] Failed to write '" + dst.string() + "'");
}

} // namespace core
} // namespace toy

#endif // TOYENGINE_CORE_CAML_CODEC_H
