#pragma once

// H6: read-only access to the gateway's dynamic counters by label path.
//
// Path syntax: "<label>=<value>/.../<counter name>", e.g. "method=GET/code=200/count" or "InFlight".
// Lookups never create subgroups or counters (FindSubgroup/FindCounter only).

#include <library/cpp/monlib/dynamic_counters/counters.h>

#include <util/datetime/base.h>
#include <util/generic/map.h>
#include <util/generic/maybe.h>
#include <util/generic/string.h>

namespace NYql::NTransportTest {

class TCountersInspector {
public:
    using TSnapshot = TMap<TString, i64>;

    explicit TCountersInspector(::NMonitoring::TDynamicCounterPtr counters);

    // Value of the counter at `path`, or Nothing() if the counter or any subgroup on the path is absent.
    TMaybe<i64> Find(TStringBuf path) const;
    // Value of the counter at `path`, 0 if absent.
    i64 Get(TStringBuf path) const;
    bool Has(TStringBuf path) const;

    // Every counter under the root, keyed by its path (same syntax as Get).
    TSnapshot Snapshot() const;
    // Entries whose value differs between `before` and `after` (absent = 0), as after - before.
    static TSnapshot Diff(const TSnapshot& before, const TSnapshot& after);
    static TString ToString(const TSnapshot& snapshot);

    // Sync point: polls until `Get(path) == expected`; throws on guard expiry.
    void WaitValue(TStringBuf path, i64 expected, TDuration guard = TDuration::Seconds(5)) const;

    // T-OBS-3: InFlight, InFlightStreams, AllocatedMemory and AwaitQueue all reach 0 within `guard`.
    // Throws yexception with the offending values otherwise.
    void AssertSettled(TDuration guard = TDuration::Seconds(5)) const;

    const ::NMonitoring::TDynamicCounterPtr& Root() const {
        return Counters_;
    }

private:
    ::NMonitoring::TDynamicCounterPtr Counters_;
};

} // namespace NYql::NTransportTest
