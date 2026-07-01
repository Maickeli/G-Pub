cmake_minimum_required(VERSION 3.20)
project(GPub VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(GPUB_WARNINGS_AS_ERRORS "Treat warnings as errors" OFF)

add_library(gpub_core
    src/core/backend_factory.cpp
    src/core/config_loader.cpp
    src/core/logger.cpp
    src/core/orchestrator.cpp
    src/core/platform_factory.cpp
    src/core/rules_engine.cpp
    src/backends/corsair/corsair_status.cpp
    src/backends/logitech/logitech_backend.cpp
    src/backends/wooting/wooting_backend.cpp
)

target_include_directories(gpub_core PUBLIC include)

if(WIN32)
    target_sources(gpub_core PRIVATE
        src/platform/windows/win32_window_helpers.cpp
        src/platform/windows/windows_foreground_window_provider.cpp
    )
    target_compile_definitions(gpub_core PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(gpub_core PRIVATE user32 hid setupapi)
endif()

if(MSVC)
    target_compile_options(gpub_core PRIVATE /W4 /permissive-)
else()
    target_compile_options(gpub_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(GPUB_WARNINGS_AS_ERRORS)
    if(MSVC)
        target_compile_options(gpub_core PRIVATE /WX)
    else()
        target_compile_options(gpub_core PRIVATE -Werror)
    endif()
endif()

add_executable(gpubd src/cli/gpubd_main.cpp)
target_link_libraries(gpubd PRIVATE gpub_core)

add_executable(gpubctl src/cli/gpubctl_main.cpp)
target_link_libraries(gpubctl PRIVATE gpub_core)

if(WIN32)
    add_executable(gpub_tray WIN32 src/tray/gpub_tray_main.cpp)
    target_compile_definitions(gpub_tray PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(gpub_tray PRIVATE gpub_core shell32 user32 gdi32)
endif()

file(GLOB GPUB_EXAMPLE_CONFIG_FILES CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/config/examples/*")

if(GPUB_EXAMPLE_CONFIG_FILES)
    add_custom_target(gpub_sync_example_configs ALL
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:gpubd>/config/examples"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${GPUB_EXAMPLE_CONFIG_FILES}
            "$<TARGET_FILE_DIR:gpubd>/config/examples"
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:gpubctl>/config/examples"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${GPUB_EXAMPLE_CONFIG_FILES}
            "$<TARGET_FILE_DIR:gpubctl>/config/examples"
        COMMENT "Syncing example config files into runtime output directories"
        VERBATIM
    )
    add_dependencies(gpubd gpub_sync_example_configs)
    add_dependencies(gpubctl gpub_sync_example_configs)
endif()
