cmake_minimum_required(VERSION 3.20)
project(cs2_overlay LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        v1.91.9
)
FetchContent_MakeAvailable(imgui)

FetchContent_Declare(
    zstd
    GIT_REPOSITORY https://github.com/facebook/zstd.git
    GIT_TAG        v1.5.6
    SOURCE_SUBDIR  build/cmake
)
set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_STATIC   ON  CACHE BOOL "" FORCE)
set(ZSTD_BUILD_SHARED   OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(zstd)

set(SOURCES
    src/main.cpp
    src/config.cpp
    src/memory/process.cpp
    src/offsets/offsets.cpp
    src/overlay/window.cpp
    src/overlay/renderer.cpp
    src/game/entity_manager.cpp
    src/game/triggerbot.cpp
    src/game/aim_assist.cpp
    src/esp/esp_renderer.cpp
    src/menu/menu.cpp
    src/gui/font.cpp
    src/gui/gui.cpp
    src/gui/icons.cpp
    src/analytics/match_intel.cpp
    src/bsp/bsp_world.cpp
    src/web/web_radar_publisher.cpp
    src/lz4.c
)

add_executable(${PROJECT_NAME} WIN32 ${SOURCES})

target_include_directories(${PROJECT_NAME} PRIVATE 
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
    ${zstd_SOURCE_DIR}/lib
)

target_link_libraries(${PROJECT_NAME} PRIVATE
    d3d11
    d3dcompiler
    dxgi
    dwmapi
    windowscodecs
    winhttp
    winmm
    gdi32
    libzstd_static
)

set(HITBOX_MODEL_ASSET "${CMAKE_CURRENT_SOURCE_DIR}/assets/hitbox playermodel.png")
set(ESP_MODEL_ASSET "${CMAKE_CURRENT_SOURCE_DIR}/assets/esp playermodel.png")
set(BRAND_LOGO_ASSET "${CMAKE_CURRENT_SOURCE_DIR}/assets/crymore_logo.png")
if(NOT EXISTS "${BRAND_LOGO_ASSET}")
    set(BRAND_LOGO_ASSET "${CMAKE_CURRENT_SOURCE_DIR}/assets/logo.png")
endif()
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/assets/crymore logo.png")
    set(BRAND_LOGO_ASSET "${CMAKE_CURRENT_SOURCE_DIR}/assets/crymore logo.png")
endif()
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${PROJECT_NAME}>/assets"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${HITBOX_MODEL_ASSET}"
    "$<TARGET_FILE_DIR:${PROJECT_NAME}>/assets/hitbox playermodel.png"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${ESP_MODEL_ASSET}"
    "$<TARGET_FILE_DIR:${PROJECT_NAME}>/assets/esp playermodel.png"
)

if(EXISTS "${BRAND_LOGO_ASSET}")
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${BRAND_LOGO_ASSET}"
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>/assets/crymore logo.png"
    )
endif()

# Build ImGui separately to speed up our compiles
add_library(imgui_lib STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp
)
target_include_directories(imgui_lib PUBLIC ${imgui_SOURCE_DIR})
target_link_libraries(${PROJECT_NAME} PRIVATE imgui_lib)

if(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE /W3 /permissive- /Zc:__cplusplus)
endif()
