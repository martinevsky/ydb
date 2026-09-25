#pragma once

// Guarded waits shared by the transport harness. A guard is a failure deadline, never a pass condition:
// when it is hit the helper throws yexception, which fails the calling test in UNITTEST and GTEST alike.

#include <util/datetime/base.h>
#include <util/generic/strbuf.h>

#include <functional>

namespace NYql::NTransportTest {

// Polls `predicate` until it returns true. Between polls the thread pauses for about 1 ms on a
// condition variable. Throws yexception mentioning `what` when `guard` expires.
// Use it only as a sync point (e.g. "the curl thread has updated a gauge"), never as the oracle.
void WaitUntil(const std::function<bool()>& predicate, TDuration guard, TStringBuf what);

} // namespace NYql::NTransportTest
