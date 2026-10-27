// Run by CMake (CMakeLists.txt, the lobby): exits 0 only if this libcurl
// speaks wss (WebSockets over TLS). A curl built without WebSockets still
// declares curl_ws_send; it then answers "Unsupported protocol" when used.
#include <curl/curl.h>

#include <cstring>

int main() {
    const curl_version_info_data* v = curl_version_info(CURLVERSION_NOW);
    for (const char* const* p = v ? v->protocols : nullptr; p && *p; ++p)
        if (std::strcmp(*p, "wss") == 0) return 0;
    return 1;
}
