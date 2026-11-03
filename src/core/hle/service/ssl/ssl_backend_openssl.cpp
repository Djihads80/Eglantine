// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <mutex>
#include <string>
#include <string_view>
#include <utility>

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include "common/fs/file.h"
#include "common/hex_util.h"
#include "common/string_util.h"

#include "core/hle/service/ssl/ssl_backend.h"
#include "core/internal_network/network.h"
#include "core/internal_network/sockets.h"

using namespace Common::FS;

namespace Service::SSL {

// Import OpenSSL's `SSL` type into the namespace.  This is needed because the
// namespace is also named `SSL`.
using ::SSL;

namespace {

std::once_flag one_time_init_flag;
bool one_time_init_success = false;

SSL_CTX* ssl_ctx;
IOFile key_log_file; // only open if SSLKEYLOGFILE set in environment
BIO_METHOD* bio_meth;

Result CheckOpenSSLErrors();
void OneTimeInit();
void OneTimeInitLogFile();
bool OneTimeInitBIO();

} // namespace

class SSLTrustStore {
public:
    ~SSLTrustStore() {
        X509_STORE_free(store);
        for (X509* const certificate : certificates) {
            X509_free(certificate);
        }
    }

    X509_STORE* store = nullptr;
    /// The certificates in store, which a store made from this one starts with
    std::vector<X509*> certificates;
};

class SSLConnectionBackendOpenSSL final : public SSLConnectionBackend {
public:
    Result Init() {
        std::call_once(one_time_init_flag, OneTimeInit);

        if (!one_time_init_success) {
            LOG_ERROR(Service_SSL,
                      "Can't create SSL connection because OpenSSL one-time initialization failed");
            return ResultInternalError;
        }

        ssl = SSL_new(ssl_ctx);
        if (!ssl) {
            LOG_ERROR(Service_SSL, "SSL_new failed");
            return CheckOpenSSLErrors();
        }

        SSL_set_app_data(ssl, this); // for VerifyCallback
        SSL_set_connect_state(ssl);

        bio = BIO_new(bio_meth);
        if (!bio) {
            LOG_ERROR(Service_SSL, "BIO_new failed");
            return CheckOpenSSLErrors();
        }

        BIO_set_data(bio, this);
        BIO_set_init(bio, 1);
        SSL_set_bio(ssl, bio, bio);

        return ResultSuccess;
    }

    void SetSocket(std::shared_ptr<Network::SocketBase> socket_in) override {
        socket = std::move(socket_in);
    }

    Result SetHostName(const char* hostname_in) override {
        hostname = hostname_in; // checked at the handshake if VerifyOption::HostName is set
        if (!SSL_set_tlsext_host_name(ssl, hostname_in)) { // hostname for SNI
            LOG_ERROR(Service_SSL, "SSL_set_tlsext_host_name({}) failed", hostname_in);
            return CheckOpenSSLErrors();
        }
        R_SUCCEED();
    }

    Result GetHostName(std::span<u8> data, u32* out_size) override {
        auto const peer_name = SSL_get0_peername(ssl);
        if (peer_name == nullptr) {
            LOG_ERROR(Service_SSL, "SSL_get0_peername()");
            return CheckOpenSSLErrors();
        }
        auto const s = std::string{peer_name};
        *out_size = u32(s.size());
        std::memcpy(data.data(), s.data(), (std::min)(s.size(), data.size()));
        R_SUCCEED();
    }

    void SetVerifyOption(VerifyOption option) override {
        verify_option = option;
    }

    void SetTrustStore(std::shared_ptr<const SSLTrustStore> store) override {
        trust_store = std::move(store);
    }

