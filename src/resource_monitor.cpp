#include "resource_monitor.h"

#ifdef __linux__
#include <dirent.h>
#include <pwd.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/storage/IOBlockStorageDriver.h>
#include <ifaddrs.h>
#include <libproc.h>
#include <mach/host_info.h>
#include <mach/mach.h>
#include <mach/mach_host.h>
#include <mach/processor_info.h>
#include <net/if.h>
#include <net/if_dl.h>
#include <pwd.h>
#include <sys/proc.h>
#include <sys/sysctl.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <utility>

using monitor_model::CpuTimes;

#if defined(__linux__) || defined(__APPLE__)
using monitor_model::IoCounters;
using monitor_model::ProcessSample;
#endif

namespace {

// Экранирует строки перед ручной сборкой JSON

std::string jsonEscape(const std::string& value) {
    std::ostringstream escaped;

    for (unsigned char symbol : value) {
        switch (symbol) {
            case '"':
                escaped << "\\\"";
                break;
            case '\\':
                escaped << "\\\\";
                break;
            case '\n':
                escaped << "\\n";
                break;
            case '\r':
                escaped << "\\r";
                break;
            case '\t':
                escaped << "\\t";
                break;
            default:
                if (symbol < 0x20) {
                    escaped << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                            << static_cast<int>(symbol) << std::dec << std::setfill(' ');
                } else {
                    escaped << static_cast<char>(symbol);
                }
        }
    }

    return escaped.str();
}

// Читаемая отметка времени для верхней панели WebUI

std::string isoTimestampNow() {
    const std::time_t now = std::time(nullptr);
    std::tm localTime {};
    localtime_r(&now, &localTime);

    std::ostringstream formatted;
    formatted << std::put_time(&localTime, "%Y-%m-%d %H:%M:%S");
    return formatted.str();
}

#if defined(__linux__) || defined(__APPLE__)

// Общая структура памяти для Linux и macOS
// Все значения храним в KiB, чтобы frontend одинаково работал на обеих ОС

struct MemoryInfo {
    std::uint64_t totalKb = 0;
    std::uint64_t availableKb = 0;
    std::uint64_t usedKb = 0;
    std::uint64_t swapTotalKb = 0;
    std::uint64_t swapUsedKb = 0;
};

// Load average и счетчик процессов. На Linux часть данных приходит из /proc/loadavg, а на macOS load берется через getloadavg(), а процессы считаются отдельно
struct LoadInfo {
    double one = 0.0;
    double five = 0.0;
    double fifteen = 0.0;
    int runningProcesses = 0;
    int totalProcesses = 0;
};

// Унифицированная строка таблицы процессов для WebUI
// Backend заполняет ее из разных источников(/proc на Linux, libproc на macOS)

struct ProcessInfo {
    int pid = 0;
    std::string user;
    std::string command;
    char state = '?';
    double cpuPercent = 0.0;
    std::uint64_t memoryKb = 0;
    std::uint64_t threads = 0;
    std::uint64_t cpuTicks = 0;
};

// Защита от деления на ноль

double percent(std::uint64_t used, std::uint64_t total) {
    if (total == 0) {
        return 0.0;
    }

    return static_cast<double>(used) * 100.0 / static_cast<double>(total);
}

// Переводит счетчики байт в скорость байт/сек между двумя снимками

double counterRate(std::uint64_t current, std::uint64_t previous, double seconds) {
    if (seconds <= 0.0 || current < previous) {
        return 0.0;
    }

    return static_cast<double>(current - previous) / seconds;
}

// CPU считается по дельте счетчиков, как в top/htop: берем разницу total и idle между двумя соседними запросами

double calculateCpuUsage(const CpuTimes& current, const CpuTimes& previous) {
    const std::uint64_t totalDiff = current.total() - previous.total();
    const std::uint64_t idleDiff = current.idleTotal() - previous.idleTotal();

    if (totalDiff == 0 || idleDiff > totalDiff) {
        return 0.0;
    }

    return static_cast<double>(totalDiff - idleDiff) * 100.0 /
           static_cast<double>(totalDiff);
}

// Сериализует массив чисел в JSON без внешней библиотеки

std::string formatJsonArray(const std::vector<double>& values) {
    std::ostringstream json;
    json << std::fixed << std::setprecision(1);
    json << "[";

    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i > 0) {
            json << ",";
        }
        json << values[i];
    }

    json << "]";
    return json.str();
}

// UID превращаем в имя пользователя, чтобы таблица процессов была читаемой

std::string userNameByUid(std::uint64_t uid) {
    passwd* user = getpwuid(static_cast<uid_t>(uid));
    if (user != nullptr && user->pw_name != nullptr) {
        return user->pw_name;
    }

    return std::to_string(uid);
}

// Собирает JSON-массив процессов. Команды обязательно экранируются

std::string buildProcessesJson(const std::vector<ProcessInfo>& processes) {
    std::ostringstream json;
    json << std::fixed << std::setprecision(1);
    json << "[";

    for (std::size_t i = 0; i < processes.size(); ++i) {
        const ProcessInfo& process = processes[i];
        if (i > 0) {
            json << ",";
        }

        json << "{";
        json << "\"pid\":" << process.pid << ",";
        json << "\"user\":\"" << jsonEscape(process.user) << "\",";
        json << "\"command\":\"" << jsonEscape(process.command) << "\",";
        json << "\"state\":\"" << process.state << "\",";
        json << "\"cpu\":" << process.cpuPercent << ",";
        json << "\"memoryKb\":" << process.memoryKb << ",";
        json << "\"threads\":" << process.threads;
        json << "}";
    }

    json << "]";
    return json.str();
}

