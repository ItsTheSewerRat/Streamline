#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>

#include <Windows.h>

#include "source/core/sl.log/log.h"

namespace renodx::streamline_diagnostics {

using Clock = std::chrono::steady_clock;

inline std::atomic<uint64_t> sequence{};
inline std::atomic<uint64_t> option_calls{};
inline std::atomic<uint64_t> state_calls{};
inline std::atomic<uint64_t> tag_calls{};
inline std::atomic<uint64_t> acquire_calls{};
inline std::atomic<uint64_t> host_present_calls{};
inline std::atomic<uint64_t> dlssg_present_calls{};
inline std::atomic<uint64_t> activation_calls{};
inline std::atomic<uint64_t> conversion_calls{};
inline std::atomic<uint32_t> detailed_trace_budget{};

inline uint64_t NextSequence()
{
    return sequence.fetch_add(1u, std::memory_order_relaxed) + 1u;
}

inline uint64_t ElapsedMicros(Clock::time_point start)
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            Clock::now() - start).count());
}

inline void ArmDetailedTrace(uint32_t count = 2048u)
{
    uint32_t current = detailed_trace_budget.load(std::memory_order_relaxed);
    while (current < count
        && !detailed_trace_budget.compare_exchange_weak(
            current, count, std::memory_order_relaxed))
    {
    }
}

inline bool TakeDetailedTrace()
{
    uint32_t current = detailed_trace_budget.load(std::memory_order_relaxed);
    while (current != 0u)
    {
        if (detailed_trace_budget.compare_exchange_weak(
                current, current - 1u, std::memory_order_relaxed))
        {
            return true;
        }
    }
    return false;
}

inline void Announce()
{
    static std::once_flag once;
    std::call_once(once, [] {
        SL_LOG_INFO("[RenoDX][diag-v1] Lifecycle diagnostics enabled; no synchronization or GPU behavior changes");
    });
}

inline void ObserveForeground(const char* site)
{
    static std::mutex foreground_mutex;
    static HWND previous_window{};
    static DWORD previous_process_id{UINT32_MAX};

    const HWND window = ::GetForegroundWindow();
    DWORD process_id{};
    if (window)
    {
        ::GetWindowThreadProcessId(window, &process_id);
    }

    std::lock_guard<std::mutex> lock(foreground_mutex);
    if (window == previous_window && process_id == previous_process_id)
    {
        return;
    }
    previous_window = window;
    previous_process_id = process_id;
    ArmDetailedTrace();

    char class_name[128]{};
    RECT rect{};
    if (window)
    {
        ::GetClassNameA(window, class_name, static_cast<int>(sizeof(class_name)));
        ::GetWindowRect(window, &rect);
    }
    SL_LOG_INFO(
        "[RenoDX][diag-v1] #%llu foreground site=%s hwnd=0x%llx pid=%lu self=%u class=%s visible=%u iconic=%u zoomed=%u rect=%ld,%ld,%ld,%ld",
        static_cast<unsigned long long>(NextSequence()),
        site,
        static_cast<unsigned long long>(
            reinterpret_cast<uintptr_t>(window)),
        static_cast<unsigned long>(process_id),
        process_id == ::GetCurrentProcessId() ? 1u : 0u,
        class_name[0] ? class_name : "<none>",
        window && ::IsWindowVisible(window) ? 1u : 0u,
        window && ::IsIconic(window) ? 1u : 0u,
        window && ::IsZoomed(window) ? 1u : 0u,
        rect.left,
        rect.top,
        rect.right,
        rect.bottom);
}

inline void LogHeartbeat(const char* site, uint64_t call)
{
    SL_LOG_INFO(
        "[RenoDX][diag-v1] #%llu heartbeat site=%s call=%llu options=%llu states=%llu tags=%llu acquires=%llu hostPresents=%llu dlssgPresents=%llu activations=%llu conversions=%llu traceBudget=%u",
        static_cast<unsigned long long>(NextSequence()),
        site,
        static_cast<unsigned long long>(call),
        static_cast<unsigned long long>(option_calls.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(state_calls.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(tag_calls.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(acquire_calls.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(host_present_calls.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(dlssg_present_calls.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(activation_calls.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(conversion_calls.load(std::memory_order_relaxed)),
        detailed_trace_budget.load(std::memory_order_relaxed));
}

} // namespace renodx::streamline_diagnostics
