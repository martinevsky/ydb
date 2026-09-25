#pragma once

// H2: a throw-away PKI generated at runtime with OpenSSL (EC P-256 keys, SHA-256 signatures).
// Everything is created in the constructor and written as PEM files into a private temp dir that is
// removed with the object. No certificate is checked into the repo and nothing depends on the host
// trust store.

#include <util/folder/tempdir.h>
#include <util/generic/string.h>

namespace NYql::NTransportTest {

struct TTestCert {
    TString CertPem;
    TString KeyPem;
    TString CertFile; // absolute path of CertPem on disk
    TString KeyFile;  // absolute path of KeyPem on disk
};

class TTestPki {
public:
    // Throws yexception if any OpenSSL call fails.
    TTestPki();
    ~TTestPki();

    // Trusted test CA (pass CaFile() to the client under test).
    const TTestCert& Ca() const {
        return Ca_;
    }
    const TString& CaFile() const {
        return Ca_.CertFile;
    }

    // Signed by Ca(); SAN = DNS:localhost, IP:127.0.0.1. Valid now.
    const TTestCert& Leaf() const {
        return Leaf_;
    }
    // Signed by Ca(); SAN = DNS:wrong.test only (hostname mismatch for localhost / 127.0.0.1).
    const TTestCert& WrongHostLeaf() const {
        return WrongHostLeaf_;
    }
    // Self-signed (issuer == subject, not a CA); SAN = DNS:localhost, IP:127.0.0.1.
    const TTestCert& SelfSignedLeaf() const {
        return SelfSignedLeaf_;
    }
    // Signed by Ca(); SAN = DNS:localhost, IP:127.0.0.1; notBefore = now - 2 days, notAfter = now - 1 day.
    const TTestCert& ExpiredLeaf() const {
        return ExpiredLeaf_;
    }
    // A second CA that no client trusts, and a leaf (SAN localhost, 127.0.0.1) signed by it.
    const TTestCert& UntrustedCa() const {
        return UntrustedCa_;
    }
    const TTestCert& UntrustedLeaf() const {
        return UntrustedLeaf_;
    }

    const TString& Dir() const {
        return Dir_.Name();
    }

private:
    TTempDir Dir_;
    TTestCert Ca_;
    TTestCert Leaf_;
    TTestCert WrongHostLeaf_;
    TTestCert SelfSignedLeaf_;
    TTestCert ExpiredLeaf_;
    TTestCert UntrustedCa_;
    TTestCert UntrustedLeaf_;
};

} // namespace NYql::NTransportTest
