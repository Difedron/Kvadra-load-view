#pragma once

#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace monitor_model {

// Универсальный набор CPU-счетчиков
// На Linux поля приходят из /proc/stat, на macOS - из host_processor_info

struct CpuTimes {
    std::uint64_t user = 0;
    std::uint64_t nice = 0;
    std::uint64_t system = 0;
    std::uint64_t idle = 0;
    std::uint64_t iowait = 0;
    std::uint64_t irq = 0;
    std::uint64_t softirq = 0;
    std::uint64_t steal = 0;

    std::uint64_t total() const;
    std::uint64_t idleTotal() const;
};

// Минимальный прошлый снимок процесса для расчета CPU%

struct ProcessSample {
    int pid = 0;
    std::uint64_t cpuTicks = 0;
};

// Накопительные счетчики I/O. Скорости считаются по разнице двух таких снимков

struct IoCounters {
    std::uint64_t netRxBytes = 0;
    std::uint64_t netTxBytes = 0;
    std::uint64_t diskReadBytes = 0;
    std::uint64_t diskWriteBytes = 0;
};

}

class ResourceMonitor {
public:
    ResourceMonitor();

    // Возвращает актуальный снимок системы в JSON-формате для WebUI
    std::string collectJson();

private:
    std::mutex mutex_;
#if defined(__linux__) || defined(__APPLE__)
    std::vector<monitor_model::CpuTimes> previousCpuTimes_;
    std::unordered_map<int, monitor_model::ProcessSample> previousProcesses_;
    monitor_model::IoCounters previousIoCounters_;
    std::chrono::steady_clock::time_point previousIoAt_;
    std::chrono::steady_clock::time_point previousProcessesAt_;
    bool hasPreviousIo_ = false;
    bool hasPreviousProcesses_ = false;
#endif
};