// Финальная сборка ответа /api/stats
// Платформенные функции только собирают данные, а этот блок приводит их к одному JSON

std::string buildSnapshotJson(const std::string& platform,
                              double uptime,
                              const LoadInfo& load,
                              double totalCpu,
                              const std::vector<double>& coreUsage,
                              const MemoryInfo& memory,
                              const IoCounters& currentIo,
                              double netRxRate,
                              double netTxRate,
                              double diskReadRate,
                              double diskWriteRate,
                              const std::vector<ProcessInfo>& processes) {
    std::ostringstream json;
    json << std::fixed << std::setprecision(1);
    json << "{";
    json << "\"timestamp\":\"" << isoTimestampNow() << "\",";
    json << "\"platform\":\"" << platform << "\",";
    json << "\"uptimeSeconds\":" << uptime << ",";
    json << "\"loadAverage\":[" << load.one << "," << load.five << ","
         << load.fifteen << "],";
    json << "\"runningProcesses\":" << load.runningProcesses << ",";
    json << "\"totalProcesses\":" << load.totalProcesses << ",";
    json << "\"cpu\":{\"total\":" << totalCpu
         << ",\"cores\":" << formatJsonArray(coreUsage) << "},";
    json << "\"memory\":{\"totalKb\":" << memory.totalKb
         << ",\"usedKb\":" << memory.usedKb
         << ",\"availableKb\":" << memory.availableKb
         << ",\"percent\":" << percent(memory.usedKb, memory.totalKb) << "},";
    json << "\"swap\":{\"totalKb\":" << memory.swapTotalKb
         << ",\"usedKb\":" << memory.swapUsedKb
         << ",\"percent\":" << percent(memory.swapUsedKb, memory.swapTotalKb) << "},";
    json << "\"network\":{\"rxBytes\":" << currentIo.netRxBytes
         << ",\"txBytes\":" << currentIo.netTxBytes
         << ",\"rxRate\":" << netRxRate
         << ",\"txRate\":" << netTxRate << "},";
    json << "\"disk\":{\"readBytes\":" << currentIo.diskReadBytes
         << ",\"writeBytes\":" << currentIo.diskWriteBytes
         << ",\"readRate\":" << diskReadRate
         << ",\"writeRate\":" << diskWriteRate << "},";
    json << "\"processes\":" << buildProcessesJson(processes) << ",";
    json << "\"error\":null";
    json << "}\n";
    return json.str();
}

// Обновляет общий CPU и список ядер на основе предыдущего снимка
// Первый вызов всегда возвращает нули: для процента нужна пара измерений

void updateCpuUsage(const std::vector<CpuTimes>& cpuTimes,
                    std::vector<CpuTimes>& previousCpuTimes,
                    double& totalCpu,
                    std::vector<double>& coreUsage) {
    std::vector<double> cpuUsage;
    if (!previousCpuTimes.empty() && previousCpuTimes.size() == cpuTimes.size()) {
        for (std::size_t i = 0; i < cpuTimes.size(); ++i) {
            cpuUsage.push_back(calculateCpuUsage(cpuTimes[i], previousCpuTimes[i]));
        }
    } else {
        cpuUsage.assign(cpuTimes.size(), 0.0);
    }
    previousCpuTimes = cpuTimes;

    totalCpu = cpuUsage.empty() ? 0.0 : cpuUsage.front();
    if (cpuUsage.size() > 1) {
        coreUsage.assign(cpuUsage.begin() + 1, cpuUsage.end());
    }
}

// Хранит прошлые счетчики сети/диска и считает скорости для текущего ответа

void updateIoRates(const IoCounters& currentIo,
                   IoCounters& previousIo,
                   std::chrono::steady_clock::time_point& previousAt,
                   bool& hasPrevious,
                   double& netRxRate,
                   double& netTxRate,
                   double& diskReadRate,
                   double& diskWriteRate) {
    const auto now = std::chrono::steady_clock::now();
    double elapsed = 0.0;
    if (hasPrevious) {
        elapsed = std::chrono::duration<double>(now - previousAt).count();
    }

    netRxRate = counterRate(currentIo.netRxBytes, previousIo.netRxBytes, elapsed);
    netTxRate = counterRate(currentIo.netTxBytes, previousIo.netTxBytes, elapsed);
    diskReadRate =
        counterRate(currentIo.diskReadBytes, previousIo.diskReadBytes, elapsed);
    diskWriteRate =
        counterRate(currentIo.diskWriteBytes, previousIo.diskWriteBytes, elapsed);

    previousIo = currentIo;
    previousAt = now;
    hasPrevious = true;
}

// Запоминает CPU-счетчики процессов, чтобы на следующем запросе посчитать проценты

void rememberProcessSamples(const std::vector<ProcessInfo>& processes,
                            std::unordered_map<int, ProcessSample>& previousProcesses,
                            std::chrono::steady_clock::time_point& previousAt,
                            bool& hasPrevious) {
    previousProcesses.clear();
    for (const ProcessInfo& process : processes) {
        previousProcesses[process.pid] = ProcessSample {process.pid, process.cpuTicks};
    }

    previousAt = std::chrono::steady_clock::now();
    hasPrevious = true;
}

