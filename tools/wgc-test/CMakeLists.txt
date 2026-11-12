cmake_minimum_required(VERSION 3.20)

project(ts3stream_wgc_test LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(wgc_test
    main.cpp
    ../../plugin/src/media/wgc_capture.cpp
    ../../plugin/src/media/desktop_capture.cpp
    ../../plugin/src/log.cpp
)

target_include_directories(wgc_test PRIVATE
    ../../plugin/src
)

target_compile_definitions(wgc_test PRIVATE
    UNICODE
    _UNICODE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    _CRT_SECURE_NO_WARNINGS
)

target_link_libraries(wgc_test PRIVATE
    d3d11
    dxgi
    windowsapp
    runtimeobject
    ole32
)

if (MSVC)
    target_compile_options(wgc_test PRIVATE /W4 /permissive- /Zc:__cplusplus)
endif()
