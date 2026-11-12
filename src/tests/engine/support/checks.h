#pragma once

/**
 * @file checks.h
 * @brief The check vocabulary every engine suite is written in: coopa/testing/test.h's sentence
 *        checks, brought into scope by their short names.
 *
 * The engine's tests read as `expect(condition, "what this proves")` -- a sentence per claim,
 * printed on failure -- rather than as bare EXPECT_TRUE macros, so the reason a check exists
 * travels with it into the failure output. This header is the one place that vocabulary is
 * imported, so no test file repeats the using-declarations.
 */

#include <coopa/testing/test.h>

#include <root_directory.h>   // ROOT_DIR (this checkout), PROJ_DIR (its libs/)

using coopa::test::expect;
using coopa::test::expect_at_least;
using coopa::test::expect_near;
using coopa::test::require;
using coopa::test::ScopedEnv;
