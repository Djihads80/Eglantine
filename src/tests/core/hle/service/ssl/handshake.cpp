// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <future>
#include <initializer_list>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <openssl/bio.h>
#include <openssl/ec.h>
#include <openssl/evp.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include "core/hle/service/ssl/cert_store.h"
#include "core/hle/service/ssl/ssl_backend.h"
#include "core/internal_network/network.h"
#include "core/internal_network/sockets.h"
#include "tests/core/hle/service/ssl/private_server_ca.h"

// Handshakes of the emulator's SSL backend, as a connection makes them, with a server run by
// OpenSSL alone on a loopback socket. The certificates are made for the test.

namespace {

namespace ssl = Service::SSL;
using ssl::VerifyOption;

using Key = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
using Certificate = std::unique_ptr<X509, decltype(&X509_free)>;

constexpr long Day = 24 * 60 * 60;
constexpr auto DefaultVerifyOption = VerifyOption::PeerCa | VerifyOption::HostName;

Key MakeKey() {
    EVP_PKEY* key = nullptr;
    EVP_PKEY_CTX* const context = EVP_PKEY_CTX_new_id(EVP_PKEY_EC, nullptr);
    if (context && EVP_PKEY_keygen_init(context) == 1 &&
        EVP_PKEY_CTX_set_ec_paramgen_curve_nid(context, NID_X9_62_prime256v1) == 1) {
        EVP_PKEY_keygen(context, &key);
    }
    EVP_PKEY_CTX_free(context);
    REQUIRE(key);
    return Key{key, &EVP_PKEY_free};
}

void AddExtension(X509* certificate, X509V3_CTX* context, int nid, const std::string& value) {
    X509_EXTENSION* const extension = X509V3_EXT_conf_nid(nullptr, context, nid, value.c_str());
    REQUIRE(extension);
    REQUIRE(X509_add_ext(certificate, extension, -1) == 1);
    X509_EXTENSION_free(extension);
}

// A CA certificate when issuer is null, which it then signs itself, or else one for the host
// name, signed by issuer. Valid from not_before to not_after seconds from now.
Certificate MakeCertificate(EVP_PKEY* key, const std::string& name, X509* issuer,
                            EVP_PKEY* issuer_key, long not_before, long not_after) {
    static long serial = 1;
    Certificate certificate{X509_new(), &X509_free};
    X509* const x509 = certificate.get();
    REQUIRE(x509);
    REQUIRE(X509_set_version(x509, 2) == 1);
    REQUIRE(ASN1_INTEGER_set(X509_get_serialNumber(x509), serial++) == 1);
    REQUIRE(X509_gmtime_adj(X509_getm_notBefore(x509), not_before));
    REQUIRE(X509_gmtime_adj(X509_getm_notAfter(x509), not_after));
    REQUIRE(X509_set_pubkey(x509, key) == 1);
    const std::unique_ptr<X509_NAME, decltype(&X509_NAME_free)> subject{X509_NAME_new(),
                                                                       &X509_NAME_free};
    REQUIRE(subject);
    REQUIRE(X509_NAME_add_entry_by_txt(subject.get(), "CN", MBSTRING_ASC,
                                       reinterpret_cast<const unsigned char*>(name.c_str()),
                                       -1, -1, 0) == 1);
    REQUIRE(X509_set_subject_name(x509, subject.get()) == 1);
    const bool ca = issuer == nullptr;
    REQUIRE(X509_set_issuer_name(x509, X509_get_subject_name(ca ? x509 : issuer)) == 1);

    X509V3_CTX context{};
    X509V3_set_ctx(&context, ca ? x509 : issuer, x509, nullptr, nullptr, 0);
    AddExtension(x509, &context, NID_subject_key_identifier, "hash");
    if (ca) {
        AddExtension(x509, &context, NID_basic_constraints, "critical,CA:TRUE");
        AddExtension(x509, &context, NID_key_usage, "critical,keyCertSign,cRLSign");
    } else {
        AddExtension(x509, &context, NID_authority_key_identifier, "keyid:always");
        AddExtension(x509, &context, NID_basic_constraints, "critical,CA:FALSE");
        AddExtension(x509, &context, NID_subject_alt_name, "DNS:" + name);
    }
    REQUIRE(X509_sign(x509, ca ? key : issuer_key, EVP_sha256()) > 0);
    return certificate;
}

struct Pki {
    Key ca_key = MakeKey();
    Certificate ca = MakeCertificate(ca_key.get(), "Test CA", nullptr, nullptr, -Day, Day);
    /// A CA that signed none of the server's certificates
    Key other_ca_key = MakeKey();
    Certificate other_ca =
        MakeCertificate(other_ca_key.get(), "Other CA", nullptr, nullptr, -Day, Day);
    Key server_key = MakeKey();
    Certificate server =
        MakeCertificate(server_key.get(), "server.test", ca.get(), ca_key.get(), -Day, Day);
    Certificate expired =
        MakeCertificate(server_key.get(), "server.test", ca.get(), ca_key.get(), -2 * Day, -Day);
};

const Pki& GetPki() {
    static const Pki pki;
    return pki;
}

std::vector<u8> Der(X509* certificate) {
    const int size = i2d_X509(certificate, nullptr);
    REQUIRE(size > 0);
    std::vector<u8> der(static_cast<size_t>(size));
    unsigned char* out = der.data();
    REQUIRE(i2d_X509(certificate, &out) == size);
    return der;
}

std::shared_ptr<const ssl::SSLTrustStore> MakeTrustStore(const ssl::SSLTrustStore* base,
                                                         std::initializer_list<X509*> trusted) {
    std::vector<std::vector<u8>> der;
    for (X509* const certificate : trusted) {
        der.push_back(Der(certificate));
    }
    const std::vector<std::span<const u8>> certificates(der.begin(), der.end());
    std::shared_ptr<const ssl::SSLTrustStore> store;
    REQUIRE(ssl::CreateSSLTrustStore(base, certificates, &store).IsSuccess());
    REQUIRE(store);
    return store;
}

bool SendAll(Network::SocketBase& socket, std::span<const u8> data) {
    while (!data.empty()) {
        const auto [sent, error] = socket.Send(data, 0);
        if (error != Network::Errno::SUCCESS || sent <= 0) {
            return false;
        }
        data = data.subspan(static_cast<size_t>(sent));
    }
    return true;
}

// The server's side of a handshake on socket. Whether it finished.
bool ServeHandshake(SSL_CTX* context, Network::SocketBase& socket) {
    const std::unique_ptr<SSL, decltype(&SSL_free)> server{SSL_new(context), &SSL_free};
    BIO* const in = BIO_new(BIO_s_mem());
    BIO* const out = BIO_new(BIO_s_mem());
    if (!server || !in || !out) {
        BIO_free(in);
        BIO_free(out);
        return false;
    }
    SSL_set_bio(server.get(), in, out);
    SSL_set_accept_state(server.get());

    std::array<u8, 0x4000> buffer;
    while (true) {
        const int ret = SSL_do_handshake(server.get());
        bool sent = true;
        int size;
        while (sent && (size = BIO_read(out, buffer.data(), static_cast<int>(buffer.size()))) > 0) {
            sent = SendAll(socket, std::span{buffer}.first(static_cast<size_t>(size)));
        }
        if (ret == 1) {
            return true;
        }
        if (!sent || SSL_get_error(server.get(), ret) != SSL_ERROR_WANT_READ) {
            return false;
        }
        const auto [received, error] = socket.Recv(0, buffer);
        if (error != Network::Errno::SUCCESS || received <= 0) {
            return false;
        }
        BIO_write(in, buffer.data(), received);
    }
}

struct Outcome {
    Result handshake = ResultSuccess;               ///< What DoHandshake returned
    Result verify_cert_error = ResultSuccess;       ///< What GetVerifyCertError returned after it
    Result verify_cert_error_again = ResultSuccess; ///< What GetVerifyCertError returned next
    bool server_finished = false;
};

// Handshakes with a server presenting certificate, as a connection with verify option, host
// name (none if null) and trust store does.
Outcome Handshake(X509* certificate, VerifyOption option, const char* host_name,
                  std::shared_ptr<const ssl::SSLTrustStore> trust_store) {
    Network::NetworkInstance network_instance;
    Network::Socket listener;
    REQUIRE(listener.Initialize(Network::Domain::INET, Network::Type::STREAM,
                                Network::Protocol::TCP) == Network::Errno::SUCCESS);
    REQUIRE(listener.Bind({Network::Domain::INET, {127, 0, 0, 1}, 0}) ==
            Network::Errno::SUCCESS);
    REQUIRE(listener.Listen(1) == Network::Errno::SUCCESS);
    const std::pair<Network::SockAddrIn, Network::Errno> address = listener.GetSockName();
    REQUIRE(address.second == Network::Errno::SUCCESS);

    // Connected before accepting, so that nothing waits on a connection that never comes. Either
    // side gives up on a peer silent for 5 seconds.
    auto client = std::make_shared<Network::Socket>();
    REQUIRE(client->Initialize(Network::Domain::INET, Network::Type::STREAM,
                               Network::Protocol::TCP) == Network::Errno::SUCCESS);
    REQUIRE(client->SetRcvTimeo(5000) == Network::Errno::SUCCESS);
    REQUIRE(client->Connect(address.first) == Network::Errno::SUCCESS);
    auto accepted = listener.Accept();
    REQUIRE(accepted.second == Network::Errno::SUCCESS);
    Network::SocketBase& server_socket = *accepted.first.socket;
    REQUIRE(server_socket.SetRcvTimeo(5000) == Network::Errno::SUCCESS);

    const std::unique_ptr<SSL_CTX, decltype(&SSL_CTX_free)> server_context{
        SSL_CTX_new(TLS_server_method()), &SSL_CTX_free};
    REQUIRE(server_context);
    REQUIRE(SSL_CTX_use_certificate(server_context.get(), certificate) == 1);
    REQUIRE(SSL_CTX_use_PrivateKey(server_context.get(), GetPki().server_key.get()) == 1);
    // No session tickets, which the client would leave unread
    REQUIRE(SSL_CTX_set_num_tickets(server_context.get(), 0) == 1);

    // Nothing may fail the test from here until the server is done
    auto server = std::async(std::launch::async, [&server_context, &server_socket] {
        return ServeHandshake(server_context.get(), server_socket);
    });

    Outcome outcome;
    std::unique_ptr<ssl::SSLConnectionBackend> backend;
    outcome.handshake = ssl::CreateSSLConnectionBackend(&backend);
    if (outcome.handshake.IsSuccess()) {
        backend->SetSocket(client);
        backend->SetVerifyOption(option);
        backend->SetTrustStore(std::move(trust_store));
        if (host_name) {
            outcome.handshake = backend->SetHostName(host_name);
        }
        if (outcome.handshake.IsSuccess()) {
            outcome.handshake = backend->DoHandshake();
        }
        outcome.verify_cert_error = backend->GetVerifyCertError();
        outcome.verify_cert_error_again = backend->GetVerifyCertError();
        backend.reset();
    }
    client.reset();
    outcome.server_finished = server.get();
    return outcome;
}

/// The trust store of a cert store without firmware, made with private_server_enabled and
/// private_server_ca set to enabled and ca. They are restored before the store is used, as the
/// private server mode limits where sockets connect.
std::shared_ptr<const ssl::SSLTrustStore> CertStoreTrustStore(bool enabled, const std::string& ca) {
    const PrivateServerCaSettings settings{enabled, ca};
    ssl::CertStore cert_store{nullptr};
    return cert_store.GetTrustStore();
}

} // namespace

