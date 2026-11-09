// A bug report to GitHub: src/net/report.hpp.
#include "net/report.hpp"
#include "net/ca_file.hpp"
#include "net/json.hpp"

#include <cstdlib>
#include <curl/curl.h>

namespace net::report {

namespace {

struct Answer {
    long status = 0;
    std::string text, error;
};

std::size_t keep(char* data, std::size_t size, std::size_t n, void* user) {
    static_cast<std::string*>(user)->append(data, size * n);
    return size * n;
}

// One request: `body` sent if not empty (`type` its Content-Type), `token` as GitHub's Bearer if not empty.
Answer request(const std::string& method, const std::string& url, const std::string& body, const std::string& type,
               const std::string& token) {
    static const bool ready = curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK;
    (void)ready;
    Answer a;
    CURL* c = curl_easy_init();
    if (!c) {
        a.error = "no connection";
        return a;
    }
    curl_slist* headers = curl_slist_append(nullptr, "Accept: application/json");
    if (!type.empty()) headers = curl_slist_append(headers, ("Content-Type: " + type).c_str());
    if (!token.empty()) headers = curl_slist_append(headers, ("Authorization: Bearer " + token).c_str());
    headers = curl_slist_append(headers, "X-GitHub-Api-Version: 2022-11-28");
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, method.c_str());
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "spikeout-recomp");
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 60L);
    curl_easy_setopt(c, CURLOPT_SSL_OPTIONS, static_cast<long>(CURLSSLOPT_REVOKE_BEST_EFFORT | CURLSSLOPT_NATIVE_CA));
    set_ca_file(c);
    if (!body.empty()) {
        curl_easy_setopt(c, CURLOPT_POSTFIELDS, body.data());
        curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(body.size()));
    }
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, keep);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &a.text);
    if (const CURLcode r = curl_easy_perform(c); r != CURLE_OK) a.error = curl_easy_strerror(r);
    else curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &a.status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(c);
    return a;
}

std::string env_or(const char* name, const char* otherwise) { // (tests: a GitHub of their own)
    const char* e = std::getenv(name);
    return e && *e ? e : otherwise;
}
std::string api() { return env_or("SPIKEOUT_GITHUB_API", "https://api.github.com"); }
std::string web() { return env_or("SPIKEOUT_GITHUB_WEB", "https://github.com"); }

std::string answer_error(const Answer& a, const char* who) {
    if (!a.error.empty()) return "no connection to " + std::string(who) + " (" + a.error + ")";
    if (const auto j = Json::parse(a.text); j && j->is_object()) {
        if (const std::string& m = (*j)["message"].as_string(); !m.empty()) return std::string(who) + ": " + m; // (GitHub's)
    }
    return std::string(who) + " answered " + std::to_string(a.status);
}

} // namespace

bool available() { return true; }
bool can_log_in() { return kClientId[0] != '\0' || std::getenv("SPIKEOUT_GITHUB_CLIENT_ID"); }

DeviceCode login_start() {
    DeviceCode d;
    const std::string id = env_or("SPIKEOUT_GITHUB_CLIENT_ID", kClientId);
    const Answer a = request("POST", web() + "/login/device/code", "client_id=" + id, "application/x-www-form-urlencoded", {});
    const auto j = Json::parse(a.text);
    if (a.error.empty() && a.status == 200 && j && !(*j)["device_code"].as_string().empty()) {
        d.device_code = (*j)["device_code"].as_string();
        d.user_code = (*j)["user_code"].as_string();
        d.verification_uri = (*j)["verification_uri"].as_string();
        d.interval = static_cast<int>((*j)["interval"].as_number(5));
        d.expires_in = static_cast<int>((*j)["expires_in"].as_number(900));
    } else {
        d.error = answer_error(a, "GitHub");
    }
    return d;
}

Poll login_poll(const std::string& device_code) {
    Poll p;
    const std::string id = env_or("SPIKEOUT_GITHUB_CLIENT_ID", kClientId);
    const Answer a = request("POST", web() + "/login/oauth/access_token",
                             "client_id=" + id + "&device_code=" + device_code +
                                 "&grant_type=urn:ietf:params:oauth:grant-type:device_code",
                             "application/x-www-form-urlencoded", {});
    const auto j = Json::parse(a.text);
    if (!a.error.empty() || !j || !j->is_object()) {
        p.error = answer_error(a, "GitHub");
        return p;
    }
    if (const std::string& token = (*j)["access_token"].as_string(); !token.empty()) {
        p.state = Poll::State::done, p.token = token;
        return p;
    }
    const std::string& e = (*j)["error"].as_string();
    p.state = e == "authorization_pending" ? Poll::State::pending
            : e == "slow_down"             ? Poll::State::slow_down
            : e == "access_denied"         ? Poll::State::denied
            : e == "expired_token"         ? Poll::State::expired
                                           : Poll::State::failed;
    if (p.state == Poll::State::failed) p.error = "GitHub: " + ((*j)["error_description"].as_string().empty() ? e : (*j)["error_description"].as_string());
    return p;
}

std::string login_name(const std::string& token, std::string* error) {
    const Answer a = request("GET", api() + "/user", {}, {}, token);
    const auto j = Json::parse(a.text);
    if (a.error.empty() && a.status == 200 && j) return (*j)["login"].as_string();
    if (error) *error = answer_error(a, "GitHub");
    return {};
}

Sent send_as_user(const Report& r, const std::string& token) {
    Sent s;
    Json issue = Json::object();
    issue.set("title", r.title);
    issue.set("body", r.body);
    const Answer a = request("POST", api() + "/repos/" + env_or("SPIKEOUT_REPORT_REPO", kRepo) + "/issues", issue.dump(),
                             "application/json", token);
    const auto j = Json::parse(a.text);
    if (!a.error.empty() || a.status != 201 || !j || (*j)["number"].as_number() <= 0) {
        s.error = a.status == 401 ? "your GitHub login has run out: log in again" : answer_error(a, "GitHub");
        return s;
    }
    s.number = static_cast<int>((*j)["number"].as_number());
    s.url = (*j)["html_url"].as_string();
    s.ok = true;
    for (const std::string& c : r.comments) { // (the logs; a failed one leaves the issue, said in the error)
        Json comment = Json::object();
        comment.set("body", c);
        const Answer ca = request("POST", api() + "/repos/" + env_or("SPIKEOUT_REPORT_REPO", kRepo) + "/issues/" +
                                              std::to_string(s.number) + "/comments",
                                  comment.dump(), "application/json", token);
        if (!ca.error.empty() || ca.status != 201) s.error = "the issue was opened, but a log couldn't be added: " + answer_error(ca, "GitHub");
    }
    return s;
}

} // namespace net::report
