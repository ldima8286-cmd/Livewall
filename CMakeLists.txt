cmake_minimum_required(VERSION 3.20)

project(LiveWall
    VERSION 0.1.0
    DESCRIPTION "Live wallpapers for the Windows desktop"
    LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR
        "LiveWall is a Windows application.\n"
        "Cross-compile with: cmake -B build -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-mingw64.cmake")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(LIVEWALL_CONSOLE "Build as a console app (useful for debugging)" OFF)
option(LIVEWALL_WERROR "Treat compiler warnings as errors" OFF)
option(LIVEWALL_ASAN "Enable AddressSanitizer (MSVC / Clang)" OFF)

enable_language(RC)

add_executable(livewall
    src/main.cpp
    src/app.cpp
    src/desktop.cpp
    src/renderer.cpp
    src/media.cpp
    src/media_wic.cpp
    src/media_mf.cpp
    src/settings.cpp
    src/util.cpp
    src/log.cpp
    resources/livewall.rc
)

if(NOT LIVEWALL_CONSOLE)
    set_target_properties(livewall PROPERTIES WIN32_EXECUTABLE TRUE)
endif()

target_include_directories(livewall PRIVATE src resources)

target_compile_definitions(livewall PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _CRT_SECURE_NO_WARNINGS
    _WIN32_WINNT=0x0A00
    WINVER=0x0A00
)

if(LIVEWALL_CONSOLE)
    target_compile_definitions(livewall PRIVATE LIVEWALL_CONSOLE=1)
endif()

target_link_libraries(livewall PRIVATE
    d3d11
    dxgi
    ole32
    oleaut32
    uuid
    shlwapi
    shell32
    comdlg32
    windowscodecs
    mfplat
    mfreadwrite
    mfuuid
    user32
    gdi32
    advapi32
    wtsapi32
)

if(MSVC)
    target_compile_options(livewall PRIVATE /W4 /permissive- /utf-8 /EHsc)
    if(LIVEWALL_WERROR)
        target_compile_options(livewall PRIVATE /WX)
    endif()
    target_link_options(livewall PRIVATE /MANIFEST:NO)
    if(LIVEWALL_ASAN)
        target_compile_options(livewall PRIVATE /fsanitize=address)
        target_link_options(livewall PRIVATE /fsanitize=address)
    endif()
else()
    target_compile_options(livewall PRIVATE
        -Wall -Wextra -Wno-unused-parameter -Wno-cast-function-type
        -municode)
    target_link_options(livewall PRIVATE
        -municode
        -static -static-libgcc -static-libstdc++)
    if(LIVEWALL_WERROR)
        target_compile_options(livewall PRIVATE -Werror)
    endif()
endif()

# Deployment helper: copy the built binary next to a runnable folder.
add_custom_command(TARGET livewall POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/dist"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:livewall>" "${CMAKE_BINARY_DIR}/dist/livewall.exe"
    COMMENT "Copying livewall.exe to ${CMAKE_BINARY_DIR}/dist")