// UI показывает только верх таблицы, но для расчета CPU мы сначала читаем все процессы

void trimProcessesForUi(std::vector<ProcessInfo>& processes) {
    constexpr std::size_t MaxProcessesInUi = 18;
    if (processes.size() > MaxProcessesInUi) {
        processes.resize(MaxProcessesInUi);
    }
}

#endif

#ifdef __linux__

// Чтение маленьких файлов из /proc

std::string readTextFile(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return {};
    }

    std::ostringstream content;
    content << file.rdbuf();
    return content.str();
}

// Утилита для фильтрации строк и имен устройств

bool startsWith(const std::string& value, const std::string& prefix) {
    return value.rfind(prefix, 0) == 0;
}

// Удаляет пробелы по краям строки, например вокруг имени сетевого интерфейса

std::string trim(const std::string& value) {
    const auto first = value.find_first_not_of(" \t\n\r");
    if (first == std::string::npos) {
        return {};
    }

    const auto last = value.find_last_not_of(" \t\n\r");
    return value.substr(first, last - first + 1);
}

// Безопасный перевод строки в число для данных из /proc

std::uint64_t toUInt64(const std::string& value) {
    try {
        return static_cast<std::uint64_t>(std::stoull(value));
    } catch (...) {
        return 0;
    }
}

// /proc/stat содержит накопительные CPU-счетчики с момента загрузки системы

std::vector<CpuTimes> readLinuxCpuTimes() {
    std::ifstream file("/proc/stat");
    std::vector<CpuTimes> result;
    std::string line;

    while (std::getline(file, line)) {
        if (!startsWith(line, "cpu")) {
            break;
        }

        std::istringstream input(line);
        std::string name;
        CpuTimes times;
        input >> name >> times.user >> times.nice >> times.system >> times.idle >>
            times.iowait >> times.irq >> times.softirq >> times.steal;

        // Берем строку cpu и строки cpu0/cpu1/...; остальные строки /proc/stat не нужны

        if (name == "cpu" ||
            (name.size() > 3 && std::isdigit(static_cast<unsigned char>(name[3])))) {
            result.push_back(times);
        }
    }

    return result;
}

// /proc/meminfo дает RAM и swap в KiB

MemoryInfo readLinuxMemoryInfo() {
    std::ifstream file("/proc/meminfo");
    std::map<std::string, std::uint64_t> values;
    std::string key;
    std::uint64_t value = 0;
    std::string unit;

    while (file >> key >> value >> unit) {
        if (!key.empty() && key.back() == ':') {
            key.pop_back();
        }
        values[key] = value;
    }

    MemoryInfo info;
    info.totalKb = values["MemTotal"];
    info.availableKb = values.count("MemAvailable")
                           ? values["MemAvailable"]
                           : values["MemFree"] + values["Buffers"] + values["Cached"];
    info.usedKb = info.totalKb > info.availableKb ? info.totalKb - info.availableKb : 0;
    info.swapTotalKb = values["SwapTotal"];
    const std::uint64_t swapFreeKb = values["SwapFree"];
    info.swapUsedKb = info.swapTotalKb > swapFreeKb ? info.swapTotalKb - swapFreeKb : 0;
    return info;
}

// /proc/loadavg сразу содержит load average и пару running/total processes

LoadInfo readLinuxLoadInfo() {
    std::ifstream file("/proc/loadavg");
    LoadInfo info;
    std::string processes;

    file >> info.one >> info.five >> info.fifteen >> processes;

    const std::size_t slash = processes.find('/');
    if (slash != std::string::npos) {
        info.runningProcesses = static_cast<int>(toUInt64(processes.substr(0, slash)));
        info.totalProcesses = static_cast<int>(toUInt64(processes.substr(slash + 1)));
    }

    return info;
}

// /proc/uptime хранит количество секунд с момента загрузки

double readLinuxUptimeSeconds() {
    std::ifstream file("/proc/uptime");
    double uptime = 0.0;
    file >> uptime;
    return uptime;
}

// /proc/net/dev хранит суммарные байты по интерфейсам
// loopback пропускаем, чтобы локальные запросы WebUI не искажали картину сети

IoCounters readLinuxNetworkCounters(IoCounters counters) {
    std::ifstream file("/proc/net/dev");
    std::string line;
    int lineNumber = 0;

    while (std::getline(file, line)) {
        if (++lineNumber <= 2) {
            continue;
        }

        const std::size_t colon = line.find(':');
        if (colon == std::string::npos) {
            continue;
        }

        const std::string interfaceName = trim(line.substr(0, colon));
        if (interfaceName == "lo") {
            continue;
        }

        std::istringstream values(line.substr(colon + 1));
        std::uint64_t rxBytes = 0;
        std::uint64_t ignored = 0;
        std::uint64_t txBytes = 0;

        values >> rxBytes;
        for (int i = 0; i < 7; ++i) {
            values >> ignored;
        }
        values >> txBytes;

        counters.netRxBytes += rxBytes;
        counters.netTxBytes += txBytes;
    }

    return counters;
}

// В diskstats не берем виртуальные ram/loop/fd устройства

