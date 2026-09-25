#include "test_pki.h"

#include <util/generic/yexception.h>
#include <util/stream/file.h>
#include <util/string/builder.h>

#include <openssl/bio.h>
#include <openssl/bn.h>
#include <openssl/ec.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/obj_mac.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <atomic>
#include <memory>

namespace NYql::NTransportTest {

namespace {

TString OpenSslError(TStringBuf what) {
    TStringBuilder out;
    out << what << ":";
    while (const unsigned long code = ERR_get_error()) {
        char buf[256];
        ERR_error_string_n(code, buf, sizeof(buf));
        out << " " << buf;
    }
    return out;
}

#define TRANSPORT_TEST_SSL_ENSURE(cond, what)                       \
    do {                                                            \
        if (!(cond)) {                                              \
            ythrow yexception() << OpenSslError(what);              \
        }                                                           \
    } while (false)

struct TKeyDeleter {
    void operator()(EVP_PKEY* key) const {
        EVP_PKEY_free(key);
    }
};
struct TCertDeleter {
    void operator()(X509* cert) const {
        X509_free(cert);
    }
};
struct TBioDeleter {
    void operator()(BIO* bio) const {
        BIO_free(bio);
    }
};
struct TPkeyCtxDeleter {
    void operator()(EVP_PKEY_CTX* ctx) const {
        EVP_PKEY_CTX_free(ctx);
    }
};

using TKeyPtr = std::unique_ptr<EVP_PKEY, TKeyDeleter>;
using TCertPtr = std::unique_ptr<X509, TCertDeleter>;
using TBioPtr = std::unique_ptr<BIO, TBioDeleter>;

struct TKeyAndCert {
    TKeyPtr Key;
    TCertPtr Cert;
};

TKeyPtr GenerateKey() {
    std::unique_ptr<EVP_PKEY_CTX, TPkeyCtxDeleter> ctx(EVP_PKEY_CTX_new_id(EVP_PKEY_EC, nullptr));
    TRANSPORT_TEST_SSL_ENSURE(ctx, "EVP_PKEY_CTX_new_id");
    TRANSPORT_TEST_SSL_ENSURE(EVP_PKEY_keygen_init(ctx.get()) == 1, "EVP_PKEY_keygen_init");
    TRANSPORT_TEST_SSL_ENSURE(EVP_PKEY_CTX_set_ec_paramgen_curve_nid(ctx.get(), NID_X9_62_prime256v1) == 1, "set curve");
    TRANSPORT_TEST_SSL_ENSURE(EVP_PKEY_CTX_set_ec_param_enc(ctx.get(), OPENSSL_EC_NAMED_CURVE) == 1, "set param enc");
    EVP_PKEY* key = nullptr;
    TRANSPORT_TEST_SSL_ENSURE(EVP_PKEY_keygen(ctx.get(), &key) == 1, "EVP_PKEY_keygen");
    return TKeyPtr(key);
}

void AddExtension(X509* cert, X509* issuer, int nid, const TString& value) {
    X509V3_CTX ctx;
    X509V3_set_ctx_nodb(&ctx);
    X509V3_set_ctx(&ctx, issuer, cert, nullptr, nullptr, 0);
    X509_EXTENSION* ext = X509V3_EXT_conf_nid(nullptr, &ctx, nid, const_cast<char*>(value.c_str()));
    TRANSPORT_TEST_SSL_ENSURE(ext, TStringBuilder() << "X509V3_EXT_conf_nid " << nid << " " << value);
    const int added = X509_add_ext(cert, ext, -1);
    X509_EXTENSION_free(ext);
    TRANSPORT_TEST_SSL_ENSURE(added == 1, "X509_add_ext");
}

void SetName(X509_NAME* name, const TString& commonName) {
    TRANSPORT_TEST_SSL_ENSURE(X509_NAME_add_entry_by_txt(name, "O", MBSTRING_ASC,
        reinterpret_cast<const unsigned char*>("YDB transport test"), -1, -1, 0) == 1, "X509_NAME O");
    TRANSPORT_TEST_SSL_ENSURE(X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
        reinterpret_cast<const unsigned char*>(commonName.c_str()), -1, -1, 0) == 1, "X509_NAME CN");
}

struct TCertSpec {
    TString CommonName;
    TString SubjectAltName; // empty = no SAN
    bool IsCa = false;
    long NotBeforeOffsetSec = -3600;
    long NotAfterOffsetSec = 30 * 24 * 3600;
};

// Issues a certificate for a fresh key. `issuer == nullptr` makes it self-signed.
TKeyAndCert Issue(const TCertSpec& spec, const TKeyAndCert* issuer) {
    static std::atomic<long> serial = 1000;
    TKeyAndCert result{GenerateKey(), TCertPtr(X509_new())};
    X509* cert = result.Cert.get();
    TRANSPORT_TEST_SSL_ENSURE(cert, "X509_new");
    TRANSPORT_TEST_SSL_ENSURE(X509_set_version(cert, 2) == 1, "X509_set_version");
    TRANSPORT_TEST_SSL_ENSURE(ASN1_INTEGER_set(X509_get_serialNumber(cert), ++serial) == 1, "serial");
    TRANSPORT_TEST_SSL_ENSURE(X509_gmtime_adj(X509_getm_notBefore(cert), spec.NotBeforeOffsetSec), "notBefore");
    TRANSPORT_TEST_SSL_ENSURE(X509_gmtime_adj(X509_getm_notAfter(cert), spec.NotAfterOffsetSec), "notAfter");
    TRANSPORT_TEST_SSL_ENSURE(X509_set_pubkey(cert, result.Key.get()) == 1, "X509_set_pubkey");
    SetName(X509_get_subject_name(cert), spec.CommonName);

    X509* issuerCert = issuer ? issuer->Cert.get() : cert;
    EVP_PKEY* issuerKey = issuer ? issuer->Key.get() : result.Key.get();
    TRANSPORT_TEST_SSL_ENSURE(X509_set_issuer_name(cert, X509_get_subject_name(issuerCert)) == 1, "X509_set_issuer_name");

    if (spec.IsCa) {
        AddExtension(cert, issuerCert, NID_basic_constraints, "critical,CA:TRUE");
        AddExtension(cert, issuerCert, NID_key_usage, "critical,keyCertSign,cRLSign");
    } else {
        AddExtension(cert, issuerCert, NID_basic_constraints, "critical,CA:FALSE");
        AddExtension(cert, issuerCert, NID_key_usage, "critical,digitalSignature,keyEncipherment");
        AddExtension(cert, issuerCert, NID_ext_key_usage, "serverAuth,clientAuth");
    }
    AddExtension(cert, issuerCert, NID_subject_key_identifier, "hash");
    if (issuer) {
        AddExtension(cert, issuerCert, NID_authority_key_identifier, "keyid:always");
    }
    if (spec.SubjectAltName) {
        AddExtension(cert, issuerCert, NID_subject_alt_name, spec.SubjectAltName);
    }
    TRANSPORT_TEST_SSL_ENSURE(X509_sign(cert, issuerKey, EVP_sha256()) > 0, "X509_sign");
    return result;
}

TString ReadBio(BIO* bio) {
    char* data = nullptr;
    const long len = BIO_get_mem_data(bio, &data);
    return TString(data, len);
}

TString CertToPem(X509* cert) {
    TBioPtr bio(BIO_new(BIO_s_mem()));
    TRANSPORT_TEST_SSL_ENSURE(bio && PEM_write_bio_X509(bio.get(), cert) == 1, "PEM_write_bio_X509");
    return ReadBio(bio.get());
}

TString KeyToPem(EVP_PKEY* key) {
    TBioPtr bio(BIO_new(BIO_s_mem()));
    TRANSPORT_TEST_SSL_ENSURE(bio && PEM_write_bio_PrivateKey(bio.get(), key, nullptr, nullptr, 0, nullptr, nullptr) == 1,
        "PEM_write_bio_PrivateKey");
    return ReadBio(bio.get());
}

TTestCert Store(const TKeyAndCert& keyAndCert, const TString& dir, const TString& name) {
    TTestCert out;
    out.CertPem = CertToPem(keyAndCert.Cert.get());
    out.KeyPem = KeyToPem(keyAndCert.Key.get());
    out.CertFile = TStringBuilder() << dir << "/" << name << ".crt";
    out.KeyFile = TStringBuilder() << dir << "/" << name << ".key";
    TFileOutput(out.CertFile).Write(out.CertPem);
    TFileOutput(out.KeyFile).Write(out.KeyPem);
    return out;
}

const TString LOCAL_SAN = "DNS:localhost,IP:127.0.0.1";

} // namespace

