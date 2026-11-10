#pragma once

/**
 * @file scratch_file.h
 * @brief Writes fixture files into the running test's scratch directory.
 *
 * Loader fixtures live on disk (not in strings) so relative paths -- `inherit_from`, a clip a
 * scene references, an asset path inside an inherited prefab -- resolve exactly as they do for
 * real files, including across directories.
 */

#include <coopa/testing/test.h>

#include <filesystem>
#include <fstream>
#include <string>

namespace libcoopa_test {

/** @brief Writes `content` to `<scratch_dir>/<relative_path>` (creating parents); returns the absolute path. */
inline std::string write_scratch_file(const std::string& relative_path, const std::string& content) {
    const std::filesystem::path path = coopa::test::scratch_dir() / relative_path;
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out << content;
    return path.string();
}

} // namespace libcoopa_test
