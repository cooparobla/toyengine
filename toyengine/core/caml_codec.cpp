#include <toyengine/core/caml_codec.h>

#include <caml/caml.h>
#include <coopa/yaml/document.h>

#include <cstdlib>
#include <fstream>
#include <stdexcept>

namespace toy {
namespace core {
namespace detail {

CamlKeyState& caml_key_state() {
    static CamlKeyState s;
    return s;
}

std::string default_caml_passphrase() {
#ifdef TOY_CAML_KEY_BAKED
    if (const char* baked = TOY_CAML_KEY_BAKED; baked && *baked) return std::string(baked);
#endif
#ifndef TOY_SHIPPING
    const char* v = std::getenv("TOY_CAML_KEY");
    if (v && *v) return std::string(v);
#endif
    return std::string(caml::DEFAULT_PASSPHRASE);
}

std::vector<uint8_t> caml_key(const std::string& passphrase) {
    auto& s = caml_key_state();
    std::lock_guard lock(s.mutex);
    if (s.key.empty() || s.passphrase != passphrase) {
        s.key = caml::derive_key(passphrase);
        s.passphrase = passphrase;
    }
    return s.key;
}

} // namespace detail
} // namespace core
} // namespace toy

namespace toy {
namespace core {

void install_caml_codec(std::string passphrase) {
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

std::vector<uint8_t> encode_caml_text(const std::string& yaml_text, const std::string& passphrase) {
    const std::string p = passphrase.empty() ? detail::default_caml_passphrase() : passphrase;
    return caml::encode(yaml_text, detail::caml_key(p));
}

void encode_caml_file(const std::filesystem::path& src, const std::filesystem::path& dst,
                             const std::string& passphrase) {
    const std::vector<uint8_t> bytes = encode_caml_text(coopa::yaml::read_text(src), passphrase);
    std::ofstream out(dst, std::ios::binary | std::ios::trunc);
    if (!out) throw std::runtime_error("[caml] Failed to open '" + dst.string() + "' for writing");
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!out) throw std::runtime_error("[caml] Failed to write '" + dst.string() + "'");
}

} // namespace core
} // namespace toy