bool shouldSkipLinuxBlockDevice(const std::string& name) {
    return startsWith(name, "loop") || startsWith(name, "ram") || startsWith(name, "fd");
}

// /sys/block помогает отличить реальные блочные устройства от разделов

std::set<std::string> readLinuxBlockDevices() {
    std::set<std::string> devices;
    DIR* directory = opendir("/sys/block");
    if (directory == nullptr) {
        return devices;
    }

    while (dirent* entry = readdir(directory)) {
        const std::string name = entry->d_name;
        if (name == "." || name == ".." || shouldSkipLinuxBlockDevice(name)) {
            continue;
        }
        devices.insert(name);
    }

    closedir(directory);
    return devices;
}

// /proc/diskstats хранит сектора чтения/записи

IoCounters readLinuxDiskCounters(IoCounters counters) {
    const std::set<std::string> blockDevices = readLinuxBlockDevices();
    std::ifstream file("/proc/diskstats");
    std::string line;

    while (std::getline(file, line)) {
        std::istringstream input(line);
        int major = 0;
        int minor = 0;
        std::string name;
        input >> major >> minor >> name;

        if (!blockDevices.count(name)) {
            continue;
        }

        std::uint64_t readsCompleted = 0;
        std::uint64_t readsMerged = 0;
        std::uint64_t sectorsRead = 0;
        std::uint64_t msReading = 0;
        std::uint64_t writesCompleted = 0;
        std::uint64_t writesMerged = 0;
        std::uint64_t sectorsWritten = 0;

        input >> readsCompleted >> readsMerged >> sectorsRead >> msReading >>
            writesCompleted >> writesMerged >> sectorsWritten;

        // Linux обычно хранит размер сектора 512 байт для этих счетчиков

        counters.diskReadBytes += sectorsRead * 512ULL;
        counters.diskWriteBytes += sectorsWritten * 512ULL;
    }

    return counters;
}

// Все PID в Linux представлены числовыми папками внутри /proc

std::vector<int> readLinuxProcessIds() {
    std::vector<int> pids;
    DIR* directory = opendir("/proc");
    if (directory == nullptr) {
        return pids;
    }

    while (dirent* entry = readdir(directory)) {
        const std::string name = entry->d_name;
        if (!name.empty() &&
            std::all_of(name.begin(), name.end(), [](unsigned char symbol) {
                return std::isdigit(symbol);
            })) {
            pids.push_back(static_cast<int>(toUInt64(name)));
        }
    }

    closedir(directory);
    return pids;
}

// Пользователя процесса берем из /proc/<pid>/status

std::string readLinuxProcessUser(int pid) {
    std::ifstream file("/proc/" + std::to_string(pid) + "/status");
    std::string line;

    while (std::getline(file, line)) {
        if (!startsWith(line, "Uid:")) {
            continue;
        }

        std::istringstream input(line.substr(4));
        std::uint64_t uid = 0;
        input >> uid;
        return userNameByUid(uid);
    }

    return "?";
}

// Командная строка процесса лежит в /proc/<pid>/cmdline с нулевыми разделителями

std::string readLinuxProcessCommand(int pid, const std::string& fallbackName) {
    std::string command = readTextFile("/proc/" + std::to_string(pid) + "/cmdline");
    for (char& symbol : command) {
        if (symbol == '\0') {
            symbol = ' ';
        }
    }

    command = trim(command);
    if (command.empty()) {
        return "[" + fallbackName + "]";
    }

    constexpr std::size_t MaxCommandLength = 120;
    if (command.size() > MaxCommandLength) {
        command = command.substr(0, MaxCommandLength - 3) + "...";
    }

    return command;
}

// Разбирает /proc/<pid>/stat. Там есть CPU ticks, RSS, state и число потоков

bool readLinuxProcessInfo(int pid, ProcessInfo& info) {
    const std::string stat = readTextFile("/proc/" + std::to_string(pid) + "/stat");
    if (stat.empty()) {
        return false;
    }

    const std::size_t openName = stat.find('(');
    const std::size_t closeName = stat.rfind(')');
    if (openName == std::string::npos || closeName == std::string::npos ||
        closeName <= openName) {
        return false;
    }

    const std::string name = stat.substr(openName + 1, closeName - openName - 1);
    std::istringstream rest(stat.substr(closeName + 2));

    char state = '?';
    rest >> state;

    std::vector<std::string> fields;
    std::string token;
    while (rest >> token) {
        fields.push_back(token);
    }

    if (fields.size() < 21) {
        return false;
    }

    const std::uint64_t userTicks = toUInt64(fields[10]);
    const std::uint64_t systemTicks = toUInt64(fields[11]);
    const std::uint64_t threadCount = toUInt64(fields[16]);
    const long long residentPages = static_cast<long long>(toUInt64(fields[20]));
    const long pageSize = sysconf(_SC_PAGESIZE);

    info.pid = pid;
    info.user = readLinuxProcessUser(pid);
    info.command = readLinuxProcessCommand(pid, name);
    info.state = state;
    info.cpuTicks = userTicks + systemTicks;
    info.threads = threadCount;
    info.memoryKb = residentPages > 0
                        ? static_cast<std::uint64_t>(residentPages) *
                              static_cast<std::uint64_t>(pageSize) / 1024ULL
                        : 0;
    return true;
}

// Собирает список процессов и считает CPU% по разнице ticks между запросами

