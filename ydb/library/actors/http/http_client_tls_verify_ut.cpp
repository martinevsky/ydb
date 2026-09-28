#include "http_proxy.h"

#include <ydb/core/security/certificate_check/test_utils/test_cert_auth_utils.h>
#include <ydb/library/actors/core/hfunc.h>
#include <ydb/library/actors/testlib/test_runtime.h>

#include <library/cpp/testing/unittest/registar.h>
#include <library/cpp/testing/unittest/tests_data.h>

#include <util/string/ascii.h>
#include <util/system/env.h>
#include <util/system/tempfile.h>

#include <atomic>
#include <memory>

// T-TLS-6 (FQ transport test plan, finding F-A-1): the NHttp client and the verification of the
// server certificate. The opt-in knob is TEvHttpOutgoingRequest::CaFile; without it the client does not
// verify the server (legacy behaviour).

namespace {

// Same contract as YDB_SKIP_KNOWN_BUG in ydb/library/yql/providers/common/ut_helpers/transport/known_bug.h
// (not linked here: ydb/library/actors must not depend on ydb/library/yql). A contract test that fails on
// today's code is skipped unless YDB_TRANSPORT_RUN_KNOWN_BUGS=1.
bool SkipKnownBug(TStringBuf bugId) {
    if (GetEnv("YDB_TRANSPORT_RUN_KNOWN_BUGS") == "1") {
        Cerr << "RUN known bug " << bugId << " (YDB_TRANSPORT_RUN_KNOWN_BUGS=1)" << Endl;
        return false;
    }
    Cerr << "SKIP known bug " << bugId << Endl;
    return true;
}

#define SKIP_KNOWN_BUG(bugId)        \
    do {                             \
        if (SkipKnownBug(bugId)) {   \
            return;                  \
        }                            \
    } while (false)

// Failure deadline for every edge-event wait (real-threads runtime: the dispatch timeout throws when no
// event arrives in time). Never a pass condition.
const TDuration Guard = TDuration::Seconds(20);

void WriteTempFile(TTempFileHandle& file, const std::string& content) {
    file.Write(content.data(), content.size());
    file.FlushData();
}

bool LooksLikeCertificateError(const TString& error) {
    const TString lower = to_lower(error);
    return lower.Contains("certificate") || lower.Contains("verify");
}

// Server-side handler: counts the requests that reached the server and answers each with 200 "ok".
class TCountingHandler : public NActors::TActor<TCountingHandler> {
public:
    explicit TCountingHandler(std::shared_ptr<std::atomic<ui32>> requests)
        : TActor(&TCountingHandler::StateFunc)
        , Requests(std::move(requests))
    {}

    STRICT_STFUNC(StateFunc,
        hFunc(NHttp::TEvHttpProxy::TEvHttpIncomingRequest, Handle);
    )

private:
    void Handle(NHttp::TEvHttpProxy::TEvHttpIncomingRequest::TPtr& ev) {
        // Counted before the response is sent, so the client's response happens after the increment.
        ++*Requests;
        Send(ev->Sender, new NHttp::TEvHttpProxy::TEvHttpOutgoingResponse(ev->Get()->Request->CreateResponseOK("ok", "text/plain")));
    }

    const std::shared_ptr<std::atomic<ui32>> Requests;
};

// A real-threads actor runtime with an NHttp proxy that serves https on one port with the given server
// certificate, and a client edge actor that sends one GET to https://localhost:<port>/x.
struct TTlsServerSetup {
    NActors::TTestActorRuntimeBase Runtime{1, true};
    TPortManager PortManager;
    TIpPort Port = 0;
    TTempFileHandle CertFile;
    TTempFileHandle KeyFile;
    NActors::TActorId ProxyId;
    NActors::TActorId ClientId;
    std::shared_ptr<std::atomic<ui32>> HandlerRequests = std::make_shared<std::atomic<ui32>>(0);

    explicit TTlsServerSetup(const NKikimr::NCertTestUtils::TCertAndKey& serverCert) {
        WriteTempFile(CertFile, serverCert.Certificate);
        WriteTempFile(KeyFile, serverCert.PrivateKey);
        Runtime.Initialize();
        Runtime.SetDispatchTimeout(Guard);
        ProxyId = Runtime.Register(NHttp::CreateHttpProxy());

        Port = PortManager.GetTcpPort();
        auto add = MakeHolder<NHttp::TEvHttpProxy::TEvAddListeningPort>(Port);
        add->Secure = true;
        add->CertificateFile = CertFile.Name();
        add->PrivateKeyFile = KeyFile.Name();
        Runtime.Send(new NActors::IEventHandle(ProxyId, Runtime.AllocateEdgeActor(), add.Release()), 0, true);
        TAutoPtr<NActors::IEventHandle> handle;
        Runtime.GrabEdgeEvent<NHttp::TEvHttpProxy::TEvConfirmListen>(handle);

        const NActors::TActorId handlerId = Runtime.Register(new TCountingHandler(HandlerRequests));
        Runtime.Send(new NActors::IEventHandle(ProxyId, handlerId, new NHttp::TEvHttpProxy::TEvRegisterHandler("/x", handlerId)), 0, true);
        ClientId = Runtime.AllocateEdgeActor();
    }

    NHttp::TEvHttpProxy::TEvHttpIncomingResponse::TPtr Get(const TString& caFile) {
        auto request = NHttp::THttpOutgoingRequest::CreateRequestGet(TStringBuilder() << "https://localhost:" << Port << "/x");
        auto event = MakeHolder<NHttp::TEvHttpProxy::TEvHttpOutgoingRequest>(request);
        event->CaFile = caFile;
        Runtime.Send(new NActors::IEventHandle(ProxyId, ClientId, event.Release()), 0, true);
        auto response = Runtime.GrabEdgeEvent<NHttp::TEvHttpProxy::TEvHttpIncomingResponse>(ClientId);
        UNIT_ASSERT_C(response, "no response within the guard");
        return response;
    }

