cmake_minimum_required(VERSION 3.20)
project(disk_statistix LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG v1.92.1
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(imgui)

add_executable(disk_statistix WIN32
    src/main.cpp
    src/app.cpp
    src/scanner.cpp
    src/dupes.cpp
    src/media.cpp
    src/app.rc
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp)

target_include_directories(disk_statistix PRIVATE ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_compile_definitions(disk_statistix PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(disk_statistix PRIVATE d3d11 d3dcompiler dxgi shell32 ole32 mfplat mfreadwrite mfuuid xaudio2)

if(MSVC)
    target_compile_options(disk_statistix PRIVATE /utf-8 /W3)
endif()