std::vector<ProcessInfo> readLinuxProcesses(
    const std::unordered_map<int, ProcessSample>& previous,
    bool hasPrevious,
    double elapsedSeconds) {
    std::vector<ProcessInfo> processes;
    const long ticksPerSecond = sysconf(_SC_CLK_TCK);

    for (int pid : readLinuxProcessIds()) {
        ProcessInfo process;
        if (!readLinuxProcessInfo(pid, process)) {
            continue;
        }

        const auto previousIt = previous.find(pid);
        if (hasPrevious && previousIt != previous.end() && elapsedSeconds > 0.0 &&
            ticksPerSecond > 0 && process.cpuTicks >= previousIt->second.cpuTicks) {
            const std::uint64_t ticksDiff =
                process.cpuTicks - previousIt->second.cpuTicks;
            process.cpuPercent = static_cast<double>(ticksDiff) * 100.0 /
                                 (static_cast<double>(ticksPerSecond) *
                                  elapsedSeconds);
        }

        processes.push_back(std::move(process));
    }

    std::sort(processes.begin(), processes.end(), [](const ProcessInfo& left,
                                                     const ProcessInfo& right) {
        if (std::fabs(left.cpuPercent - right.cpuPercent) > 0.05) {
            return left.cpuPercent > right.cpuPercent;
        }
        if (left.memoryKb != right.memoryKb) {
            return left.memoryKb > right.memoryKb;
        }
        return left.pid < right.pid;
    });

    return processes;
}

#endif

#ifdef __APPLE__

// sysctlbyname удобен для простых числовых метрик macOS, например hw.memsize

std::uint64_t readMacSysctlUInt64(const char* name) {
    std::uint64_t value = 0;
    std::size_t size = sizeof(value);
    if (sysctlbyname(name, &value, &size, nullptr, 0) != 0) {
        return 0;
    }

    return value;
}

// host_processor_info возвращает CPU ticks по каждому ядру
// Сначала добавляем суммарную строку, затем отдельные ядра

std::vector<CpuTimes> readMacCpuTimes() {
    natural_t cpuCount = 0;
    mach_msg_type_number_t infoCount = 0;
    processor_info_array_t rawCpuInfo = nullptr;

    const kern_return_t result = host_processor_info(mach_host_self(),
                                                     PROCESSOR_CPU_LOAD_INFO,
                                                     &cpuCount,
                                                     &rawCpuInfo,
                                                     &infoCount);
    if (result != KERN_SUCCESS || rawCpuInfo == nullptr) {
        return {};
    }

    const auto cpuInfo = reinterpret_cast<processor_cpu_load_info_t>(rawCpuInfo);
    std::vector<CpuTimes> times;
    CpuTimes total;

    for (natural_t i = 0; i < cpuCount; ++i) {
        CpuTimes core;
        core.user = cpuInfo[i].cpu_ticks[CPU_STATE_USER];
        core.nice = cpuInfo[i].cpu_ticks[CPU_STATE_NICE];
        core.system = cpuInfo[i].cpu_ticks[CPU_STATE_SYSTEM];
        core.idle = cpuInfo[i].cpu_ticks[CPU_STATE_IDLE];

        total.user += core.user;
        total.nice += core.nice;
        total.system += core.system;
        total.idle += core.idle;
        times.push_back(core);
    }

    times.insert(times.begin(), total);

    vm_deallocate(mach_task_self(),
                  reinterpret_cast<vm_address_t>(rawCpuInfo),
                  infoCount * sizeof(integer_t));
    return times;
}

// Память macOS читаем через mach VM statistics, swap - через sysctl vm.swapusage

MemoryInfo readMacMemoryInfo() {
    MemoryInfo info;
    const std::uint64_t totalBytes = readMacSysctlUInt64("hw.memsize");

    vm_statistics64_data_t stats {};
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    vm_size_t pageSize = 0;

    host_page_size(mach_host_self(), &pageSize);
    const kern_return_t result = host_statistics64(mach_host_self(),
                                                   HOST_VM_INFO64,
                                                   reinterpret_cast<host_info64_t>(&stats),
                                                   &count);
    if (result == KERN_SUCCESS && pageSize > 0) {
        const std::uint64_t availablePages =
            stats.free_count + stats.inactive_count + stats.speculative_count;
        const std::uint64_t availableBytes = availablePages * pageSize;

        info.totalKb = totalBytes / 1024ULL;
        info.availableKb = availableBytes / 1024ULL;
        info.usedKb = info.totalKb > info.availableKb ? info.totalKb - info.availableKb : 0;
    }

    struct xsw_usage swap {};
    std::size_t swapSize = sizeof(swap);
    if (sysctlbyname("vm.swapusage", &swap, &swapSize, nullptr, 0) == 0) {
        info.swapTotalKb = swap.xsu_total / 1024ULL;
        info.swapUsedKb = swap.xsu_used / 1024ULL;
    }

    return info;
}

// getloadavg - стандартный POSIX-способ получить load average на macOS

LoadInfo readMacLoadInfo() {
    double loadAverage[3] {};
    LoadInfo info;

    if (getloadavg(loadAverage, 3) == 3) {
        info.one = loadAverage[0];
        info.five = loadAverage[1];
        info.fifteen = loadAverage[2];
    }

    return info;
}

// Время загрузки системы хранится в kern.boottime