TEST_CASE("SSL handshakes trust a certificate for the host name from a trusted CA",
          "[core][ssl]") {
    const Pki& pki = GetPki();
    const Outcome outcome = Handshake(pki.server.get(), DefaultVerifyOption, "server.test",
                                      MakeTrustStore(nullptr, {pki.ca.get()}));
    CHECK(outcome.handshake.raw == ResultSuccess.raw);
    CHECK(outcome.verify_cert_error.raw == ResultSuccess.raw);
    CHECK(outcome.server_finished);
}

TEST_CASE("SSL handshakes refuse a certificate for another host name", "[core][ssl]") {
    const Pki& pki = GetPki();
    const Outcome outcome = Handshake(pki.server.get(), DefaultVerifyOption, "other.test",
                                      MakeTrustStore(nullptr, {pki.ca.get()}));
    CHECK(outcome.handshake.raw == ssl::ResultSslErrorInvalidCertDomain.raw);
    CHECK(outcome.verify_cert_error.raw == ssl::ResultSslErrorInvalidCertDomain.raw);
    CHECK(outcome.verify_cert_error_again.raw == ResultSuccess.raw);
    CHECK_FALSE(outcome.server_finished);
}

TEST_CASE("SSL handshakes refuse a certificate from a CA not trusted", "[core][ssl]") {
    const Pki& pki = GetPki();
    const Outcome other_ca = Handshake(pki.server.get(), DefaultVerifyOption, "server.test",
                                       MakeTrustStore(nullptr, {pki.other_ca.get()}));
    CHECK(other_ca.handshake.raw == ssl::ResultSslErrorUnknownCa.raw);
    CHECK(other_ca.verify_cert_error.raw == ssl::ResultSslErrorUnknownCa.raw);
    CHECK_FALSE(other_ca.server_finished);

    // Without a store, nothing is trusted: not even what the host trusts
    const Outcome no_store =
        Handshake(pki.server.get(), DefaultVerifyOption, "server.test", nullptr);
    CHECK(no_store.handshake.raw == ssl::ResultSslErrorUnknownCa.raw);
    CHECK_FALSE(no_store.server_finished);
}

