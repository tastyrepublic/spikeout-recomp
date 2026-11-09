// The update check (src/net/update.hpp): GitHub's release list over HTTPS
// through libcurl.
#include "net/update.hpp"
#include "net/ca_file.hpp"

#include <cstdlib>
#include <curl/curl.h>

namespace net {

// The version, findable in the program file (tools/make-release.ps1 checks
// that a download carries the version it is named for).
extern const char kVersionMark[];
const char kVersionMark[] = "SPIKEOUT_VERSION=" SPIKEOUT_VERSION;

namespace {

size_t keep(char* data, size_t size, size_t count, void* out) {
    auto* text = static_cast<std::string*>(out);
    if (text->size() > (4u << 20)) return 0; // (a list that big isn't GitHub's)
    text->append(data, size * count);
    return size * count;
}

} // namespace

bool UpdateCheck::available() { return true; }

UpdateCheck::~UpdateCheck() = default; // (the thread was detached: it holds its own share of the state)

void UpdateCheck::start(std::string current, std::string url) {
    if (started_) return;
    started_ = true;
    static const bool ready = curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK; // (never cleaned up: a question may outlive main)
    (void)ready;
    if (const char* e = std::getenv("SPIKEOUT_UPDATE_URL"); e && *e) url = e;
    if (url.empty()) url = SPIKEOUT_RELEASES_URL;
    std::thread(run, state_, std::move(current), std::move(url)).detach();
}

std::optional<Release> UpdateCheck::newer() const {
    std::lock_guard guard(state_->lock);
    return state_->newer;
}

std::string UpdateCheck::error() const {
    std::lock_guard guard(state_->lock);
    return state_->error;
}

void UpdateCheck::run(std::shared_ptr<State> state, std::string current, std::string url) {
    std::string text, error;
    std::optional<Release> found;
    if (CURL* c = curl_easy_init()) {
        curl_slist* headers = curl_slist_append(nullptr, "Accept: application/vnd.github+json");
        curl_easy_setopt(c, CURLOPT_URL, url.c_str());
        curl_easy_setopt(c, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(c, CURLOPT_USERAGENT, "spikeout-recomp"); // (GitHub wants one)
        curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 10L);
        curl_easy_setopt(c, CURLOPT_TIMEOUT, 20L);
        curl_easy_setopt(c, CURLOPT_SSL_OPTIONS, static_cast<long>(CURLSSLOPT_REVOKE_BEST_EFFORT | CURLSSLOPT_NATIVE_CA));
        set_ca_file(c);
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, keep);
        curl_easy_setopt(c, CURLOPT_WRITEDATA, &text);
        long status = 0;
        if (const CURLcode r = curl_easy_perform(c); r != CURLE_OK) error = curl_easy_strerror(r);
        else if (curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status), status != 200) error = "GitHub answered " + std::to_string(status);
        else if (const auto list = Json::parse(text); !list || !list->is_array()) error = "GitHub's answer couldn't be read";
        else found = newer_release(*list, current);
        curl_slist_free_all(headers);
        curl_easy_cleanup(c);
    } else {
        error = "no connection";
    }
    {
        std::lock_guard guard(state->lock);
        state->newer = std::move(found), state->error = std::move(error);
    }
    state->done = true;
}

} // namespace net
