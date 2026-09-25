#pragma once

// H7: TLogCapture, captures the YQL logger (YQL_LOG / YQL_CLOG) into memory at TRACE for all components.
//
// The YQL logger is process-global and reference-counted: only the first YqlLoggerScope installs a
// backend. TLogCapture must therefore be the first (and only) owner while it lives; the constructor
// verifies this with a probe line and throws yexception otherwise. Create it before the code under test
// (e.g. before IHTTPGateway::Make) and do not keep a static YqlLoggerScope in the same test binary.
//
// For actor-runtime targets use runtime.SetLogBackend + SetLogPriority(..., PRI_TRACE) instead.

#include <util/generic/string.h>
#include <util/generic/vector.h>

#include <memory>

namespace NYql::NTransportTest {

class TLogCapture {
public:
    TLogCapture();
    ~TLogCapture();

    TLogCapture(const TLogCapture&) = delete;
    TLogCapture& operator=(const TLogCapture&) = delete;

    // Everything captured so far (probe line excluded).
    TString Text() const;
    TVector<TString> Lines() const;
    bool Contains(TStringBuf substring) const;
    // Number of lines matching the ECMAScript regex `pattern` anywhere in the line (std::regex_search).
    size_t CountLines(const TString& pattern) const;
    // Throws yexception quoting the first offending line if `secret` appears anywhere in the log.
    void AssertNoSecret(TStringBuf secret) const;

private:
    struct TImpl;
    std::unique_ptr<TImpl> Impl_;
};

} // namespace NYql::NTransportTest