TEST_CASE("SSL handshakes without verification accept any certificate", "[core][ssl]") {
    const Pki& pki = GetPki();
    const Outcome outcome =
        Handshake(pki.expired.get(), VerifyOption::None, "other.test", nullptr);
    CHECK(outcome.handshake.raw == ResultSuccess.raw);
    CHECK(outcome.verify_cert_error.raw == ResultSuccess.raw);
    CHECK(outcome.server_finished);
}

TEST_CASE("SSL verify options check the chain and the host name apart", "[core][ssl]") {
    const Pki& pki = GetPki();
    const auto trusted = MakeTrustStore(nullptr, {pki.ca.get()});
    const auto untrusted = MakeTrustStore(nullptr, {pki.other_ca.get()});

    const Outcome chain_only_other_name =
        Handshake(pki.server.get(), VerifyOption::PeerCa, "other.test", trusted);
    CHECK(chain_only_other_name.handshake.raw == ResultSuccess.raw);
    CHECK(chain_only_other_name.server_finished);

    const Outcome chain_only_untrusted =
        Handshake(pki.server.get(), VerifyOption::PeerCa, "server.test", untrusted);
    CHECK(chain_only_untrusted.handshake.raw == ssl::ResultSslErrorUnknownCa.raw);

    const Outcome name_only_untrusted =
        Handshake(pki.server.get(), VerifyOption::HostName, "server.test", untrusted);
    CHECK(name_only_untrusted.handshake.raw == ResultSuccess.raw);
    CHECK(name_only_untrusted.server_finished);

    const Outcome name_only_other_name =
        Handshake(pki.server.get(), VerifyOption::HostName, "other.test", untrusted);
    CHECK(name_only_other_name.handshake.raw == ssl::ResultSslErrorInvalidCertDomain.raw);
}