TTestPki::TTestPki() {
    const TString& dir = Dir_.Name();

    const auto ca = Issue({.CommonName = "YDB transport test CA", .IsCa = true, .NotAfterOffsetSec = 365 * 24 * 3600}, nullptr);
    Ca_ = Store(ca, dir, "ca");
    Leaf_ = Store(Issue({.CommonName = "localhost", .SubjectAltName = LOCAL_SAN}, &ca), dir, "leaf");
    WrongHostLeaf_ = Store(Issue({.CommonName = "wrong.test", .SubjectAltName = "DNS:wrong.test"}, &ca), dir, "wrong_host");
    SelfSignedLeaf_ = Store(Issue({.CommonName = "localhost", .SubjectAltName = LOCAL_SAN}, nullptr), dir, "self_signed");
    ExpiredLeaf_ = Store(Issue({
        .CommonName = "localhost",
        .SubjectAltName = LOCAL_SAN,
        .NotBeforeOffsetSec = -2 * 24 * 3600,
        .NotAfterOffsetSec = -1 * 24 * 3600,
    }, &ca), dir, "expired");

    const auto untrustedCa = Issue({.CommonName = "YDB transport test untrusted CA", .IsCa = true}, nullptr);
    UntrustedCa_ = Store(untrustedCa, dir, "untrusted_ca");
    UntrustedLeaf_ = Store(Issue({.CommonName = "localhost", .SubjectAltName = LOCAL_SAN}, &untrustedCa), dir, "untrusted_leaf");
}

TTestPki::~TTestPki() = default;

} // namespace NYql::NTransportTest