double readMacUptimeSeconds() {
    timeval bootTime {};
    std::size_t size = sizeof(bootTime);
    int mib[2] = {CTL_KERN, KERN_BOOTTIME};

    if (sysctl(mib, 2, &bootTime, &size, nullptr, 0) != 0 || bootTime.tv_sec == 0) {
        return 0.0;
    }

    return std::difftime(std::time(nullptr), bootTime.tv_sec);
}

// getifaddrs дает список интерфейсов и счетчики байт
// loopback пропускаем по той же причине, что и на Linux

IoCounters readMacNetworkCounters(IoCounters counters) {
    ifaddrs* interfaces = nullptr;
    if (getifaddrs(&interfaces) != 0 || interfaces == nullptr) {
        return counters;
    }

    for (ifaddrs* item = interfaces; item != nullptr; item = item->ifa_next) {
        if (item->ifa_addr == nullptr || item->ifa_data == nullptr) {
            continue;
        }
        if (item->ifa_addr->sa_family != AF_LINK) {
            continue;
        }
        if ((item->ifa_flags & IFF_LOOPBACK) != 0 || (item->ifa_flags & IFF_UP) == 0) {
            continue;
        }

        const auto* data = static_cast<const if_data*>(item->ifa_data);
        counters.netRxBytes += data->ifi_ibytes;
        counters.netTxBytes += data->ifi_obytes;
    }

    freeifaddrs(interfaces);
    return counters;
}

// Достает числовое значение из CFDictionary, который возвращает IOKit

std::uint64_t readCfNumberFromDictionary(CFDictionaryRef dictionary, const char* key) {
    if (dictionary == nullptr || key == nullptr) {
        return 0;
    }

    CFStringRef cfKey =
        CFStringCreateWithCString(kCFAllocatorDefault, key, kCFStringEncodingUTF8);
    if (cfKey == nullptr) {
        return 0;
    }

    const void* rawValue = CFDictionaryGetValue(dictionary, cfKey);
    CFRelease(cfKey);

    if (rawValue == nullptr || CFGetTypeID(rawValue) != CFNumberGetTypeID()) {
        return 0;
    }

    std::uint64_t value = 0;
    CFNumberGetValue(static_cast<CFNumberRef>(rawValue), kCFNumberSInt64Type, &value);
    return value;
}

// IOKit отдает накопительные счетчики чтения или записи по блочным драйверам

IoCounters readMacDiskCounters(IoCounters counters) {
    io_iterator_t iterator = IO_OBJECT_NULL;
    const kern_return_t result =
        IOServiceGetMatchingServices(kIOMainPortDefault,
                                     IOServiceMatching(kIOBlockStorageDriverClass),
                                     &iterator);
    if (result != KERN_SUCCESS) {
        return counters;
    }

    io_object_t service = IO_OBJECT_NULL;
    while ((service = IOIteratorNext(iterator)) != IO_OBJECT_NULL) {
        CFMutableDictionaryRef properties = nullptr;
        const kern_return_t propertyResult =
            IORegistryEntryCreateCFProperties(service,
                                              &properties,
                                              kCFAllocatorDefault,
                                              kNilOptions);
        if (propertyResult == KERN_SUCCESS && properties != nullptr) {
            CFStringRef statsKey = CFStringCreateWithCString(
                kCFAllocatorDefault,
                kIOBlockStorageDriverStatisticsKey,
                kCFStringEncodingUTF8);
            const void* rawStats = statsKey != nullptr
                                       ? CFDictionaryGetValue(properties, statsKey)
                                       : nullptr;
            if (statsKey != nullptr) {
                CFRelease(statsKey);
            }

            if (rawStats != nullptr && CFGetTypeID(rawStats) == CFDictionaryGetTypeID()) {
                const auto stats = static_cast<CFDictionaryRef>(rawStats);
                counters.diskReadBytes += readCfNumberFromDictionary(
                    stats,
                    kIOBlockStorageDriverStatisticsBytesReadKey);
                counters.diskWriteBytes += readCfNumberFromDictionary(
                    stats,
                    kIOBlockStorageDriverStatisticsBytesWrittenKey);
            }

            CFRelease(properties);
        }

        IOObjectRelease(service);
    }

    IOObjectRelease(iterator);
    return counters;
}

// libproc возвращает список PID без необходимости вызывать внешние команды

std::vector<int> readMacProcessIds() {
    const int bytesNeeded = proc_listpids(PROC_ALL_PIDS, 0, nullptr, 0);
    if (bytesNeeded <= 0) {
        return {};
    }

    std::vector<pid_t> rawPids(static_cast<std::size_t>(bytesNeeded) / sizeof(pid_t) +
                               64);
    const int bytesUsed = proc_listpids(PROC_ALL_PIDS,
                                        0,
                                        rawPids.data(),
                                        static_cast<int>(rawPids.size() * sizeof(pid_t)));
    if (bytesUsed <= 0) {
        return {};
    }

    std::vector<int> pids;
    rawPids.resize(static_cast<std::size_t>(bytesUsed) / sizeof(pid_t));
    for (pid_t pid : rawPids) {
        if (pid > 0) {
            pids.push_back(pid);
        }
    }

    return pids;
}

// Приводит BSD-статус процесса к короткой букве, похожей на top