TEST_CASE("SSL handshakes check certificate dates only with DateCheck", "[core][ssl]") {
    const Pki& pki = GetPki();
    const auto trusted = MakeTrustStore(nullptr, {pki.ca.get()});

    const Outcome without_date_check =
        Handshake(pki.expired.get(), DefaultVerifyOption, "server.test", trusted);
    CHECK(without_date_check.handshake.raw == ResultSuccess.raw);
    CHECK(without_date_check.server_finished);

    const Outcome with_date_check = Handshake(
        pki.expired.get(), DefaultVerifyOption | VerifyOption::DateCheck, "server.test", trusted);
    CHECK(with_date_check.handshake.raw == ssl::ResultSslErrorExpiredCertificate.raw);
    CHECK_FALSE(with_date_check.server_finished);
}

TEST_CASE("SSL trust stores made from another trust what either trusts", "[core][ssl]") {
    const Pki& pki = GetPki();
    // As a context's store adds its imported server PKIs to the console's built-in certificates
    const auto built_in = MakeTrustStore(nullptr, {pki.other_ca.get()});
    const auto with_import = MakeTrustStore(built_in.get(), {pki.ca.get()});
    const auto import_on_trusted =
        MakeTrustStore(MakeTrustStore(nullptr, {pki.ca.get()}).get(), {pki.other_ca.get()});

    CHECK(Handshake(pki.server.get(), DefaultVerifyOption, "server.test", with_import)
              .handshake.raw == ResultSuccess.raw);
    CHECK(Handshake(pki.server.get(), DefaultVerifyOption, "server.test", import_on_trusted)
              .handshake.raw == ResultSuccess.raw);
    // Making a store from it leaves the base as it was, as removing the import does
    CHECK(Handshake(pki.server.get(), DefaultVerifyOption, "server.test", built_in)
              .handshake.raw == ssl::ResultSslErrorUnknownCa.raw);
}