    // The client is refused during the handshake: the response carries a certificate error and the
    // server handler received nothing (checked after the response: an accepted request is answered
    // by the handler before the client can see any response).
    void ExpectRejected(const TString& caFile) {
        auto response = Get(caFile);
        const TString error = response->Get()->GetError();
        Cerr << "client error: '" << error << "', handler requests: " << HandlerRequests->load() << Endl;
        UNIT_ASSERT_VALUES_EQUAL_C(HandlerRequests->load(), 0u, "the request reached the server over an unverified TLS connection");
        UNIT_ASSERT_C(LooksLikeCertificateError(error), "expected a certificate error, got '" << error << "'");
    }

    void ExpectServedOk(const TString& caFile) {
        auto response = Get(caFile);
        UNIT_ASSERT_C(response->Get()->GetError().empty(), response->Get()->GetError());
        UNIT_ASSERT_VALUES_EQUAL(response->Get()->Response->Status, "200");
        UNIT_ASSERT_VALUES_EQUAL(response->Get()->Response->Body, "ok");
        UNIT_ASSERT_VALUES_EQUAL(HandlerRequests->load(), 1u);
    }
};

struct TTestCerts {
    NKikimr::NCertTestUtils::TCertAndKey Ca;
    NKikimr::NCertTestUtils::TCertAndKey Leaf;          // signed by Ca; SAN localhost, 127.0.0.1, ::1
    NKikimr::NCertTestUtils::TCertAndKey WrongHostLeaf; // signed by Ca; SAN wrong.test only
    NKikimr::NCertTestUtils::TCertAndKey SelfSigned;    // self-signed; SAN localhost, 127.0.0.1, ::1
    TTempFileHandle CaFile;

    TTestCerts()
        : Ca(NKikimr::NCertTestUtils::GenerateCA(NKikimr::NCertTestUtils::TProps::AsCA()))
        , Leaf(NKikimr::NCertTestUtils::GenerateSignedCert(Ca, NKikimr::NCertTestUtils::TProps::AsServer()))
        , WrongHostLeaf(NKikimr::NCertTestUtils::GenerateSignedCert(Ca, WrongHostProps()))
        , SelfSigned(NKikimr::NCertTestUtils::GenerateCA(SelfSignedProps()))
    {
        WriteTempFile(CaFile, Ca.Certificate);
    }

    static NKikimr::NCertTestUtils::TProps WrongHostProps() {
        auto props = NKikimr::NCertTestUtils::TProps::AsServer();
        props.CommonName = "wrong.test";
        props.AltNames = {"DNS:wrong.test"};
        return props;
    }

    static NKikimr::NCertTestUtils::TProps SelfSignedProps() {
        auto props = NKikimr::NCertTestUtils::TProps::AsServer();
        props.CommonName = "self-signed localhost";
        props.AuthorityKeyIdentifier = "";
        return props;
    }
};

} // namespace

Y_UNIT_TEST_SUITE(THttpClientTlsVerify) {
    // Contract (F-A-1): an https request to a server with a self-signed certificate is refused by default.
    // Today the client does not verify the server, so the request (with whatever credentials it carries)
    // reaches the server.
    Y_UNIT_TEST(SelfSignedServerRejectedByDefault) {
        SKIP_KNOWN_BUG("F-A-1");
        TTestCerts certs;
        TTlsServerSetup setup(certs.SelfSigned);
        setup.ExpectRejected({});
    }

    // Seam S6 (opt-in CaFile): a self-signed server is refused before any request is sent.
    Y_UNIT_TEST(CaFileRejectsSelfSignedServer) {
        TTestCerts certs;
        TTlsServerSetup setup(certs.SelfSigned);
        setup.ExpectRejected(certs.CaFile.Name());
    }

    // Seam S6: a certificate signed by the trusted CA but issued for another name is refused.
    Y_UNIT_TEST(CaFileRejectsHostNameMismatch) {
        TTestCerts certs;
        TTlsServerSetup setup(certs.WrongHostLeaf);
        setup.ExpectRejected(certs.CaFile.Name());
    }

    // Seam S6: a certificate signed by the trusted CA for this host name is accepted.
    Y_UNIT_TEST(CaFileAcceptsCaSignedServer) {
        TTestCerts certs;
        TTlsServerSetup setup(certs.Leaf);
        setup.ExpectServedOk(certs.CaFile.Name());
    }

    // Seam S6: a CaFile that cannot be loaded fails closed and is reported as such.
    Y_UNIT_TEST(UnloadableCaFileIsReported) {
        TTestCerts certs;
        TTlsServerSetup setup(certs.Leaf);
        const TString caFile = "/nonexistent/ca.pem";
        auto response = setup.Get(caFile);
        const TString error = response->Get()->GetError();
        Cerr << "client error: '" << error << "'" << Endl;
        UNIT_ASSERT_VALUES_EQUAL(setup.HandlerRequests->load(), 0u);
        UNIT_ASSERT_STRING_CONTAINS(error, "failed to load CA file " + caFile);
    }

    // Pin: without CaFile the client keeps today's behaviour and talks to a self-signed server.
    Y_UNIT_TEST(NoCaFileKeepsLegacyNoVerify) {
        TTestCerts certs;
        TTlsServerSetup setup(certs.SelfSigned);
        setup.ExpectServedOk({});
    }
}