char macProcessStateToChar(uint32_t status) {
    switch (status) {
        case SRUN:
            return 'R';
        case SSLEEP:
            return 'S';
        case SSTOP:
            return 'T';
        case SZOMB:
            return 'Z';
        case SIDL:
            return 'I';
        default:
            return '?';
    }
}

// Ограничивает длинные пути к приложениям, чтобы таблица не разъезжалась

std::string truncateCommand(std::string command) {
    constexpr std::size_t MaxCommandLength = 120;
    if (command.size() > MaxCommandLength) {
        command = command.substr(0, MaxCommandLength - 3) + "...";
    }

    return command;
}

// Читает процесс macOS через libproc: пользователь, память, CPU time, потоков и путь

bool readMacProcessInfo(int pid, ProcessInfo& info) {
    proc_bsdinfo bsd {};
    const int bsdBytes = proc_pidinfo(pid,
                                      PROC_PIDTBSDINFO,
                                      0,
                                      &bsd,
                                      PROC_PIDTBSDINFO_SIZE);
    if (bsdBytes < static_cast<int>(PROC_PIDTBSDINFO_SIZE)) {
        return false;
    }

    proc_taskinfo task {};
    const int taskBytes = proc_pidinfo(pid,
                                       PROC_PIDTASKINFO,
                                       0,
                                       &task,
                                       PROC_PIDTASKINFO_SIZE);
    if (taskBytes < static_cast<int>(PROC_PIDTASKINFO_SIZE)) {
        return false;
    }

    char path[PROC_PIDPATHINFO_MAXSIZE] {};
    const int pathLength = proc_pidpath(pid, path, sizeof(path));
    std::string command;
    if (pathLength > 0) {
        command = path;
    } else if (bsd.pbi_name[0] != '\0') {
        command = bsd.pbi_name;
    } else {
        command = bsd.pbi_comm;
    }

    info.pid = pid;
    info.user = userNameByUid(bsd.pbi_uid);
    info.command = truncateCommand(command.empty() ? "?" : command);

    // В macOS статус BSD часто означает "runnable" для живого процесса,
    // поэтому для UI точнее смотреть, есть ли прямо сейчас выполняющиеся потоки

    const char bsdState = macProcessStateToChar(bsd.pbi_status);
    info.state = task.pti_numrunning > 0 ? 'R' : (bsdState == 'R' ? 'S' : bsdState);
    info.cpuTicks = task.pti_total_user + task.pti_total_system;
    info.memoryKb = task.pti_resident_size / 1024ULL;
    info.threads = static_cast<std::uint64_t>(std::max(task.pti_threadnum, 0));
    return true;
}

// Собирает процессы macOS и считает CPU% по наносекундам user+system time

std::vector<ProcessInfo> readMacProcesses(
    const std::unordered_map<int, ProcessSample>& previous,
    bool hasPrevious,
    double elapsedSeconds,
    LoadInfo& load) {
    std::vector<ProcessInfo> processes;

    for (int pid : readMacProcessIds()) {
        ProcessInfo process;
        if (!readMacProcessInfo(pid, process)) {
            continue;
        }

        const auto previousIt = previous.find(pid);
        if (hasPrevious && previousIt != previous.end() && elapsedSeconds > 0.0 &&
            process.cpuTicks >= previousIt->second.cpuTicks) {
            const std::uint64_t ticksDiff =
                process.cpuTicks - previousIt->second.cpuTicks;
            process.cpuPercent = static_cast<double>(ticksDiff) * 100.0 /
                                 (elapsedSeconds * 1000000000.0);
        }

        if (process.state == 'R') {
            ++load.runningProcesses;
        }
        ++load.totalProcesses;

        processes.push_back(std::move(process));
    }

    std::sort(processes.begin(), processes.end(), [](const ProcessInfo& left,
                                                     const ProcessInfo& right) {
        if (std::fabs(left.cpuPercent - right.cpuPercent) > 0.05) {
            return left.cpuPercent > right.cpuPercent;
        }
        if (left.memoryKb != right.memoryKb) {
            return left.memoryKb > right.memoryKb;
        }
        return left.pid < right.pid;
    });

    return processes;
}

#endif

#if !defined(__linux__) && !defined(__APPLE__)

// Демо-режим нужен только для неизвестных платформ, где нет предусмотрена реализация сборщика

std::string buildDemoJson() {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    const double seconds =
        std::chrono::duration_cast<std::chrono::milliseconds>(now).count() / 1000.0;
    const double wave = (std::sin(seconds) + 1.0) * 0.5;
    const double cpu = 18.0 + wave * 55.0;
    const double mem = 42.0 + wave * 18.0;

    std::ostringstream json;
    json << std::fixed << std::setprecision(1);
    json << "{";
    json << "\"timestamp\":\"" << isoTimestampNow() << "\",";
    json << "\"platform\":\"demo\",";
    json << "\"uptimeSeconds\":" << static_cast<int>(seconds) << ",";
    json << "\"loadAverage\":[0.4,0.6,0.8],";
    json << "\"runningProcesses\":3,";
    json << "\"totalProcesses\":128,";
    json << "\"cpu\":{\"total\":" << cpu
         << ",\"cores\":[23.0,41.0,17.0,52.0]},";
    json << "\"memory\":{\"totalKb\":16384000,\"usedKb\":"
         << static_cast<std::uint64_t>(16384000.0 * mem / 100.0)
         << ",\"availableKb\":7000000,\"percent\":" << mem << "},";
    json << "\"swap\":{\"totalKb\":2097152,\"usedKb\":262144,\"percent\":12.5},";
    json << "\"network\":{\"rxBytes\":104857600,\"txBytes\":36700160,"
         << "\"rxRate\":24576,\"txRate\":8192},";
    json << "\"disk\":{\"readBytes\":1099511627776,\"writeBytes\":549755813888,"
         << "\"readRate\":131072,\"writeRate\":65536},";
    json << "\"processes\":[";
    json << "{\"pid\":101,\"user\":\"demo\",\"command\":\"/usr/bin/firefox\","
         << "\"state\":\"S\",\"cpu\":12.5,\"memoryKb\":420000,\"threads\":35},";
    json << "{\"pid\":202,\"user\":\"demo\",\"command\":\"./Kvadra_load_view\","
         << "\"state\":\"R\",\"cpu\":3.4,\"memoryKb\":18000,\"threads\":4}";
    json << "],";
    json << "\"error\":\"Демо-режим: настоящие метрики доступны на Linux и macOS\"";
    json << "}\n";
    return json.str();
}

