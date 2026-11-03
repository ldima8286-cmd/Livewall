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
    LIVEWALL_VERSION="${PROJECT_VERSION}"
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

# Deployment helper: stage everything a release needs in build/dist.
add_custom_command(TARGET livewall POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/dist"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:livewall>" "${CMAKE_BINARY_DIR}/dist/livewall.exe"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/README.md" "${CMAKE_BINARY_DIR}/dist/README.md"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE" "${CMAKE_BINARY_DIR}/dist/LICENSE"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/resources/portable-settings.ini"
        "${CMAKE_BINARY_DIR}/dist/settings.ini"
    COMMENT "Staging livewall.exe, README.md, LICENSE and settings.ini in ${CMAKE_BINARY_DIR}/dist")

# Portable ZIP: unzip anywhere, drop a livewall.ini next to livewall.exe to keep
# the settings in that folder.
add_custom_target(portable
    COMMAND ${CMAKE_COMMAND} -E tar cfv
        "${CMAKE_BINARY_DIR}/dist/LiveWall-${PROJECT_VERSION}-portable.zip"
        --format=zip livewall.exe README.md LICENSE settings.ini
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/dist"
    DEPENDS livewall
    COMMENT "Packing the portable build"
    VERBATIM)

# Installer: requires Inno Setup 6 (ISCC on PATH or in the default folder).
find_program(LIVEWALL_ISCC
    NAMES ISCC iscc
    HINTS "$ENV{ProgramFiles\(x86\)}/Inno Setup 6" "$ENV{ProgramFiles}/Inno Setup 6")

if(LIVEWALL_ISCC)
    add_custom_target(installer
        COMMAND "${LIVEWALL_ISCC}"
            "/DAppVersion=${PROJECT_VERSION}"
            "/DSourceDir=${CMAKE_BINARY_DIR}/dist"
            "/DOutputDir=${CMAKE_BINARY_DIR}/dist"
            "${CMAKE_CURRENT_SOURCE_DIR}/installer/livewall.iss"
        DEPENDS livewall
        COMMENT "Building the LiveWall installer"
        VERBATIM)
else()
    message(STATUS "Inno Setup (ISCC) not found - the 'installer' target is disabled")
endif()