    Result DoHandshake() override {
        if (!handshake_started) {
            R_TRY(ConfigureVerification());
            handshake_started = true;
        }
        const int ret = SSL_do_handshake(ssl);

        if (verify_error != X509_V_OK) {
            LOG_ERROR(Service_SSL, "SSL cert verification failed because: {}",
                      X509_verify_cert_error_string(verify_error));
            CheckOpenSSLErrors();
            return VerifyCertErrorResult(verify_error);
        }

        if (ret <= 0) {
            const int ssl_err = SSL_get_error(ssl, ret);
            if (ssl_err == SSL_ERROR_ZERO_RETURN ||
                (ssl_err == SSL_ERROR_SYSCALL && got_read_eof)) {
                LOG_ERROR(Service_SSL, "SSL handshake failed because server hung up");
                return ResultInternalError;
            }
        }
        return HandleReturn("SSL_do_handshake", 0, ret);
    }

    Result GetVerifyCertError() override {
        const long error = std::exchange(verify_error, X509_V_OK);
        if (error == X509_V_OK) {
            R_SUCCEED();
        }
        return VerifyCertErrorResult(error);
    }

    // Sets the handshake to check what verify_option asks for, against trust_store. The chain
    // is always built so that the host name and dates can be checked without VerifyOption::PeerCa,
    // whose failures VerifyCallback then lets pass.
    Result ConfigureVerification() {
        constexpr auto extended_validation = VerifyOption::EvCertPartial |
                                             VerifyOption::EvPolicyOid |
                                             VerifyOption::EvCertFingerprint;
        if (True(verify_option & extended_validation)) {
            LOG_ERROR(Service_SSL, "Extended validation (verify option {:#x}) is not implemented",
                      static_cast<u32>(verify_option));
            return ResultInternalError;
        }
        if (True(verify_option & VerifyOption::HostName)) {
            if (hostname.empty()) {
                LOG_ERROR(Service_SSL, "Host name verification was asked for without a host name");
                return ResultInternalError;
            }
            if (!SSL_set1_host(ssl, hostname.c_str())) {
                LOG_ERROR(Service_SSL, "SSL_set1_host({}) failed", hostname);
                return CheckOpenSSLErrors();
            }
        }
        // Any trusted certificate ends a chain: server certificates may be trusted directly.
        unsigned long flags = X509_V_FLAG_PARTIAL_CHAIN;
        if (False(verify_option & VerifyOption::DateCheck)) {
            flags |= X509_V_FLAG_NO_CHECK_TIME;
        }
        X509_VERIFY_PARAM_set_flags(SSL_get0_param(ssl), flags);
        if (trust_store) {
            if (!SSL_set1_verify_cert_store(ssl, trust_store->store)) {
                LOG_ERROR(Service_SSL, "SSL_set1_verify_cert_store failed");
                return CheckOpenSSLErrors();
            }
        }
        SSL_set_verify(ssl, SSL_VERIFY_PEER, &VerifyCallback);
        R_SUCCEED();
    }

    // Fails the handshake on the first failure of a check that verify_option asks for.
    static int VerifyCallback(int preverify_ok, X509_STORE_CTX* store_ctx) {
        if (preverify_ok) {
            return 1;
        }
        auto* const ssl = static_cast<SSL*>(
            X509_STORE_CTX_get_ex_data(store_ctx, SSL_get_ex_data_X509_STORE_CTX_idx()));
        auto* const self = static_cast<SSLConnectionBackendOpenSSL*>(SSL_get_app_data(ssl));
        const int error = X509_STORE_CTX_get_error(store_ctx);
        // The host name is only set with VerifyOption::HostName and the dates only checked with
        // DateCheck. Anything else fails the chain, which only PeerCa asks to be trusted.
        const bool chain_error = !IsHostNameError(error) && !IsDateError(error);
        if (chain_error && False(self->verify_option & VerifyOption::PeerCa)) {
            return 1;
        }
        if (self->verify_error == X509_V_OK) {
            self->verify_error = error;
        }
        return 0;
    }

