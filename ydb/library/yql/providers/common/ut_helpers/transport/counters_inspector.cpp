#include "counters_inspector.h"
#include "wait.h"

#include <util/generic/yexception.h>
#include <util/string/builder.h>
#include <util/string/split.h>

namespace NYql::NTransportTest {

namespace {

void Walk(const ::NMonitoring::TDynamicCounters& group, const TString& prefix, TCountersInspector::TSnapshot& out) {
    for (const auto& [id, countable] : group.ReadSnapshot()) {
        if (const auto* subgroup = dynamic_cast<const ::NMonitoring::TDynamicCounters*>(countable.Get())) {
            Walk(*subgroup, TStringBuilder() << prefix << id.LabelName << "=" << id.LabelValue << "/", out);
        } else if (const auto* counter = dynamic_cast<const ::NMonitoring::TCounterForPtr*>(countable.Get())) {
            out[prefix + id.LabelValue] = counter->Val();
        }
    }
}

} // namespace

TCountersInspector::TCountersInspector(::NMonitoring::TDynamicCounterPtr counters)
    : Counters_(std::move(counters))
{
    Y_ENSURE(Counters_, "TCountersInspector needs a counters root");
}

TMaybe<i64> TCountersInspector::Find(TStringBuf path) const {
    TVector<TString> parts = StringSplitter(path).Split('/').SkipEmpty();
    Y_ENSURE(!parts.empty(), "empty counter path");
    ::NMonitoring::TDynamicCounterPtr group = Counters_;
    for (size_t i = 0; i + 1 < parts.size(); ++i) {
        TStringBuf name;
        TStringBuf value;
        Y_ENSURE(TStringBuf(parts[i]).TrySplit('=', name, value), "bad path segment '" << parts[i] << "' in " << path);
        group = group->FindSubgroup(TString(name), TString(value));
        if (!group) {
            return Nothing();
        }
    }
    if (const auto counter = group->FindCounter(parts.back())) {
        return counter->Val();
    }
    return Nothing();
}

i64 TCountersInspector::Get(TStringBuf path) const {
    return Find(path).GetOrElse(0);
}

bool TCountersInspector::Has(TStringBuf path) const {
    return Find(path).Defined();
}

TCountersInspector::TSnapshot TCountersInspector::Snapshot() const {
    TSnapshot out;
    Walk(*Counters_, TString(), out);
    return out;
}

TCountersInspector::TSnapshot TCountersInspector::Diff(const TSnapshot& before, const TSnapshot& after) {
    TSnapshot diff;
    for (const auto& [path, value] : after) {
        const auto it = before.find(path);
        const i64 old = it == before.end() ? 0 : it->second;
        if (old != value) {
            diff[path] = value - old;
        }
    }
    for (const auto& [path, value] : before) {
        if (!after.contains(path) && value != 0) {
            diff[path] = -value;
        }
    }
    return diff;
}

TString TCountersInspector::ToString(const TSnapshot& snapshot) {
    TStringBuilder out;
    for (const auto& [path, value] : snapshot) {
        out << path << "=" << value << "\n";
    }
    return out;
}

void TCountersInspector::WaitValue(TStringBuf path, i64 expected, TDuration guard) const {
    WaitUntil([&]() {
        return Get(path) == expected;
    }, guard, TStringBuilder() << "counter " << path << " == " << expected);
}

void TCountersInspector::AssertSettled(TDuration guard) const {
    static const TStringBuf gauges[] = {"InFlight", "InFlightStreams", "AllocatedMemory", "AwaitQueue"};
    auto settled = [&]() {
        for (const auto gauge : gauges) {
            if (Get(gauge) != 0) {
                return false;
            }
        }
        return true;
    };
    try {
        WaitUntil(settled, guard, "gateway gauges settle to 0");
    } catch (const yexception&) {
        TStringBuilder values;
        for (const auto gauge : gauges) {
            values << " " << gauge << "=" << Get(gauge);
        }
        ythrow yexception() << "AssertSettled failed after " << guard << ":" << values;
    }
}

} // namespace NYql::NTransportTest
