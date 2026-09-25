#pragma once

// Known-bug guard for contract tests that fail on today's code.
//
// A contract test that is red on the base commit starts with
//     YDB_SKIP_KNOWN_BUG("F-A-1");
// It is skipped (the body returns early and the skip is printed to Cerr) unless the environment
// variable YDB_TRANSPORT_RUN_KNOWN_BUGS is "1":
//     ./ya make -tA <target> -F 'Suite::Test' --test-env=YDB_TRANSPORT_RUN_KNOWN_BUGS=1
// Remove the guard in the same PR that fixes the bug.
//
// UNITTEST: use YDB_SKIP_KNOWN_BUG in a Y_UNIT_TEST body.
// GTEST: include known_bug_gtest.h and use YDB_SKIP_KNOWN_BUG_GTEST (it wraps GTEST_SKIP()).

#include <util/generic/strbuf.h>
#include <util/stream/output.h>
#include <util/system/env.h>

namespace NYql::NTransportTest {

inline bool RunKnownBugs() {
    return GetEnv("YDB_TRANSPORT_RUN_KNOWN_BUGS") == "1";
}

inline bool SkipKnownBug(TStringBuf bugId) {
    if (RunKnownBugs()) {
        Cerr << "RUN known bug " << bugId << " (YDB_TRANSPORT_RUN_KNOWN_BUGS=1)" << Endl;
        return false;
    }
    Cerr << "SKIP known bug " << bugId << Endl;
    return true;
}

} // namespace NYql::NTransportTest

#define YDB_SKIP_KNOWN_BUG(bugId)                                   \
    do {                                                            \
        if (::NYql::NTransportTest::SkipKnownBug(bugId)) {          \
            return;                                                 \
        }                                                           \
    } while (false)
