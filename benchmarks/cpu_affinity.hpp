#pragma once

#include <cerrno>
#include <cstdlib>
#include <limits>
#include <string>

#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#endif

namespace orderbook::benchmarking {

struct CpuBindingResult {
    int requested_cpu{-1};
    bool supported{};
    bool applied{};
};

inline int read_cpu_from_environment(const char* variable) noexcept {
    const char* text = std::getenv(variable);
    if (text == nullptr || *text == '\0') {
        return -1;
    }

    char* end = nullptr;
    errno = 0;
    const long value = std::strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value < 0 ||
        value > std::numeric_limits<int>::max()) {
        return -1;
    }
    return static_cast<int>(value);
}

inline CpuBindingResult bind_current_thread_from_environment(
    const char* variable) noexcept {
    CpuBindingResult result;
    result.requested_cpu = read_cpu_from_environment(variable);
    if (result.requested_cpu < 0) {
        return result;
    }

#if defined(__linux__)
    result.supported = true;
    if (result.requested_cpu >= CPU_SETSIZE) {
        return result;
    }
    cpu_set_t cpu_set;
    CPU_ZERO(&cpu_set);
    CPU_SET(result.requested_cpu, &cpu_set);
    result.applied = pthread_setaffinity_np(
                         pthread_self(), sizeof(cpu_set), &cpu_set) == 0;
#else
    result.supported = false;
    result.applied = false;
#endif
    return result;
}

inline bool binding_failed(const CpuBindingResult& result) noexcept {
    return result.requested_cpu >= 0 && !result.applied;
}

inline std::string binding_description(const char* role,
                                       const CpuBindingResult& result) {
    if (result.requested_cpu < 0) {
        return std::string(role) + "=unpinned";
    }
    if (!result.supported) {
        return std::string(role) + "=pinning-unsupported";
    }
    if (!result.applied) {
        return std::string(role) + "=pinning-failed";
    }
    return std::string(role) + "=cpu" +
           std::to_string(result.requested_cpu);
}

}  // namespace orderbook::benchmarking
