#pragma once

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

#include <cstdio>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>

// 鍵をリポジトリに置かないため、自己署名証明書はテスト実行時に作る。
class TempCert {
  public:
    TempCert() {
        namespace fs = std::filesystem;
        dir_ = fs::temp_directory_path() /
               ("hayate_cert_" + std::to_string(reinterpret_cast<std::uintptr_t>(this)));
        fs::create_directories(dir_);
        cert_ = dir_ / "server.pem";
        key_ = dir_ / "server.key";

        std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> pkey(EVP_EC_gen("prime256v1"),
                                                                 &EVP_PKEY_free);
        if (!pkey) {
            throw std::runtime_error("EVP_EC_gen failed");
        }
        std::unique_ptr<X509, decltype(&X509_free)> x509(X509_new(), &X509_free);
        if (!x509) {
            throw std::runtime_error("X509_new failed");
        }
        X509_set_version(x509.get(), 2);
        ASN1_INTEGER_set(X509_get_serialNumber(x509.get()), 1);
        X509_gmtime_adj(X509_getm_notBefore(x509.get()), 0);
        X509_gmtime_adj(X509_getm_notAfter(x509.get()), 3600);
        X509_set_pubkey(x509.get(), pkey.get());
        auto *name = X509_get_subject_name(x509.get());
        X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                                   reinterpret_cast<const unsigned char *>("localhost"), -1, -1, 0);
        X509_set_issuer_name(x509.get(), name);
        if (X509_sign(x509.get(), pkey.get(), EVP_sha256()) == 0) {
            throw std::runtime_error("X509_sign failed");
        }

        write(key_.string(), [&](std::FILE *f) {
            return PEM_write_PrivateKey(f, pkey.get(), nullptr, nullptr, 0, nullptr, nullptr);
        });
        write(cert_.string(), [&](std::FILE *f) { return PEM_write_X509(f, x509.get()); });
    }

    ~TempCert() {
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
    }

    const std::filesystem::path &cert() const { return cert_; }
    const std::filesystem::path &key() const { return key_; }

  private:
    template <typename F> static void write(const std::string &path, F &&fn) {
        std::unique_ptr<std::FILE, decltype(&std::fclose)> f(std::fopen(path.c_str(), "wb"),
                                                             &std::fclose);
        if (!f || fn(f.get()) == 0) {
            throw std::runtime_error("PEM write failed: " + path);
        }
    }

    std::filesystem::path dir_;
    std::filesystem::path cert_;
    std::filesystem::path key_;
};