TEST_CASE("SSL handshakes refuse checks they cannot make", "[core][ssl]") {
    const Pki& pki = GetPki();
    const auto trusted = MakeTrustStore(nullptr, {pki.ca.get()});

    const Outcome no_host_name =
        Handshake(pki.server.get(), DefaultVerifyOption, nullptr, trusted);
    CHECK(no_host_name.handshake.IsError());
    CHECK_FALSE(no_host_name.server_finished);

    const Outcome extended_validation =
        Handshake(pki.server.get(),
                  DefaultVerifyOption | VerifyOption::DateCheck | VerifyOption::EvPolicyOid,
                  "server.test", trusted);
    CHECK(extended_validation.handshake.IsError());
    CHECK_FALSE(extended_validation.server_finished);
}

TEST_CASE("SSL handshakes trust the private server CA of the cert store", "[core][ssl]") {
    const Pki& pki = GetPki();
    const TemporaryFile ca_file{Der(pki.ca.get())};
    // Without firmware, the private server CA is the only built-in certificate
    const auto trust_store = CertStoreTrustStore(true, ca_file.Path());
    REQUIRE(trust_store);

    const Outcome trusted =
        Handshake(pki.server.get(), DefaultVerifyOption, "server.test", trust_store);
    CHECK(trusted.handshake.raw == ResultSuccess.raw);
    CHECK(trusted.server_finished);

    // It is a CA like the others, which certificates for another host name do not get past
    const Outcome other_name =
        Handshake(pki.server.get(), DefaultVerifyOption, "other.test", trust_store);
    CHECK(other_name.handshake.raw == ssl::ResultSslErrorInvalidCertDomain.raw);
    CHECK_FALSE(other_name.server_finished);

    // A context's imported server PKIs add to it
    const auto with_import = MakeTrustStore(trust_store.get(), {pki.other_ca.get()});
    CHECK(Handshake(pki.server.get(), DefaultVerifyOption, "server.test", with_import)
              .handshake.raw == ResultSuccess.raw);
}

TEST_CASE("SSL handshakes trust no private server CA that is not configured", "[core][ssl]") {
    const Pki& pki = GetPki();
    const TemporaryFile ca_file{Der(pki.ca.get())};
    const TemporaryFile other_ca_file{Der(pki.other_ca.get())};

    const Outcome not_enabled = Handshake(pki.server.get(), DefaultVerifyOption, "server.test",
                                          CertStoreTrustStore(false, ca_file.Path()));
    CHECK(not_enabled.handshake.raw == ssl::ResultSslErrorUnknownCa.raw);
    CHECK_FALSE(not_enabled.server_finished);

    const Outcome other_ca = Handshake(pki.server.get(), DefaultVerifyOption, "server.test",
                                       CertStoreTrustStore(true, other_ca_file.Path()));
    CHECK(other_ca.handshake.raw == ssl::ResultSslErrorUnknownCa.raw);
    CHECK_FALSE(other_ca.server_finished);

    const Outcome missing_ca =
        Handshake(pki.server.get(), DefaultVerifyOption, "server.test",
                  CertStoreTrustStore(true, ca_file.Path() + ".missing"));
    CHECK(missing_ca.handshake.raw == ssl::ResultSslErrorUnknownCa.raw);
    CHECK_FALSE(missing_ca.server_finished);
}
