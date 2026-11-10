/**
 * @file caml.h
 * @brief Header-only library for .caml (Compressed & Encrypted YAML) file processing.
 */

#ifndef CAML_H
#define CAML_H

#include <string>
#include <vector>
#include <cstdint>
#include <stdexcept>
#include <fstream>
#include <iostream>
#include <sstream>
#include <cstring>
#include <zstd.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <fkYAML/node.hpp>

namespace caml {

// Magic header bytes for .caml format: "CAML"
constexpr char MAGIC[4] = {'C', 'A', 'M', 'L'};
constexpr uint8_t FORMAT_VERSION = 1;
constexpr char DEFAULT_PASSPHRASE[] = "coopa-caml-key";

/**
 * @brief Derives a 32-byte key from a passphrase string using PBKDF2 (SHA-256).
 */
inline std::vector<uint8_t> derive_key(const std::string& passphrase = DEFAULT_PASSPHRASE, const std::vector<uint8_t>& salt = {
    'c', 'a', 'm', 'l', '_', 's', 'a', 'l', 't', '_', '2', '0', '2', '6', '!', '!'
}) {
    std::string active_passphrase = passphrase.empty() ? DEFAULT_PASSPHRASE : passphrase;
    std::vector<uint8_t> key(32);
    if (PKCS5_PBKDF2_HMAC(active_passphrase.c_str(), -1,
                          reinterpret_cast<const unsigned char*>(salt.data()), static_cast<int>(salt.size()),
                          10000, EVP_sha256(), 32, key.data()) != 1) {
        throw std::runtime_error("CAML: PBKDF2 key derivation failed");
    }
    return key;
}

/**
 * @brief Encodes a raw YAML string into binary .caml format (Zstd compression + AES-256-GCM encryption).
 */
inline std::vector<uint8_t> encode(const std::string& yaml_str, const std::vector<uint8_t>& key = {}) {
    std::vector<uint8_t> active_key = key;
    if (active_key.empty()) {
        active_key = derive_key();
    } else if (active_key.size() != 32) {
        throw std::invalid_argument("CAML: Encryption key must be exactly 32 bytes");
    }

    // 1. Zstd Compress
    size_t max_comp_size = ZSTD_compressBound(yaml_str.size());
    std::vector<uint8_t> compressed(max_comp_size);
    size_t comp_size = ZSTD_compress(compressed.data(), max_comp_size, yaml_str.data(), yaml_str.size(), 3);
    if (ZSTD_isError(comp_size)) {
        throw std::runtime_error(std::string("CAML: Zstd compression error: ") + ZSTD_getErrorName(comp_size));
    }
    compressed.resize(comp_size);

    // 2. Generate Nonce
    std::vector<uint8_t> nonce(12);
    if (RAND_bytes(nonce.data(), 12) != 1) {
        throw std::runtime_error("CAML: Failed to generate random IV/nonce");
    }

    // 3. Encrypt AES-256-GCM
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) throw std::runtime_error("CAML: EVP_CIPHER_CTX_new failed");

    if (EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1 ||
        EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, 12, nullptr) != 1 ||
        EVP_EncryptInit_ex(ctx, nullptr, nullptr, active_key.data(), nonce.data()) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        throw std::runtime_error("CAML: AES-GCM init failed");
    }

    // AAD ("CAML")
    int outlen = 0;
    if (EVP_EncryptUpdate(ctx, nullptr, &outlen, reinterpret_cast<const uint8_t*>(MAGIC), 4) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        throw std::runtime_error("CAML: AAD update failed");
    }

    std::vector<uint8_t> ciphertext(compressed.size());
    if (EVP_EncryptUpdate(ctx, ciphertext.data(), &outlen, compressed.data(), static_cast<int>(compressed.size())) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        throw std::runtime_error("CAML: Encryption update failed");
    }
    int ciphertext_len = outlen;

    int final_len = 0;
    if (EVP_EncryptFinal_ex(ctx, ciphertext.data() + outlen, &final_len) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        throw std::runtime_error("CAML: Encryption final failed");
    }
    ciphertext_len += final_len;
    ciphertext.resize(ciphertext_len);

    std::vector<uint8_t> tag(16);
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, 16, tag.data()) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        throw std::runtime_error("CAML: Getting auth tag failed");
    }
    EVP_CIPHER_CTX_free(ctx);

    // 4. Pack Header & Output Buffer (41 bytes header)
    std::vector<uint8_t> result;
    result.reserve(41 + ciphertext.size());

    result.insert(result.end(), MAGIC, MAGIC + 4);
    result.push_back(FORMAT_VERSION);
    result.insert(result.end(), nonce.begin(), nonce.end());
    result.insert(result.end(), tag.begin(), tag.end());

    uint64_t payload_len = static_cast<uint64_t>(ciphertext.size());
    for (int i = 0; i < 8; ++i) {
        result.push_back(static_cast<uint8_t>((payload_len >> (8 * (7 - i))) & 0xFF));
    }
    result.insert(result.end(), ciphertext.begin(), ciphertext.end());

    return result;
}

