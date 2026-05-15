# Kvadra Load View

Тестовое задание для команды kvadraOS Desktop (Команда 2).

Учебное приложение для отображения загрузки ресурсов Linux и macOS в стиле `top`/`htop`.

Backend написан на C++17 и читает системные метрики через API операционной системы.
Frontend сделан как WebUI: `HTML + CSS + TypeScript`. Готовый для браузера файл
`web/app.js` лежит рядом с исходником `web/app.ts`.


- Приложение отображает загрузку ресурсов PC наподобие `top`/`htop`.
- Frontend реализован как WebUI: `web/index.html`, `web/styles.css`,
  `web/app.ts`.
- Backend реализован на C++17: `src/main.cpp`, `src/http_server.cpp`,
  `src/resource_monitor.cpp`.
- IPC выбран в виде локального HTTP API между браузером и C++ backend.
- Данные обновляются динамически: frontend запрашивает `/api/stats` раз в секунду.
- Linux поддерживается через чтение `/proc` и `/sys`; сборка рассчитана на
  Ubuntu 22.04.5 LTS.
- Дополнительно реализована поддержка macOS через системные API, потому что
  разработка и первичное тестирование выполнялись на macOS-устройстве. Это
  позволяет проверять интерфейс и backend локально, не нарушая основное
  требование о работе в Linux VM.

## Что показывает приложение

- общую загрузку CPU и загрузку по ядрам;
- использование RAM и swap;
- load average и количество процессов;
- скорость чтения/записи диска;
- скорость приема/передачи по сети;
- таблицу top-процессов по CPU/RAM.

## Архитектура

Приложение состоит из двух частей:

- `src/` - C++ backend, локальный HTTP-сервер и сбор системных метрик;
- `web/` - WebUI, который отображает данные в браузере.

IPC между UI и backend реализован через локальный HTTP API:

- `GET /` - интерфейс;
- `GET /api/stats` - актуальный JSON-снимок системы;
- `GET /api/health` - проверка, что backend жив.

Такой вариант удобен для проверки: не нужны Node.js или npm.

Поддержка macOS добавлена как вспомогательный режим для разработки: структура
JSON и WebUI остаются такими же, как на Linux, а различается только слой сбора
системных метрик. Это позволяет отлаживать приложение на домашней macOS-системе
и затем собирать его в Ubuntu 22.04.5 LTS без изменения frontend-кода.

## Используемые технологии

- C++17;
- POSIX sockets для локального HTTP-сервера;
- `/proc` и `/sys` для Linux-метрик;
- `sysctl`, `mach`, `libproc`, `getifaddrs`, `IOKit` для macOS-метрик;
- HTML, CSS и TypeScript для WebUI.

Внешние frontend/backend фреймворки не используются. Это сделано специально,
чтобы проект проще собирался в чистой Ubuntu VM.

## Сборка на Ubuntu 22.04 LTS

```bash
sudo apt update
sudo apt install -y build-essential cmake

cmake -S . -B build
cmake --build build
```

## Сборка на macOS

Нужны Command Line Tools:

```bash
xcode-select --install

cmake -S . -B build
cmake --build build
```

CMake сам подключает системные фреймворки `CoreFoundation` и `IOKit` для
получения macOS-метрик.

## Запуск

```bash
./build/Kvadra_load_view
```

После запуска откройте в браузере:

```text
http://127.0.0.1:8080
```

Порт можно изменить:

```bash
./build/Kvadra_load_view --port 9090
```

Если приложение запускается не из build-папки, можно явно указать путь к WebUI:

```bash
./build/Kvadra_load_view --web-root ./web
```



## Структура проекта

```text
.
├── CMakeLists.txt
├── Kvadra_load_view_source.tar.gz
├── README.md
├── src
│   ├── http_server.cpp
│   ├── http_server.h
│   ├── main.cpp
│   ├── resource_monitor.cpp
│   └── resource_monitor.h
└── web
    ├── app.js
    ├── app.ts
    ├── index.html
    └── styles.css
```

## Примечания

- На Linux используются настоящие данные из `/proc/stat`, `/proc/meminfo`,
  `/proc/loadavg`, `/proc/net/dev`, `/proc/diskstats` и `/proc/<pid>/...`.
- На macOS используются `sysctl`, `mach`, `libproc`, `getifaddrs` и `IOKit`.
  Эта поддержка добавлена для удобства разработки и демонстрации на устройстве,
  где велось тестирование; целевая проверочная среда Linux при этом остается
  основной.
- На других системах backend собирается в демо-режиме, чтобы можно было
  проверить интерфейс.
