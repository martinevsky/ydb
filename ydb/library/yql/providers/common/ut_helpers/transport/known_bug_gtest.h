#pragma once

// GTEST flavour of the known-bug guard (see known_bug.h). Include this header only from GTEST
// targets: it needs gtest, which the transport harness library itself does not depend on.

#include "known_bug.h"

#include <library/cpp/testing/gtest/gtest.h>

#define YDB_SKIP_KNOWN_BUG_GTEST(bugId)                             \
    do {                                                            \
        if (::NYql::NTransportTest::SkipKnownBug(bugId)) {          \
            GTEST_SKIP() << "known bug " << (bugId);                \
        }                                                           \
    } while (false)