/**
 * @brief Decodes binary .caml format back into a raw YAML string.
 */
inline std::string decode(const std::vector<uint8_t>& caml_data, const std::vector<uint8_t>& key = {}) {
    if (caml_data.size() < 41) {
        throw std::runtime_error("CAML: Corrupted data file (size < 41 bytes header)");
    }

    std::vector<uint8_t> active_key = key;
    if (active_key.empty()) {
        active_key = derive_key();
    } else if (active_key.size() != 32) {
        throw std::invalid_argument("CAML: Decryption key must be exactly 32 bytes");
    }

    if (std::memcmp(caml_data.data(), MAGIC, 4) != 0) {
        throw std::runtime_error("CAML: Invalid magic header (expected 'CAML')");
    }

    uint8_t version = caml_data[4];
    if (version != FORMAT_VERSION) {
        throw std::runtime_error("CAML: Unsupported format version (" + std::to_string(version) + ")");
    }

    std::vector<uint8_t> nonce(caml_data.begin() + 5, caml_data.begin() + 17);
    std::vector<uint8_t> tag(caml_data.begin() + 17, caml_data.begin() + 33);

    uint64_t payload_len = 0;
    for (int i = 0; i < 8; ++i) {
        payload_len = (payload_len << 8) | caml_data[33 + i];
    }

    if (caml_data.size() < 41 + payload_len) {
        throw std::runtime_error("CAML: File truncated (expected payload size " + std::to_string(payload_len) + ")");
    }

    std::vector<uint8_t> ciphertext(caml_data.begin() + 41, caml_data.begin() + 41 + payload_len);

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) throw std::runtime_error("CAML: EVP_CIPHER_CTX_new failed");

    if (EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1 ||
        EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, 12, nullptr) != 1 ||
        EVP_DecryptInit_ex(ctx, nullptr, nullptr, active_key.data(), nonce.data()) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        throw std::runtime_error("CAML: Decryption init failed");
    }

    int outlen = 0;
    if (EVP_DecryptUpdate(ctx, nullptr, &outlen, reinterpret_cast<const uint8_t*>(MAGIC), 4) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        throw std::runtime_error("CAML: AAD update failed");
    }

    std::vector<uint8_t> decrypted(ciphertext.size());
    if (EVP_DecryptUpdate(ctx, decrypted.data(), &outlen, ciphertext.data(), static_cast<int>(ciphertext.size())) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        throw std::runtime_error("CAML: Decryption update failed");
    }
    int decrypted_len = outlen;

    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, 16, tag.data()) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        throw std::runtime_error("CAML: Setting auth tag failed");
    }

    int final_len = 0;
    if (EVP_DecryptFinal_ex(ctx, decrypted.data() + outlen, &final_len) <= 0) {
        EVP_CIPHER_CTX_free(ctx);
        throw std::runtime_error("CAML: Decryption authentication failed! Bad passphrase or corrupted data.");
    }
    decrypted_len += final_len;
    decrypted.resize(decrypted_len);
    EVP_CIPHER_CTX_free(ctx);

    unsigned long long uncompressed_size = ZSTD_getFrameContentSize(decrypted.data(), decrypted_len);
    if (uncompressed_size == ZSTD_CONTENTSIZE_ERROR || uncompressed_size == ZSTD_CONTENTSIZE_UNKNOWN) {
        uncompressed_size = decrypted_len * 4 + 4096;
    }

    std::string decompressed_str(uncompressed_size, '\0');
    size_t actual_size = ZSTD_decompress(&decompressed_str[0], uncompressed_size, decrypted.data(), decrypted_len);
    if (ZSTD_isError(actual_size)) {
        throw std::runtime_error(std::string("CAML: Zstd decompression error: ") + ZSTD_getErrorName(actual_size));
    }
    decompressed_str.resize(actual_size);

    return decompressed_str;
}

