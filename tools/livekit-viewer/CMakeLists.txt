cmake_minimum_required(VERSION 3.20)

project(ts3stream_livekit_viewer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Qt5 5.15.2 COMPONENTS Core Gui Widgets REQUIRED)

set(LIVEKIT_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../plugin/third_party/livekit")
if (NOT EXISTS "${LIVEKIT_SDK_DIR}/lib/livekit.lib")
    message(FATAL_ERROR "LiveKit C++ SDK not found at ${LIVEKIT_SDK_DIR}")
endif()

add_executable(livekit_viewer main.cpp)

target_include_directories(livekit_viewer PRIVATE
    ${LIVEKIT_SDK_DIR}/include
)

target_link_libraries(livekit_viewer PRIVATE
    Qt5::Core
    Qt5::Gui
    Qt5::Widgets
    "${LIVEKIT_SDK_DIR}/lib/livekit.lib"
)

target_compile_definitions(livekit_viewer PRIVATE
    UNICODE
    _UNICODE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    _CRT_SECURE_NO_WARNINGS
)

if (MSVC)
    target_compile_options(livekit_viewer PRIVATE /W3 /utf-8)
    target_link_options(livekit_viewer PRIVATE /INCREMENTAL:NO)
endif()
