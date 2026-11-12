#pragma once

/**
 * @file scratch_files.h
 * @brief Writing test inputs into a test's scratch directory (coopa::test::scratch_dir()):
 *        a text file, and a scene tree re-encoded as .caml the way Build > Package lays it out.
 */

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <coopa/testing/test.h>

#include <toyengine/io/caml_codec.h>

namespace toy::test {

/** @brief Writes `text` to `path`, replacing it. */
inline void write_text_file(const std::filesystem::path& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
}

/**
 * @brief Writes `content` to `<scratch_dir>/<relative_path>`, creating parents; returns the path.
 *
 * Loader fixtures live on disk rather than in strings so that relative paths -- `inherit_from`, a
 * clip a scene references, an asset path inside an inherited prefab -- resolve exactly as they do
 * for real files, including across directories.
 */
inline std::string write_scratch_file(const std::string& relative_path, const std::string& content) {
    const std::filesystem::path path = coopa::test::scratch_dir() / relative_path;
    std::filesystem::create_directories(path.parent_path());
    write_text_file(path, content);
    return path.string();
}

/**
 * @brief Copies `src` to `dst` recursively, then replaces every YAML document in `dst` with its
 *        .caml encoding -- the layout Build > Package produces.
 */
inline void package_tree_as_caml(const std::filesystem::path& src, const std::filesystem::path& dst) {
    std::filesystem::remove_all(dst);
    std::filesystem::copy(src, dst, std::filesystem::copy_options::recursive);
    std::vector<std::filesystem::path> docs;
    for (const auto& e : std::filesystem::recursive_directory_iterator(dst)) {
        const std::string ext = e.path().extension().string();
        if (e.is_regular_file() && (ext == ".yaml" || ext == ".yml")) docs.push_back(e.path());
    }
    for (const auto& p : docs) {
        std::filesystem::path out = p;
        out.replace_extension(".caml");
        toy::io::encode_caml_file(p, out);
        std::filesystem::remove(p);
    }
}

} // namespace toy::test
