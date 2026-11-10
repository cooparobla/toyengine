/**
 * @file file_util_test.cpp
 * @brief FileUtil root/asset path resolution against the configured root directory
 *        (configuration/root_directory.h), which sfxcoopa's tests and demos resolve fixtures
 *        through.
 *
 * Not covered: StringUtil, MathUtil/Mat4 and ParallelVector have no consumers in any coopa repo;
 * ParallelQueue is exercised by every JobEngine test (it backs the global queues).
 */
#include <coopa/testing/test.h>

#include <coopa/util/file.h>

COOPA_TEST_SUITE("file_util");

COOPA_TEST(root_path_resolves_and_missing_asset_reports_absent) {
    // The configured root must point at a real checkout -- every get_root_path()/
    // get_asset_path() caller depends on it.
    EXPECT_TRUE(FileUtil::does_path_exist(FileUtil::get_root_path("CMakeLists.txt")));
    auto [path, exist] = FileUtil::get_asset_path("non_existent_file_example");
    EXPECT_FALSE(exist);
}
