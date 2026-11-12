cmake_minimum_required(VERSION 3.20)

project(ts3stream_diag LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Qt5 5.15.2 COMPONENTS Core Gui Widgets Network REQUIRED)

set(LIVEKIT_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/livekit")
if (NOT EXISTS "${LIVEKIT_SDK_DIR}/lib/livekit.lib")
    message(FATAL_ERROR "LiveKit C++ SDK not found at ${LIVEKIT_SDK_DIR}. Run tools/setup-livekit.ps1 first.")
endif()

add_library(ts3stream_diag SHARED
    src/plugin.cpp
    src/log.cpp
    src/diag/widget_dump.cpp
    src/diag/ui_probe.cpp
    src/media/wgc_capture.cpp
    src/media/window_enumerator.cpp
    src/media/audio_devices.cpp
    src/media/audio_renderer.cpp
    src/media/audio_pipe_client.cpp
    src/media/livekit_engine.cpp
    src/ui/stream_panel.cpp
    src/ui/live_badge_delegate.cpp
    src/ui/live_stream_menu.cpp
    src/ui/active_streams_button.cpp
    src/ui/source_picker.cpp
    src/ui/frame_processor.cpp
    src/ui/control_client.cpp
    src/ui/stream_ui.cpp
    src/ui/fullscreen_window.cpp
)

target_include_directories(ts3stream_diag PRIVATE
    src
    third_party/ts3sdk/include
    ${LIVEKIT_SDK_DIR}/include
)

target_link_libraries(ts3stream_diag PRIVATE
    Qt5::Core
    Qt5::Gui
    Qt5::Widgets
    Qt5::Network
    d3d11
    dxgi
    dwmapi
    windowsapp
    runtimeobject
    ole32
    mmdevapi
    propsys
    uuid
    winmm
    "${LIVEKIT_SDK_DIR}/lib/livekit.lib"
    delayimp
)

target_compile_definitions(ts3stream_diag PRIVATE
    UNICODE
    _UNICODE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    _CRT_SECURE_NO_WARNINGS
)

set_target_properties(ts3stream_diag PROPERTIES
    PREFIX ""
    OUTPUT_NAME "ts3stream_diag"
)

if (MSVC)
    target_compile_options(ts3stream_diag PRIVATE /W4 /permissive- /Zc:__cplusplus /utf-8)
    target_link_options(ts3stream_diag PRIVATE /INCREMENTAL:NO /DELAYLOAD:livekit.dll)
endif()

option(TS3STREAM_BUILD_UI_TESTS "Build interactive native fullscreen regression test" OFF)
if (TS3STREAM_BUILD_UI_TESTS)
    add_executable(viewer_controls_test tests/viewer_controls_test.cpp src/ui/stream_panel.cpp
        src/ui/active_streams_button.cpp src/ui/live_badge_delegate.cpp)
    target_include_directories(viewer_controls_test PRIVATE src)
    target_link_libraries(viewer_controls_test PRIVATE Qt5::Widgets)
    if (MSVC)
        target_compile_options(viewer_controls_test PRIVATE /utf-8)
    endif()
    add_executable(live_stream_menu_test tests/live_stream_menu_test.cpp src/ui/live_stream_menu.cpp)
    target_include_directories(live_stream_menu_test PRIVATE src)
    target_link_libraries(live_stream_menu_test PRIVATE Qt5::Widgets)
    if (MSVC)
        target_compile_options(live_stream_menu_test PRIVATE /utf-8)
    endif()
    add_executable(fullscreen_window_test
        tests/fullscreen_window_test.cpp
        src/ui/fullscreen_window.cpp
        src/ui/stream_panel.cpp
    )
    target_include_directories(fullscreen_window_test PRIVATE src)
    target_link_libraries(fullscreen_window_test PRIVATE Qt5::Widgets user32)
    target_compile_definitions(fullscreen_window_test PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    if (MSVC)
        target_compile_options(fullscreen_window_test PRIVATE /utf-8)
    endif()
    add_executable(stream_ended_test
        tests/stream_ended_test.cpp
        src/media/livekit_engine.cpp
        src/log.cpp
        src/ui/fullscreen_window.cpp
        src/ui/stream_panel.cpp
    )
    target_include_directories(stream_ended_test PRIVATE src "${LIVEKIT_SDK_DIR}/include")
    target_link_libraries(stream_ended_test PRIVATE Qt5::Widgets user32
        "${LIVEKIT_SDK_DIR}/lib/livekit.lib" delayimp)
    target_compile_definitions(stream_ended_test PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    if (MSVC)
        target_compile_options(stream_ended_test PRIVATE /utf-8)
        target_link_options(stream_ended_test PRIVATE /DELAYLOAD:livekit.dll)
    endif()
endif()

# Separate helper process for WASAPI process-loopback. It must not run inside
# TS3 because excluding the current process from system audio is illegal.
add_executable(ts3stream_audio_helper
    ../tools/audio-helper/main.cpp
    src/media/wasapi_capture.cpp
    src/log.cpp
)

target_include_directories(ts3stream_audio_helper PRIVATE src)
target_link_libraries(ts3stream_audio_helper PRIVATE mmdevapi ole32 uuid)
target_compile_definitions(ts3stream_audio_helper PRIVATE
    UNICODE
    _UNICODE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    _CRT_SECURE_NO_WARNINGS
)
set_target_properties(ts3stream_audio_helper PROPERTIES
    OUTPUT_NAME "ts3stream-audio"
    PREFIX ""
)
if (MSVC)
    target_compile_options(ts3stream_audio_helper PRIVATE /W4 /permissive- /utf-8)
endif()
