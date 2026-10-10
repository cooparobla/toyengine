/**
 * @file caml_codec.h
 * @brief Plugs caml's compressed + encrypted ".caml" documents into coopa::yaml.
 *
 * libcoopa reads every YAML document through coopa::yaml::read_text(), which
 * decodes any container whose magic has a registered decoder. caml_codec.cpp is
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

#include <cstdint>
#include <filesystem>
#include <mutex>
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

CamlKeyState& caml_key_state();

/**
 * @brief The passphrase a game decodes with: one compiled in at build time
 *        (-DTOY_CAML_KEY_BAKED=..., what a shipping build with a custom key uses, since a
 *        player's machine has no TOY_CAML_KEY), else TOY_CAML_KEY (never read in a shipping
 *        build), else caml's default.
 */
std::string default_caml_passphrase();

/** @brief The cached 32-byte key for `passphrase`, derived on first use or when it changes. */
std::vector<uint8_t> caml_key(const std::string& passphrase);

} // namespace detail

/**
 * @brief Registers the caml decoder with coopa::yaml. Idempotent; call before loading any config.
 * @param passphrase Empty selects TOY_CAML_KEY / caml::DEFAULT_PASSPHRASE.
 */
void install_caml_codec(std::string passphrase = "");

/**
 * @brief Encodes YAML text to .caml bytes with the cached key.
 * @param passphrase Empty selects TOY_CAML_KEY / caml::DEFAULT_PASSPHRASE.
 */
std::vector<uint8_t> encode_caml_text(const std::string& yaml_text, const std::string& passphrase = "");

/**
 * @brief Encodes a YAML (or already-.caml) file to a .caml file.
 * @param src  Source document; decoded first if it is already encoded.
 * @param dst  Destination path, written whole (parent directory must exist).
 */
void encode_caml_file(const std::filesystem::path& src, const std::filesystem::path& dst,
                             const std::string& passphrase = "");

} // namespace core
} // namespace toy

#endif // TOYENGINE_CORE_CAML_CODEC_H