#endif

}

std::uint64_t CpuTimes::total() const {
    return user + nice + system + idle + iowait + irq + softirq + steal;
}

std::uint64_t CpuTimes::idleTotal() const {
    return idle + iowait;
}

ResourceMonitor::ResourceMonitor() = default;

std::string ResourceMonitor::collectJson() {
    std::lock_guard<std::mutex> lock(mutex_);

#if defined(__linux__)
    // Linux-ветка: собираем все данные из /proc и /sys, затем собираем общий JSON

    const auto processNow = std::chrono::steady_clock::now();
    const std::vector<CpuTimes> cpuTimes = readLinuxCpuTimes();
    const MemoryInfo memory = readLinuxMemoryInfo();
    LoadInfo load = readLinuxLoadInfo();
    const double uptime = readLinuxUptimeSeconds();

    double totalCpu = 0.0;
    std::vector<double> coreUsage;
    updateCpuUsage(cpuTimes, previousCpuTimes_, totalCpu, coreUsage);

    const IoCounters currentIo =
        readLinuxDiskCounters(readLinuxNetworkCounters(IoCounters {}));
    double netRxRate = 0.0;
    double netTxRate = 0.0;
    double diskReadRate = 0.0;
    double diskWriteRate = 0.0;
    updateIoRates(currentIo,
                  previousIoCounters_,
                  previousIoAt_,
                  hasPreviousIo_,
                  netRxRate,
                  netTxRate,
                  diskReadRate,
                  diskWriteRate);

    double processElapsed = 0.0;
    if (hasPreviousProcesses_) {
        processElapsed =
            std::chrono::duration<double>(processNow - previousProcessesAt_).count();
    }

    std::vector<ProcessInfo> processes =
        readLinuxProcesses(previousProcesses_, hasPreviousProcesses_, processElapsed);
    rememberProcessSamples(processes,
                           previousProcesses_,
                           previousProcessesAt_,
                           hasPreviousProcesses_);
    trimProcessesForUi(processes);

    return buildSnapshotJson("linux",
                             uptime,
                             load,
                             totalCpu,
                             coreUsage,
                             memory,
                             currentIo,
                             netRxRate,
                             netTxRate,
                             diskReadRate,
                             diskWriteRate,
                             processes);
#elif defined(__APPLE__)
    // macOS-ветка: используем системные API вместо /proc, структура JSON остается той же

    const auto processNow = std::chrono::steady_clock::now();
    const std::vector<CpuTimes> cpuTimes = readMacCpuTimes();
    const MemoryInfo memory = readMacMemoryInfo();
    LoadInfo load = readMacLoadInfo();
    const double uptime = readMacUptimeSeconds();

    double totalCpu = 0.0;
    std::vector<double> coreUsage;
    updateCpuUsage(cpuTimes, previousCpuTimes_, totalCpu, coreUsage);

    const IoCounters currentIo = readMacDiskCounters(readMacNetworkCounters(IoCounters {}));
    double netRxRate = 0.0;
    double netTxRate = 0.0;
    double diskReadRate = 0.0;
    double diskWriteRate = 0.0;
    updateIoRates(currentIo,
                  previousIoCounters_,
                  previousIoAt_,
                  hasPreviousIo_,
                  netRxRate,
                  netTxRate,
                  diskReadRate,
                  diskWriteRate);

    double processElapsed = 0.0;
    if (hasPreviousProcesses_) {
        processElapsed =
            std::chrono::duration<double>(processNow - previousProcessesAt_).count();
    }

    std::vector<ProcessInfo> processes = readMacProcesses(previousProcesses_,
                                                          hasPreviousProcesses_,
                                                          processElapsed,
                                                          load);
    rememberProcessSamples(processes,
                           previousProcesses_,
                           previousProcessesAt_,
                           hasPreviousProcesses_);
    trimProcessesForUi(processes);

    return buildSnapshotJson("macOS",
                             uptime,
                             load,
                             totalCpu,
                             coreUsage,
                             memory,
                             currentIo,
                             netRxRate,
                             netTxRate,
                             diskReadRate,
                             diskWriteRate,
                             processes);
#else
    // На неподдержанной ОС оставляем UI работоспособным через demo JSON

    return buildDemoJson();
#endif
}
