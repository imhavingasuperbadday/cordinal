// Background throughput yields to a small process-wide CPU budget, not just a lower priority.
#pragma once
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>
#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif

class BackgroundCpuBudget {
    using Clock = std::chrono::steady_clock;
    Clock::time_point deadline_ = Clock::now();
    uint64_t previousCpu_ = CpuTime();
    // About 0.5% of the machine; cap at 10% of one core on large systems.
    double fraction_ = std::min(0.10, 0.005 * std::max(1u, std::thread::hardware_concurrency()));

    static uint64_t CpuTime() {
#ifdef _WIN32
        FILETIME create{}, exit{}, kernel{}, user{};
        GetThreadTimes(GetCurrentThread(), &create, &exit, &kernel, &user);
        return ((uint64_t(kernel.dwHighDateTime) << 32) | kernel.dwLowDateTime) +
               ((uint64_t(user.dwHighDateTime) << 32) | user.dwLowDateTime);
#else
        timespec time{};
        clock_gettime(CLOCK_THREAD_CPUTIME_ID, &time);
        return uint64_t(time.tv_sec) * 10000000 + time.tv_nsec / 100;
#endif
    }
public:
    void Pace(const std::atomic<bool>& cancelled) {
        const auto now = Clock::now();
        const auto cpu = CpuTime();
        // Do not accumulate unlimited credit while waiting for disk or UI backpressure.
        deadline_ = std::max(deadline_, now - std::chrono::milliseconds(20));
        deadline_ += std::chrono::nanoseconds((int64_t)((cpu - previousCpu_) * 100.0 / fraction_));
        while (!cancelled.load(std::memory_order_relaxed) && Clock::now() < deadline_) {
            const auto remaining = deadline_ - Clock::now();
            std::this_thread::sleep_for(std::min(remaining, Clock::duration(std::chrono::milliseconds(20))));
        }
        previousCpu_ = CpuTime();
    }
};