    static bool IsHostNameError(long error) {
        return error == X509_V_ERR_HOSTNAME_MISMATCH || error == X509_V_ERR_IP_ADDRESS_MISMATCH;
    }

    static bool IsDateError(long error) {
        return error == X509_V_ERR_CERT_NOT_YET_VALID || error == X509_V_ERR_CERT_HAS_EXPIRED ||
               error == X509_V_ERR_ERROR_IN_CERT_NOT_BEFORE_FIELD ||
               error == X509_V_ERR_ERROR_IN_CERT_NOT_AFTER_FIELD;
    }

    static Result VerifyCertErrorResult(long error) {
        if (IsHostNameError(error)) {
            return ResultSslErrorInvalidCertDomain;
        }
        if (IsDateError(error)) {
            return ResultSslErrorExpiredCertificate;
        }
        switch (error) {
        case X509_V_ERR_CERT_REVOKED:
            return ResultSslErrorRevokedCertificate;
        case X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT:
        case X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT_LOCALLY:
        case X509_V_ERR_UNABLE_TO_VERIFY_LEAF_SIGNATURE:
        case X509_V_ERR_DEPTH_ZERO_SELF_SIGNED_CERT:
        case X509_V_ERR_SELF_SIGNED_CERT_IN_CHAIN:
        case X509_V_ERR_CERT_UNTRUSTED:
            return ResultSslErrorUnknownCa;
        default:
            return ResultSslErrorBadCertificate;
        }
    }

    Result Read(size_t* out_size, std::span<u8> data) override {
        const int ret = SSL_read_ex(ssl, data.data(), data.size(), out_size);
        return HandleReturn("SSL_read_ex", out_size, ret);
    }

    Result Peek(size_t* out_size, std::span<u8> data) override {
        auto const n = (std::min)(data.size(), *out_size);
        const int ret = SSL_peek(ssl, data.data(), int(n));
        *out_size = n;
        return HandleReturn("SSL_write_ex", out_size, ret);
    }

    Result Write(size_t* out_size, std::span<const u8> data) override {
        const int ret = SSL_write_ex(ssl, data.data(), data.size(), out_size);
        return HandleReturn("SSL_write_ex", out_size, ret);
    }

    Result HandleReturn(const char* what, size_t* actual, int ret) {
        const int ssl_err = SSL_get_error(ssl, ret);
        CheckOpenSSLErrors();
        switch (ssl_err) {
        case SSL_ERROR_NONE:
            return ResultSuccess;
        case SSL_ERROR_ZERO_RETURN:
            LOG_DEBUG(Service_SSL, "{} => SSL_ERROR_ZERO_RETURN", what);
            // DoHandshake special-cases this, but for Read and Write:
            *actual = 0;
            return ResultSuccess;
        case SSL_ERROR_WANT_READ:
            LOG_DEBUG(Service_SSL, "{} => SSL_ERROR_WANT_READ", what);
            return ResultWouldBlock;
        case SSL_ERROR_WANT_WRITE:
            LOG_DEBUG(Service_SSL, "{} => SSL_ERROR_WANT_WRITE", what);
            return ResultWouldBlock;
        default:
            if (ssl_err == SSL_ERROR_SYSCALL && got_read_eof) {
                LOG_DEBUG(Service_SSL, "{} => SSL_ERROR_SYSCALL because server hung up", what);
                *actual = 0;
                return ResultSuccess;
            }
            LOG_ERROR(Service_SSL, "{} => other SSL_get_error return value {}", what, ssl_err);
            return ResultInternalError;
        }
    }