inline void encode_file(const std::string& input_yaml_path, const std::string& output_caml_path, const std::string& passphrase = DEFAULT_PASSPHRASE) {
    std::ifstream in(input_yaml_path, std::ios::binary);
    if (!in.is_open()) {
        throw std::runtime_error("CAML: Could not open input YAML file: " + input_yaml_path);
    }
    std::stringstream ss;
    ss << in.rdbuf();
    in.close();

    std::vector<uint8_t> key = derive_key(passphrase);
    std::vector<uint8_t> caml_bytes = encode(ss.str(), key);

    std::ofstream out(output_caml_path, std::ios::binary);
    if (!out.is_open()) {
        throw std::runtime_error("CAML: Could not open output CAML file: " + output_caml_path);
    }
    out.write(reinterpret_cast<const char*>(caml_bytes.data()), caml_bytes.size());
    out.close();
}

inline void decode_file(const std::string& input_caml_path, const std::string& output_yaml_path, const std::string& passphrase = DEFAULT_PASSPHRASE) {
    std::ifstream in(input_caml_path, std::ios::binary);
    if (!in.is_open()) {
        throw std::runtime_error("CAML: Could not open input CAML file: " + input_caml_path);
    }
    std::vector<uint8_t> caml_bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();

    std::vector<uint8_t> key = derive_key(passphrase);
    std::string yaml_str = decode(caml_bytes, key);

    std::ofstream out(output_yaml_path, std::ios::binary);
    if (!out.is_open()) {
        throw std::runtime_error("CAML: Could not open output YAML file: " + output_yaml_path);
    }
    out << yaml_str;
    out.close();
}

/**
 * @class CAMLMap
 * @brief Header-only wrapper managing YAML data nodes with built-in .caml export/import capabilities.
 */
class CAMLMap {
public:
    CAMLMap() : root_node_(fkyaml::node::mapping()) {}
    CAMLMap(fkyaml::node node) : root_node_(std::move(node)) {}

    static CAMLMap load_yaml(const std::string& filepath) {
        std::ifstream file(filepath);
        if (!file.is_open()) {
            throw std::runtime_error("CAMLMap: Could not open YAML file: " + filepath);
        }
        return CAMLMap(fkyaml::node::deserialize(file));
    }

    static CAMLMap load_caml(const std::string& filepath, const std::string& passphrase = DEFAULT_PASSPHRASE) {
        std::ifstream file(filepath, std::ios::binary);
        if (!file.is_open()) {
            throw std::runtime_error("CAMLMap: Could not open CAML file: " + filepath);
        }
        std::vector<uint8_t> caml_bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        file.close();

        std::vector<uint8_t> key = derive_key(passphrase);
        std::string yaml_str = decode(caml_bytes, key);
        return CAMLMap(fkyaml::node::deserialize(yaml_str));
    }

    void save_yaml(const std::string& filepath) const {
        std::ofstream fout(filepath);
        if (!fout.is_open()) {
            throw std::runtime_error("CAMLMap: Could not open file for writing: " + filepath);
        }
        fout << root_node_;
    }

    void save_caml(const std::string& filepath, const std::string& passphrase = DEFAULT_PASSPHRASE) const {
        std::stringstream ss;
        ss << root_node_;
        std::vector<uint8_t> key = derive_key(passphrase);
        std::vector<uint8_t> caml_bytes = encode(ss.str(), key);

        std::ofstream fout(filepath, std::ios::binary);
        if (!fout.is_open()) {
            throw std::runtime_error("CAMLMap: Could not open file for writing: " + filepath);
        }
        fout.write(reinterpret_cast<const char*>(caml_bytes.data()), caml_bytes.size());
    }

    fkyaml::node& get_raw_node() { return root_node_; }
    const fkyaml::node& get_raw_node() const { return root_node_; }

    std::string to_yaml_string() const {
        std::stringstream ss;
        ss << root_node_;
        return ss.str();
    }

    static CAMLMap from_yaml_string(const std::string& yaml_str) {
        return CAMLMap(fkyaml::node::deserialize(yaml_str));
    }

private:
    fkyaml::node root_node_;
};

} // namespace caml

#endif // CAML_H
