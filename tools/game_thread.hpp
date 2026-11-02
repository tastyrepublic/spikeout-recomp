// A board's thread: it keeps the whole board (m3::Bus, its context) on its
// stack, more than the 512 KB macOS gives a thread (Linux gives 8 MB). On
// macOS the thread is made with a stack of its own size; elsewhere it is a
// std::thread.
#pragma once

#include <thread>

#if defined(__APPLE__)
#include <pthread.h>

#include <cstddef>
#include <exception>
#include <functional>
#include <memory>
#include <system_error>
#include <utility>
#endif

namespace tools {

#if defined(__APPLE__)
class GameThread {
public:
    static constexpr std::size_t kStackSize = std::size_t{64} << 20;

    GameThread() = default;
    template <typename F>
    explicit GameThread(F&& f) {
        auto body = std::make_unique<std::function<void()>>(std::forward<F>(f));
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, kStackSize);
        const int error = pthread_create(&thread_, &attr, &GameThread::start, body.get());
        pthread_attr_destroy(&attr);
        if (error != 0) throw std::system_error(error, std::generic_category(), "pthread_create");
        body.release(); // (the thread's now)
        joinable_ = true;
    }
    GameThread(GameThread&& o) noexcept : thread_(o.thread_), joinable_(o.joinable_) { o.joinable_ = false; }
    GameThread& operator=(GameThread&& o) noexcept {
        if (joinable_) std::terminate(); // (as std::thread)
        thread_ = o.thread_, joinable_ = o.joinable_, o.joinable_ = false;
        return *this;
    }
    GameThread(const GameThread&) = delete;
    GameThread& operator=(const GameThread&) = delete;
    ~GameThread() {
        if (joinable_) std::terminate(); // (as std::thread)
    }

    bool joinable() const { return joinable_; }
    void join() {
        pthread_join(thread_, nullptr);
        joinable_ = false;
    }

private:
    static void* start(void* arg) {
        std::unique_ptr<std::function<void()>> body(static_cast<std::function<void()>*>(arg));
        (*body)();
        return nullptr;
    }

    pthread_t thread_{};
    bool joinable_ = false;
};
#else
using GameThread = std::thread;
#endif

} // namespace tools