    Result GetServerCerts(std::vector<std::vector<u8>>* out_certs) override {
        STACK_OF(X509)* chain = SSL_get_peer_cert_chain(ssl);
        if (!chain) {
            LOG_ERROR(Service_SSL, "SSL_get_peer_cert_chain returned nullptr");
            return ResultInternalError;
        }
        int count = sk_X509_num(chain);
        ASSERT(count >= 0);
        for (int i = 0; i < count; i++) {
            X509* x509 = sk_X509_value(chain, i);
            ASSERT_OR_EXECUTE(x509 != nullptr, { continue; });
            unsigned char* buf = nullptr;
            int len = i2d_X509(x509, &buf);
            ASSERT_OR_EXECUTE(len >= 0 && buf, { continue; });
            out_certs->emplace_back(buf, buf + len);
            OPENSSL_free(buf);
        }
        R_SUCCEED();
    }

    int Pending() override {
        return SSL_pending(ssl);
    }

    Result SetRenegotiationMode(u32 mode) override {
        if (mode == 0) {
            SSL_CTX_set_options(ssl_ctx, SSL_OP_NO_RENEGOTIATION);
        } else {
            SSL_CTX_set_options(ssl_ctx, SSL_OP_ALLOW_CLIENT_RENEGOTIATION);
        }
        R_SUCCEED();
    }

    Result GetRenegotiationMode(u32* mode) override {
        *mode = SSL_get_secure_renegotiation_support(ssl) ? 1 : 0;
        R_SUCCEED();
    }

    ~SSLConnectionBackendOpenSSL() {
        // this is null-tolerant:
        SSL_free(ssl);
    }

    static void KeyLogCallback(const SSL* ssl, const char* line) {
        std::string str(line);
        str.push_back('\n');
        // Do this in a single WriteString for atomicity if multiple instances
        // are running on different threads (though that can't currently
        // happen).
        if (key_log_file.WriteString(str) != str.size() || !key_log_file.Flush()) {
            LOG_CRITICAL(Service_SSL, "Failed to write to SSLKEYLOGFILE");
        }
        LOG_DEBUG(Service_SSL, "Wrote to SSLKEYLOGFILE: {}", line);
    }

    static int WriteCallback(BIO* bio, const char* buf, size_t len, size_t* actual_p) {
        auto self = static_cast<SSLConnectionBackendOpenSSL*>(BIO_get_data(bio));
        ASSERT_OR_EXECUTE_MSG(
            self->socket, { return 0; }, "OpenSSL asked to send but we have no socket");
        BIO_clear_retry_flags(bio);
        auto [actual, err] = self->socket->Send({reinterpret_cast<const u8*>(buf), len}, 0);
        switch (err) {
        case Network::Errno::SUCCESS:
            *actual_p = actual;
            return 1;
        case Network::Errno::AGAIN:
            BIO_set_flags(bio, BIO_FLAGS_WRITE | BIO_FLAGS_SHOULD_RETRY);
            return 0;
        default:
            LOG_ERROR(Service_SSL, "Socket send returned Network::Errno {}", err);
            return -1;
        }
    }

    static int ReadCallback(BIO* bio, char* buf, size_t len, size_t* actual_p) {
        auto self = static_cast<SSLConnectionBackendOpenSSL*>(BIO_get_data(bio));
        ASSERT_OR_EXECUTE_MSG(
            self->socket, { return 0; }, "OpenSSL asked to recv but we have no socket");
        BIO_clear_retry_flags(bio);
        auto [actual, err] = self->socket->Recv(0, {reinterpret_cast<u8*>(buf), len});
        switch (err) {
        case Network::Errno::SUCCESS:
            *actual_p = actual;
            if (actual == 0) {
                self->got_read_eof = true;
            }
            return actual ? 1 : 0;
        case Network::Errno::AGAIN:
            BIO_set_flags(bio, BIO_FLAGS_READ | BIO_FLAGS_SHOULD_RETRY);
            return 0;
        default:
            LOG_ERROR(Service_SSL, "Socket recv returned Network::Errno {}", err);
            return -1;
        }
    }

