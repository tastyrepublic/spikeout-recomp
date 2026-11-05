#include "net/lobby.hpp"

#include <chrono>
#include <curl/curl.h>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <poll.h>
#endif

namespace net {

namespace {

constexpr auto kKeepAlive = std::chrono::seconds(30);

struct CurlGlobal {
    CurlGlobal() { curl_global_init(CURL_GLOBAL_DEFAULT); }
    ~CurlGlobal() { curl_global_cleanup(); }
};

// Until the socket has something to read, or `ms` milliseconds.
void wait_readable(curl_socket_t s, int ms) {
#ifdef _WIN32
    WSAPOLLFD p{s, POLLRDNORM, 0};
    WSAPoll(&p, 1, ms);
#else
    pollfd p{s, POLLIN, 0};
    ::poll(&p, 1, ms);
#endif
}

} // namespace

bool LobbyClient::available() { return true; }

LobbyClient::LobbyClient() { static CurlGlobal global; }

LobbyClient::~LobbyClient() {
    stop_ = true;
    if (thread_.joinable()) thread_.join();
}

std::string LobbyClient::error() const {
    std::lock_guard guard(lock_);
    return error_;
}

void LobbyClient::connect(const std::string& build, const std::string& name, const std::string& url) {
    if (thread_.joinable()) return;
    Json hello = Json::object();
    hello.set("t", "hello"), hello.set("build", build), hello.set("name", name), hello.set("version", SPIKEOUT_VERSION);
    thread_ = std::thread([this, url, h = hello.dump()] { run(url, h); });
}

void LobbyClient::send(const Json& message) {
    std::lock_guard guard(lock_);
    out_.push_back(message.dump());
}

std::optional<Json> LobbyClient::next(int ms) {
    std::unique_lock guard(lock_);
    changed_.wait_for(guard, std::chrono::milliseconds(ms), [&] { return !in_.empty() || failed_; });
    if (in_.empty()) return std::nullopt;
    Json m = std::move(in_.front());
    in_.pop_front();
    return m;
}

std::optional<Json> LobbyClient::wait_for(const std::string& t, int ms) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    std::unique_lock guard(lock_);
    for (;;) {
        for (auto it = in_.begin(); it != in_.end(); ++it)
            if ((*it)["t"].as_string() == t || (*it)["t"].as_string() == "error") {
                Json m = std::move(*it);
                in_.erase(it);
                return m;
            }
        if (failed_ || changed_.wait_until(guard, until) == std::cv_status::timeout) return std::nullopt;
    }
}

void LobbyClient::run(std::string url, std::string hello) {
    auto fail = [this](const std::string& why) {
        {
            std::lock_guard guard(lock_);
            error_ = why;
        }
        connected_ = false;
        failed_ = true;
        changed_.notify_all();
    };
    CURL* c = curl_easy_init();
    if (!c) return fail("can't start libcurl");
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_CONNECT_ONLY, 2L); // (a WebSocket, sent and received by hand)
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(c, CURLOPT_SSL_OPTIONS, static_cast<long>(CURLSSLOPT_REVOKE_BEST_EFFORT | CURLSSLOPT_NATIVE_CA));
    curl_easy_setopt(c, CURLOPT_USERAGENT, "spikeout-recomp");
    if (const CURLcode r = curl_easy_perform(c); r != CURLE_OK) {
        curl_easy_cleanup(c);
        return fail(std::string("the lobby: ") + curl_easy_strerror(r));
    }
    curl_socket_t socket = CURL_SOCKET_BAD;
    curl_easy_getinfo(c, CURLINFO_ACTIVESOCKET, &socket);
    connected_ = true;
    {
        std::lock_guard guard(lock_);
        out_.push_front(hello);
    }
    auto last_sent = std::chrono::steady_clock::now();
    std::string frame;
    std::vector<char> buffer(16384);
    while (!stop_) {
        // Out: what's queued (and now and then a ping, answered by the lobby itself).
        std::deque<std::string> out;
        {
            std::lock_guard guard(lock_);
            out.swap(out_);
        }
        if (out.empty() && std::chrono::steady_clock::now() - last_sent > kKeepAlive) out.push_back("ping");
        for (const std::string& m : out) {
            std::size_t sent = 0;
            const CURLcode r = curl_ws_send(c, m.data(), m.size(), &sent, 0, CURLWS_TEXT);
            if (r != CURLE_OK && r != CURLE_AGAIN) {
                curl_easy_cleanup(c);
                return fail(std::string("the lobby: ") + curl_easy_strerror(r));
            }
            last_sent = std::chrono::steady_clock::now();
        }
        // In: whole text frames, as JSON.
        for (;;) {
            std::size_t got = 0;
            const struct curl_ws_frame* meta = nullptr;
            const CURLcode r = curl_ws_recv(c, buffer.data(), buffer.size(), &got, &meta);
            if (r == CURLE_AGAIN) break;
            if (r != CURLE_OK) {
                curl_easy_cleanup(c);
                return fail(std::string("the lobby: ") + curl_easy_strerror(r));
            }
            if (meta && (meta->flags & CURLWS_CLOSE)) {
                curl_easy_cleanup(c);
                return fail("the lobby closed the connection");
            }
            frame.append(buffer.data(), got);
            if (!meta || meta->bytesleft > 0 || (meta->flags & CURLWS_CONT)) continue;
            if (meta->flags & CURLWS_TEXT) {
                if (frame == "pong") {
                } else if (auto j = Json::parse(frame)) {
                    std::lock_guard guard(lock_);
                    in_.push_back(std::move(*j));
                    changed_.notify_all();
                }
            }
            frame.clear();
        }
        wait_readable(socket, 20);
    }
    std::size_t ignored = 0;
    curl_ws_send(c, "", 0, &ignored, 0, CURLWS_CLOSE);
    curl_easy_cleanup(c);
    connected_ = false;
}

} // namespace net
