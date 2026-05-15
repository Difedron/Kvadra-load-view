cmake_minimum_required(VERSION 3.16)
project(Kvadra_load_view VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)

add_executable(Kvadra_load_view
        src/main.cpp
        src/http_server.cpp
        src/resource_monitor.cpp
)

target_include_directories(Kvadra_load_view PRIVATE src)
target_link_libraries(Kvadra_load_view PRIVATE Threads::Threads)
target_compile_definitions(Kvadra_load_view
        PRIVATE KVADRA_SOURCE_WEB_ROOT="${CMAKE_SOURCE_DIR}/web"
)

if (APPLE)
    target_link_libraries(Kvadra_load_view PRIVATE
            "-framework CoreFoundation"
            "-framework IOKit"
    )
endif ()

if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
    target_compile_options(Kvadra_load_view PRIVATE -Wall -Wextra -pedantic)
endif ()

# Копируем WebUI рядом с бинарником, чтобы приложение можно было запускать из build-папки.
# Отдельная ALL-цель нужна, чтобы изменения HTML/CSS/JS попадали в build даже без пересборки C++.
add_custom_target(copy_web ALL
        COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/web
        $<TARGET_FILE_DIR:Kvadra_load_view>/web
)
add_dependencies(copy_web Kvadra_load_view)
