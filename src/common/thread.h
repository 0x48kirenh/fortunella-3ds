// Copyright 2013 Dolphin Emulator Project / 2014 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <thread>
#include "common/common_types.h"
#include "common/polyfill_thread.h"

namespace Common {

class Event {
public:
    void Set() {
        std::scoped_lock lk{mutex};
        if (!is_set) {
            is_set = true;
            condvar.notify_one();
        }
    }

    void Wait() {
        std::unique_lock lk{mutex};
        condvar.wait(lk, [&] { return is_set.load(); });
        is_set = false;
    }

    template <class Duration>
    bool WaitFor(const std::chrono::duration<Duration>& time) {
        std::unique_lock lk{mutex};
        if (!condvar.wait_for(lk, time, [this] { return is_set.load(); }))
            return false;
        is_set = false;
        return true;
    }

    template <class Clock, class Duration>
    bool WaitUntil(const std::chrono::time_point<Clock, Duration>& time) {
        std::unique_lock lk{mutex};
        if (!condvar.wait_until(lk, time, [this] { return is_set.load(); }))
            return false;
        is_set = false;
        return true;
    }

    void Reset() {
        std::unique_lock lk{mutex};
        // no other action required, since wait loops on the predicate and any lingering signal will
        // get cleared on the first iteration
        is_set = false;
    }

    [[nodiscard]] bool IsSet() {
        return is_set;
    }

private:
    std::condition_variable condvar;
    std::mutex mutex;
    std::atomic_bool is_set{false};
};

class Barrier {
public:
    explicit Barrier(std::size_t count_) : count(count_) {}

    /// Blocks until all "count" threads have called Sync()
    bool Sync(std::stop_token token = {}) {
        std::unique_lock lk{mutex};
        const std::size_t current_generation = generation;

        if (++waiting == count) {
            generation++;
            waiting = 0;
            condvar.notify_all();
            return true;
        } else {
            CondvarWait(condvar, lk, token,
                        [this, current_generation] { return current_generation != generation; });
            return !token.stop_requested();
        }
    }

    std::size_t Generation() {
        std::unique_lock lk{mutex};
        return generation;
    }

private:
    std::condition_variable_any condvar;
    std::mutex mutex;
    std::size_t count;
    std::size_t waiting = 0;
    std::size_t generation = 0; // Incremented once each time the barrier is used
};

/// Hybrid spin-then-sleep barrier.
/// Spins for a short period for low latency, then falls back to a condition
/// variable wait to conserve CPU when threads take longer to arrive.
class SpinBarrier {
public:
    explicit SpinBarrier(std::size_t count_) : total(count_) {}

    bool Sync(std::stop_token token = {}) {
        const std::size_t gen = generation.load(std::memory_order_acquire);

        if (arrived.fetch_add(1, std::memory_order_acq_rel) + 1 == total) {
            arrived.store(0, std::memory_order_relaxed);
            generation.store(gen + 1, std::memory_order_release);
            {
                std::scoped_lock lk{mutex};
            }
            condvar.notify_all();
            return true;
        }

        // Phase 1: Spin for low latency when other threads are about to arrive.
        for (int i = 0; i < SpinIterations; ++i) {
            if (generation.load(std::memory_order_acquire) != gen) {
                return true;
            }
            if (token.stop_requested()) [[unlikely]] {
                return false;
            }
            __builtin_ia32_pause();
        }

        // Phase 2: Yield to OS scheduler — moderate latency, lower power.
        for (int i = 0; i < YieldIterations; ++i) {
            if (generation.load(std::memory_order_acquire) != gen) {
                return true;
            }
            if (token.stop_requested()) [[unlikely]] {
                return false;
            }
            std::this_thread::yield();
        }

        // Phase 3: Sleep on a condition variable — highest efficiency.
        {
            std::unique_lock lk{mutex};
            condvar.wait_for(lk, std::chrono::microseconds(SleepIntervalUs),
                             [this, gen, &token] {
                                 return generation.load(std::memory_order_acquire) != gen ||
                                        token.stop_requested();
                             });
        }
        return !token.stop_requested();
    }

private:
    static constexpr int SpinIterations = 1000;
    static constexpr int YieldIterations = 10;
    static constexpr int SleepIntervalUs = 100;

    std::size_t total;
    std::atomic<std::size_t> arrived{0};
    std::atomic<std::size_t> generation{0};
    std::mutex mutex;
    std::condition_variable condvar;
};

enum class ThreadPriority : u32 {
    Low = 0,
    Normal = 1,
    High = 2,
    VeryHigh = 3,
    Critical = 4,
};

void SetCurrentThreadPriority(ThreadPriority new_priority);

void SetCurrentThreadName(const char* name);

} // namespace Common
