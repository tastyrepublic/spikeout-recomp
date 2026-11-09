// The system's certificate authorities for curl's TLS (the lobby, the update check). A curl built on OpenSSL looks
// where its build machine kept them; a download built on one Linux (the Steam Runtime's) and run on another (Fedora,
// openSUSE, SteamOS) or on macOS may keep them elsewhere: the first of the usual places that exists. Windows: curl
// uses the system's own store (Schannel), nothing to give.
#pragma once

#include <curl/curl.h>

#include <fstream>

namespace net {

inline void set_ca_file(CURL* c) {
#ifndef _WIN32
    static const char* const found = [] {
        for (const char* path : {"/etc/ssl/certs/ca-certificates.crt",                // Debian, Ubuntu, Arch, SteamOS
                                 "/etc/pki/tls/certs/ca-bundle.crt",                  // Fedora, RHEL
                                 "/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem", // Fedora, RHEL (newer)
                                 "/etc/ssl/ca-bundle.pem",                            // openSUSE
                                 "/etc/ssl/cert.pem"})                                // macOS, Alpine
            if (std::ifstream(path)) return path;
        return static_cast<const char*>(nullptr);
    }();
    if (found) curl_easy_setopt(c, CURLOPT_CAINFO, found);
#else
    (void)c;
#endif
}

} // namespace net
