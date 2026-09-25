#include "log_capture.h"

#include <yql/essentials/utils/log/log.h>

#include <library/cpp/logger/backend.h>
#include <library/cpp/logger/record.h>

#include <util/generic/guid.h>
#include <util/generic/yexception.h>
#include <util/string/split.h>

#include <mutex>
#include <regex>

namespace NYql::NTransportTest {

namespace {

class TMemoryLogBackend final : public TLogBackend {
public:
    explicit TMemoryLogBackend(std::shared_ptr<std::pair<std::mutex, TString>> sink)
        : Sink_(std::move(sink))
    {
    }

    void WriteData(const TLogRecord& record) override {
        std::lock_guard lock(Sink_->first);
        Sink_->second.append(record.Data, record.Len);
    }

    void ReopenLog() override {
    }

private:
    std::shared_ptr<std::pair<std::mutex, TString>> Sink_;
};

} // namespace

struct TLogCapture::TImpl {
    std::shared_ptr<std::pair<std::mutex, TString>> Sink = std::make_shared<std::pair<std::mutex, TString>>();
    std::unique_ptr<NLog::YqlLoggerScope> Scope;
};

TLogCapture::TLogCapture()
    : Impl_(std::make_unique<TImpl>())
{
    Impl_->Scope = std::make_unique<NLog::YqlLoggerScope>(new TMemoryLogBackend(Impl_->Sink));
    NLog::EComponentHelpers::ForEach([](NLog::EComponent component) {
        NLog::YqlLogger().SetComponentLevel(component, NLog::ELevel::TRACE);
    });

    const TString probe = "TLogCapture probe " + CreateGuidAsString();
    YQL_LOG(INFO) << probe;
    std::lock_guard lock(Impl_->Sink->first);
    if (!Impl_->Sink->second.Contains(probe)) {
        ythrow yexception() << "TLogCapture does not own the YQL logger: another YqlLoggerScope/InitLogger is active";
    }
    Impl_->Sink->second.clear();
}

TLogCapture::~TLogCapture() = default;

TString TLogCapture::Text() const {
    std::lock_guard lock(Impl_->Sink->first);
    return Impl_->Sink->second;
}

TVector<TString> TLogCapture::Lines() const {
    return StringSplitter(Text()).Split('\n').SkipEmpty();
}

bool TLogCapture::Contains(TStringBuf substring) const {
    return Text().Contains(substring);
}

size_t TLogCapture::CountLines(const TString& pattern) const {
    const std::regex regex(pattern.c_str(), std::regex::ECMAScript);
    size_t count = 0;
    for (const auto& line : Lines()) {
        if (std::regex_search(line.c_str(), regex)) {
            ++count;
        }
    }
    return count;
}

void TLogCapture::AssertNoSecret(TStringBuf secret) const {
    Y_ENSURE(!secret.empty(), "AssertNoSecret needs a non-empty secret");
    for (const auto& line : Lines()) {
        if (line.Contains(secret)) {
            ythrow yexception() << "secret leaked into the YQL log: " << line;
        }
    }
}

} // namespace NYql::NTransportTest