    static long CtrlCallback(BIO* bio, int cmd, long l_arg, void* p_arg) {
        switch (cmd) {
        case BIO_CTRL_FLUSH:
            // Nothing to flush.
            return 1;
        case BIO_CTRL_PUSH:
        case BIO_CTRL_POP:
#ifdef BIO_CTRL_GET_KTLS_SEND
        case BIO_CTRL_GET_KTLS_SEND:
        case BIO_CTRL_GET_KTLS_RECV:
#endif
            // We don't support these operations, but don't bother logging them
            // as they're nothing unusual.
            return 0;
        default:
            LOG_DEBUG(Service_SSL, "OpenSSL BIO got ctrl({}, {}, {})", cmd, l_arg, p_arg);
            return 0;
        }
    }

    SSL* ssl = nullptr;
    BIO* bio = nullptr;
    bool got_read_eof = false;
    bool handshake_started = false;
    VerifyOption verify_option = VerifyOption::None;
    std::shared_ptr<const SSLTrustStore> trust_store;
    std::string hostname;
    long verify_error = X509_V_OK; ///< The first check of verify_option that failed

    std::shared_ptr<Network::SocketBase> socket;
};

Result CreateSSLConnectionBackend(std::unique_ptr<SSLConnectionBackend>* out_backend) {
    auto conn = std::make_unique<SSLConnectionBackendOpenSSL>();

    R_TRY(conn->Init());

    *out_backend = std::move(conn);
    return ResultSuccess;
}

Result CreateSSLTrustStore(const SSLTrustStore* base,
                           std::span<const std::span<const u8>> der_certificates,
                           std::shared_ptr<const SSLTrustStore>* out_store) {
    auto trust_store = std::make_shared<SSLTrustStore>();
    trust_store->store = X509_STORE_new();
    if (!trust_store->store) {
        LOG_ERROR(Service_SSL, "X509_STORE_new failed");
        return CheckOpenSSLErrors();
    }
    // Takes the reference to certificate. A certificate the store fails to take, as one already
    // in it, is trusted all the same or not at all.
    const auto add = [&trust_store](X509* certificate) {
        trust_store->certificates.push_back(certificate);
        if (!X509_STORE_add_cert(trust_store->store, certificate)) {
            ERR_clear_error();
        }
    };
    if (base) {
        for (X509* const certificate : base->certificates) {
            X509_up_ref(certificate);
            add(certificate);
        }
    }
    for (const std::span<const u8> der : der_certificates) {
        const unsigned char* next = der.data();
        X509* const certificate = d2i_X509(nullptr, &next, static_cast<long>(der.size()));
        if (!certificate) {
            LOG_ERROR(Service_SSL, "Not trusting a certificate that does not decode");
            CheckOpenSSLErrors();
            continue;
        }
        add(certificate);
    }
    *out_store = std::move(trust_store);
    R_SUCCEED();
}

Result DecodeServerPki(std::span<const u8> data, std::vector<std::vector<u8>>* out_certificates) {
    std::vector<std::vector<u8>> certificates;
    const std::string_view text{reinterpret_cast<const char*>(data.data()), data.size()};
    if (text.find("-----BEGIN") != std::string_view::npos) {
        BIO* const bio = BIO_new_mem_buf(data.data(), static_cast<int>(data.size()));
        if (!bio) {
            LOG_ERROR(Service_SSL, "BIO_new_mem_buf failed");
            return CheckOpenSSLErrors();
        }
        ERR_clear_error(); // so that the last error is the one that ended the reading
        while (X509* const certificate = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr)) {
            const int size = i2d_X509(certificate, nullptr);
            if (size > 0) {
                auto& der = certificates.emplace_back(static_cast<size_t>(size));
                unsigned char* out = der.data();
                i2d_X509(certificate, &out);
            }
            X509_free(certificate);
        }
        BIO_free(bio);
        // Reading ends at the end of the buffer, where no PEM block starts; anything else failed.
        const unsigned long error = ERR_peek_last_error();
        if (ERR_GET_LIB(error) != ERR_LIB_PEM || ERR_GET_REASON(error) != PEM_R_NO_START_LINE) {
            LOG_ERROR(Service_SSL, "Server PKI holds a malformed PEM certificate");
            return CheckOpenSSLErrors();
        }
        ERR_clear_error();
    } else {
        const unsigned char* next = data.data();
        const unsigned char* const end = next + data.size();
        while (next < end) {
            const unsigned char* const start = next;
            X509* const certificate = d2i_X509(nullptr, &next, static_cast<long>(end - next));
            if (!certificate) {
                LOG_ERROR(Service_SSL, "Server PKI holds a malformed DER certificate");
                return CheckOpenSSLErrors();
            }
            X509_free(certificate);
            certificates.emplace_back(start, next);
        }
    }
    if (certificates.empty()) {
        LOG_ERROR(Service_SSL, "Server PKI holds no certificate");
        return ResultInternalError;
    }
    *out_certificates = std::move(certificates);
    R_SUCCEED();
}

