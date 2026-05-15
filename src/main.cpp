#include "http_server.h"
#include "resource_monitor.h"

#include <csignal>
#include <cstdlib>
#include <iostream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace {

// Разбирает необязательный аргумент командной строки: --port 9090
// Если аргумент не передан, приложение слушает стандартный порт 8080

int parsePort(int argc, char* argv[]) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::string(argv[i]) == "--port") {
            return std::atoi(argv[i + 1]);
        }
    }

    return 8080;
}

// Возвращает путь, переданный пользователем через --web-root
// Пустая строка означает, что путь нужно определить автоматически

std::string parseWebRootArgument(int argc, char* argv[]) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::string(argv[i]) == "--web-root") {
            return argv[i + 1];
        }
    }

    return {};
}

// Небольшая обертка нужна для проверки в папку WebUI
bool directoryExists(const std::string& path) {
    struct stat info {};
    return stat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode);
}

// Склеивает две части пути без зависимости от std::filesystem
// Это оставляет проект простым для сборки на Ubuntu 22.04 и macOS

std::string joinPath(const std::string& left, const std::string& right) {
    if (left.empty()) {
        return right;
    }
    if (left.back() == '/') {
        return left + right;
    }

    return left + "/" + right;
}

// Текущая рабочая папка процесса, она может отличаться от папки с бинарником (особенно при запуске из CLion или через абсолютный путь)

std::string currentDirectory() {
    char buffer[4096] {};
    if (getcwd(buffer, sizeof(buffer)) == nullptr) {
        return ".";
    }

    return buffer;
}

// Получает родительскую папку для пути к файлу

std::string directoryName(const std::string& path) {
    const std::size_t slash = path.find_last_of('/');
    if (slash == std::string::npos) {
        return currentDirectory();
    }
    if (slash == 0) {
        return "/";
    }

    return path.substr(0, slash);
}

// Определяет папку, где лежит исполняемый файл
// Это помогает найти web/ рядом с бинарником в build-папке

std::string executableDirectory(const char* argv0) {
    if (argv0 == nullptr || std::string(argv0).empty()) {
        return currentDirectory();
    }

    const std::string path = argv0;
    if (!path.empty() && path.front() == '/') {
        return directoryName(path);
    }

    return directoryName(joinPath(currentDirectory(), path));
}

// Автоматически ищет WebUI в нескольких типичных местах, чтобы приложение запускалось и из корня проекта, и из build-папки

std::string detectWebRoot(const char* argv0) {
    const std::string exeDir = executableDirectory(argv0);

    // Проверяем несколько частых вариантов запуска:
    // 1) из корня проекта
    // 2) из build/cmake-build-debug после копирования CMake
    // 3) из build-папки без копирования WebUI
    // 4) через абсолютный путь к исходникам, который CMake передает при сборке

    std::vector<std::string> candidates = {
        joinPath(currentDirectory(), "web"),
        joinPath(exeDir, "web"),
        joinPath(exeDir, "../web"),
#ifdef KVADRA_SOURCE_WEB_ROOT
        KVADRA_SOURCE_WEB_ROOT,
#endif
    };

    for (const std::string& candidate : candidates) {
        if (directoryExists(candidate)) {
            return candidate;
        }
    }

    // Если ничего не нашли, вернем привычное имя: HttpServer напечатает понятную ошибку

    return "web";
}

} // namespace

int main(int argc, char* argv[]) {

    // Если браузер разорвал соединение, backend не должен падать от SIGPIPE

    std::signal(SIGPIPE, SIG_IGN);

    // Сначала читаем настройки запуска, затем создаем монитор и HTTP-сервер

    const int port = parsePort(argc, argv);
    const std::string webRootArgument = parseWebRootArgument(argc, argv);
    const std::string webRoot = webRootArgument.empty()
                                    ? detectWebRoot(argv[0])
                                    : webRootArgument;

    if (port <= 0 || port > 65535) {
        std::cerr << "Некорректный порт. Используйте диапазон 1..65535.\n";
        return 1;
    }

    try {
        ResourceMonitor monitor;
        HttpServer server(port, webRoot, monitor);

        std::cout << "Kvadra Load View запущен\n";
        std::cout << "Откройте в браузере: http://127.0.0.1:" << port << "\n";
        std::cout << "Папка WebUI: " << webRoot << "\n" << std::flush;

        server.run();
    } catch (const std::exception& error) {
        std::cerr << "Ошибка запуска: " << error.what() << "\n";
        return 1;
    }

    return 0;
}
