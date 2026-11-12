// deadnation: host threads that may call guest code (AvPlayer allocator callbacks).
// Each thread gets a guest TCB (GS base, TLS) from the C runtime before running.
#pragma once
#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <memory>
#include <mutex>
#include <stop_token>
#include <system_error>
#include <thread>
#include "common/types.h"

extern "C" void runtime_thread_attach_host(const char* name);

namespace Libraries::Kernel {
// AvPlayer joins one thread from several: its decoders call Join() on themselves as they end,
// the demuxer joins the decoders, and the game's Stop joins all three. A self-join used to
// detach, which on Win32 libc++ closes the handle another thread was waiting on (join failed
// with ERROR_INVALID_HANDLE). Now a self-join does nothing (the owner joins later), joins from
// other threads are serialized, and only a thread destroying its own object detaches.
class Thread {
public:
    Thread() = default;
    Thread(const Thread&) = delete;
    Thread& operator=(const Thread&) = delete;
    ~Thread() {
        if (IsSelf()) {
            if (thread.joinable()) thread.detach();
        } else {
            Stop();
        }
    }
    void Run(std::function<void(std::stop_token)>&& func) {
        Stop();
        state = std::make_shared<State>();
        thread = std::thread([func = std::move(func), s = state] {
            s->id.store(std::this_thread::get_id());
            runtime_thread_attach_host("deadnation:hle");
            func(s->stop.get_token());
            s->finished.store(true);
        });
    }
    // Called from guest code, which a C++ exception cannot unwind: a failed join is logged, and
    // the thread detached once its function has returned.
    void Join() {
        if (IsSelf()) return;
        std::scoped_lock lock{mutex};
        if (!thread.joinable()) return;
        try {
            thread.join();
        } catch (const std::system_error& error) {
            std::fprintf(stderr, "HLE thread: join failed (%s); waiting for it to finish\n", error.what());
            for (int waited = 0; !state->finished.load() && waited < 5000; ++waited) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            if (thread.joinable()) thread.detach();
        }
    }
    bool Joinable() const { return thread.joinable(); }
    void Stop() {
        if (state) state->stop.request_stop();
        Join();
    }

private:
    struct State {
        std::atomic<bool> finished{false};
        std::atomic<std::thread::id> id{};
        std::stop_source stop;
    };
    bool IsSelf() const { return state && state->id.load() == std::this_thread::get_id(); }

    std::thread thread;
    std::shared_ptr<State> state;
    std::mutex mutex;
};
} // namespace Libraries::Kernel