namespace {

Result CheckOpenSSLErrors() {
    unsigned long rc;
    const char* file;
    int line;
    const char* func;
    const char* data;
    int flags;
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
    while ((rc = ERR_get_error_all(&file, &line, &func, &data, &flags)))
#else
    // Can't get function names from OpenSSL on this version, so use mine:
    func = __func__;
    while ((rc = ERR_get_error_line_data(&file, &line, &data, &flags)))
#endif
    {
        std::string msg;
        msg.resize(1024, '\0');
        ERR_error_string_n(rc, msg.data(), msg.size());
        msg.resize(strlen(msg.data()), '\0');
        if (flags & ERR_TXT_STRING) {
            msg.append(" | ");
            msg.append(data);
        }
        Common::Log::FmtLogMessage(Common::Log::Class::Service_SSL, Common::Log::Level::Error,
                                   file, line, func, "OpenSSL: {}",
                                   msg);
    }
    return ResultInternalError;
}

void OneTimeInit() {
    ssl_ctx = SSL_CTX_new(TLS_client_method());
    if (!ssl_ctx) {
        LOG_ERROR(Service_SSL, "SSL_CTX_new failed");
        CheckOpenSSLErrors();
        return;
    }

    // Connections trust only the certificates the console would, which each sets with
    // SSL_set1_verify_cert_store, and never the host's: the SSL_CTX's own store stays empty.

    OneTimeInitLogFile();

    if (!OneTimeInitBIO()) {
        return;
    }

    one_time_init_success = true;
}

void OneTimeInitLogFile() {
    const char* logfile = getenv("SSLKEYLOGFILE");
    if (logfile) {
        key_log_file.Open(logfile, FileAccessMode::Append, FileType::TextFile,
                          FileShareFlag::ShareWriteOnly);
        if (key_log_file.IsOpen()) {
            SSL_CTX_set_keylog_callback(ssl_ctx, &SSLConnectionBackendOpenSSL::KeyLogCallback);
        } else {
            LOG_CRITICAL(Service_SSL,
                         "SSLKEYLOGFILE was set but file could not be opened; not logging keys!");
        }
    }
}

bool OneTimeInitBIO() {
    bio_meth =
        BIO_meth_new(BIO_get_new_index() | BIO_TYPE_SOURCE_SINK, "SSLConnectionBackendOpenSSL");
    if (!bio_meth ||
        !BIO_meth_set_write_ex(bio_meth, &SSLConnectionBackendOpenSSL::WriteCallback) ||
        !BIO_meth_set_read_ex(bio_meth, &SSLConnectionBackendOpenSSL::ReadCallback) ||
        !BIO_meth_set_ctrl(bio_meth, &SSLConnectionBackendOpenSSL::CtrlCallback)) {
        LOG_ERROR(Service_SSL, "Failed to create BIO_METHOD");
        return false;
    }
    return true;
}

} // namespace

} // namespace Service::SSL
