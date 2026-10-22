#pragma once

#include <algorithm>
#include <cstdlib>
#include <stdexcept>
#include <string>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <thread>
#include <chrono>
#endif

inline void setTestEnvironment(const char* name, const char* value) {
#if defined(_WIN32)
    const int status = _putenv_s(name, value);
#else
    const int status = setenv(name, value, 1);
#endif
    if (status != 0) throw std::runtime_error(std::string("Could not set test environment variable: ") + name);
}

inline void clearTestEnvironment(const char* name) {
#if defined(_WIN32)
    const int status = _putenv_s(name, "");
#else
    const int status = unsetenv(name);
#endif
    if (status != 0) throw std::runtime_error(std::string("Could not clear test environment variable: ") + name);
}

inline double systemLoadEquivalentCores() {
#if defined(_WIN32)
    FILETIME idleStart {}, kernelStart {}, userStart {};
    FILETIME idleEnd {}, kernelEnd {}, userEnd {};
    if (!GetSystemTimes(&idleStart, &kernelStart, &userStart)) return 0.0;
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    if (!GetSystemTimes(&idleEnd, &kernelEnd, &userEnd)) return 0.0;
    const auto ticks = [](const FILETIME& t) {
        return (static_cast<unsigned long long>(t.dwHighDateTime) << 32) | t.dwLowDateTime;
    };
    const auto idle = ticks(idleEnd) - ticks(idleStart);
    const auto total = ticks(kernelEnd) - ticks(kernelStart) + ticks(userEnd) - ticks(userStart);
    if (total == 0) return 0.0;
    const auto cores = std::max(1u, std::thread::hardware_concurrency());
    return static_cast<double>(total - std::min(total, idle)) * cores / total;
#else
    double load[1] { 0.0 };
    return getloadavg(load, 1) == 1 ? load[0] : 0.0;
#endif
}
