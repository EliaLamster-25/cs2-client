cmake_minimum_required(VERSION 3.20)
project(cs2_overlay LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ── Per-configure driver / mapper identity (rotate with: cmake -B build) ───────
string(RANDOM LENGTH 12 ALPHABET "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ" DRV_DEVICE_SUFFIX)
string(RANDOM LENGTH 10 ALPHABET "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ" DRV_MAPPER_STEM)
string(RANDOM LENGTH 10 ALPHABET "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ" DRV_HOST_STEM)
string(RANDOM LENGTH 4  ALPHABET "0123456789ABCDEF" DRV_IOCTL_DEV_HEX)
string(RANDOM LENGTH 3  ALPHABET "0123456789" DRV_IOCTL_FUNC_DEC)
string(RANDOM LENGTH 16 ALPHABET "0123456789ABCDEF" DRV_PING_HEX)

set(DRV_SYS_FILE_NAME "${DRV_DEVICE_SUFFIX}.sys")
set(DRV_MAPPER_EXE_NAME "${DRV_MAPPER_STEM}.exe")
set(DRV_HOST_EXE_NAME "${DRV_HOST_STEM}.exe")
set(DRV_IOCTL_DEVICE_TYPE "0x${DRV_IOCTL_DEV_HEX}")
math(EXPR DRV_IOCTL_FUNC_READ "2048 + ${DRV_IOCTL_FUNC_DEC}")
math(EXPR DRV_IOCTL_FUNC_MODULE "${DRV_IOCTL_FUNC_READ} + 1")
math(EXPR DRV_IOCTL_FUNC_PING "${DRV_IOCTL_FUNC_READ} + 2")
math(EXPR DRV_IOCTL_FUNC_WRITE "${DRV_IOCTL_FUNC_READ} + 3")
math(EXPR DRV_IOCTL_FUNC_PEB "${DRV_IOCTL_FUNC_READ} + 4")
set(DRV_PING_MAGIC "0x${DRV_PING_HEX}")

set(DRV_IDENTITY_GEN_DIR "${CMAKE_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${DRV_IDENTITY_GEN_DIR}")
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/driver_identity.h.in"
    "${DRV_IDENTITY_GEN_DIR}/driver_identity.h"
    @ONLY
)
file(COPY "${DRV_IDENTITY_GEN_DIR}/driver_identity.h"
     DESTINATION "${CMAKE_CURRENT_SOURCE_DIR}/generated")
file(COPY "${DRV_IDENTITY_GEN_DIR}/driver_identity.h"
     DESTINATION "${CMAKE_CURRENT_SOURCE_DIR}/PooDriver")
message(STATUS "Driver identity: device=${DRV_DEVICE_SUFFIX} sys=${DRV_SYS_FILE_NAME} mapper=${DRV_MAPPER_EXE_NAME} exe=${DRV_HOST_STEM}")
message(STATUS "After cmake configure: rebuild kernel driver (PooDriver/), then rebuild overlay.")

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

FetchContent_Declare(
    cgltf
    GIT_REPOSITORY https://github.com/jkuhlmann/cgltf.git
    GIT_TAG        v1.14
)
FetchContent_MakeAvailable(cgltf)

set(SOURCES
    src/main.cpp
    src/config.cpp
    ${CMAKE_SOURCE_DIR}/loader/src/launch_handshake.cpp
    src/memory/process.cpp
    src/memory/driver_manager.cpp
    src/memory/kernel_memory.cpp
    src/input/input_router.cpp
    src/memory/pattern_scan.cpp
    src/offsets/offsets.cpp
    src/overlay/window.cpp
    src/overlay/renderer.cpp
    src/overlay/overlay_metrics.cpp
    src/game/entity_manager.cpp
    src/game/triggerbot.cpp
    src/game/aim_assist.cpp
    src/debug/aim_debug.cpp
    src/debug/overlay_log.cpp
    src/game/aim_style.cpp
    src/game/weapon_group.cpp
    src/game/grenade_lineup.cpp
    src/game/game_trace.cpp
    src/esp/esp_renderer.cpp
    src/esp/grenade_helper.cpp
    src/esp/sound_esp.cpp
    src/esp/chams_mesh.cpp
    src/cloud/cloud_api.cpp
    src/menu/menu.cpp
    src/gui/font.cpp
    src/gui/gui.cpp
    src/gui/icons.cpp
    src/analytics/match_intel.cpp
    src/analytics/player_scout.cpp
    src/profile/user_profile.cpp
    src/bsp/bsp_world.cpp
    src/web/web_radar_publisher.cpp
    src/steam/steam_avatars.cpp
    src/lz4.c
)

add_executable(${PROJECT_NAME} WIN32 ${SOURCES})

set_target_properties(${PROJECT_NAME} PROPERTIES OUTPUT_NAME "${DRV_HOST_STEM}")

target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/generated
    ${CMAKE_SOURCE_DIR}/loader/include
    ${DRV_IDENTITY_GEN_DIR}
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
    ${zstd_SOURCE_DIR}/lib
    ${cgltf_SOURCE_DIR}
)

target_compile_definitions(${PROJECT_NAME} PRIVATE
    $<$<CONFIG:Debug>:OVERLAY_DEBUG_LOG>
    $<$<CONFIG:Debug>:OVERLAY_ALLOW_STANDALONE=1>
    $<$<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>,$<CONFIG:MinSizeRel>>:CRYMORE_REQUIRE_LAUNCH_GATE=1>
)

if(MSVC)
    target_link_options(${PROJECT_NAME} PRIVATE
        "/MANIFESTUAC:level='requireAdministrator' uiAccess='false'")
endif()

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
    bcrypt
    shell32
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
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>/assets/brand_logo.png"
    )
endif()

set(AVATAR_PLACEHOLDER_ASSET "${CMAKE_CURRENT_SOURCE_DIR}/picture placeholder.png")
if(EXISTS "${AVATAR_PLACEHOLDER_ASSET}")
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${AVATAR_PLACEHOLDER_ASSET}"
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>/assets/picture_placeholder.png"
    )
endif()

set(LEETIFY_BADGE_ASSET "${CMAKE_CURRENT_SOURCE_DIR}/Leetify Badge Black Small.png")
set(LEETIFY_BADGE_ASSET_FALLBACK "${CMAKE_CURRENT_SOURCE_DIR}/assets/leetify_badge.png")
if(EXISTS "${LEETIFY_BADGE_ASSET}")
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${LEETIFY_BADGE_ASSET}"
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>/assets/leetify_badge.png"
    )
elseif(EXISTS "${LEETIFY_BADGE_ASSET_FALLBACK}")
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${LEETIFY_BADGE_ASSET_FALLBACK}"
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>/assets/leetify_badge.png"
    )
endif()

set(ZOOM_IN_ICON_ASSET "${CMAKE_CURRENT_SOURCE_DIR}/zoom-in.png")
set(ZOOM_IN_ICON_FALLBACK "${CMAKE_CURRENT_SOURCE_DIR}/assets/zoom-in.png")
if(EXISTS "${ZOOM_IN_ICON_ASSET}")
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${ZOOM_IN_ICON_ASSET}"
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>/assets/zoom-in.png"
    )
elseif(EXISTS "${ZOOM_IN_ICON_FALLBACK}")
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${ZOOM_IN_ICON_FALLBACK}"
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>/assets/zoom-in.png"
    )
endif()

set(TAB_ICON_SRCS Aim.png ESP.png World.png Intel.png config.png)
set(TAB_ICON_DSTS tab0.png tab1.png tab2.png tab4.png tab5.png)
foreach(tab_src tab_dst IN ZIP_LISTS TAB_ICON_SRCS TAB_ICON_DSTS)
    set(TAB_ICON_SRC "${CMAKE_CURRENT_SOURCE_DIR}/assets/${tab_src}")
    if(EXISTS "${TAB_ICON_SRC}")
        add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${TAB_ICON_SRC}"
            "$<TARGET_FILE_DIR:${PROJECT_NAME}>/assets/${tab_dst}"
        )
    endif()
endforeach()
set(TAB_ICON_MISC_SRC "${CMAKE_CURRENT_SOURCE_DIR}/assets/misc_system.png")
if(NOT EXISTS "${TAB_ICON_MISC_SRC}")
    set(TAB_ICON_MISC_SRC "${CMAKE_CURRENT_SOURCE_DIR}/assets/misc; system.png")
endif()
if(EXISTS "${TAB_ICON_MISC_SRC}")
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${TAB_ICON_MISC_SRC}"
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>/assets/tab3.png"
    )
endif()

set(STEAM_API_DLL "${CMAKE_CURRENT_SOURCE_DIR}/steam_api64.dll")
set(STEAM_APPID_TXT "${CMAKE_CURRENT_SOURCE_DIR}/steam_appid.txt")
if(EXISTS "${STEAM_API_DLL}")
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${STEAM_API_DLL}"
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>/steam_api64.dll"
    )
endif()
if(EXISTS "${STEAM_APPID_TXT}")
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${STEAM_APPID_TXT}"
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>/steam_appid.txt"
    )
endif()

foreach(_icon_font csgo_icons.ttf csgo_icons_outline.ttf)
    set(_icon_font_path "${CMAKE_CURRENT_SOURCE_DIR}/${_icon_font}")
    if(EXISTS "${_icon_font_path}")
        add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_icon_font_path}"
            "$<TARGET_FILE_DIR:${PROJECT_NAME}>/${_icon_font}"
        )
    endif()
endforeach()

set(T_BOT_JPEG "${CMAKE_CURRENT_SOURCE_DIR}/t_bot.jpeg")
set(CT_BOT_JPEG "${CMAKE_CURRENT_SOURCE_DIR}/ct_bot.jpeg")
if(EXISTS "${T_BOT_JPEG}")
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${T_BOT_JPEG}"
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>/t_bot.jpeg"
    )
endif()
if(EXISTS "${CT_BOT_JPEG}")
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CT_BOT_JPEG}"
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>/ct_bot.jpeg"
    )
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/meshes")
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/meshes"
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>/meshes"
    )
endif()

set(KERNEL_DRIVER_SYS "${CMAKE_CURRENT_SOURCE_DIR}/PooDriver/x64/Release/MemReaderKdmp.sys")
if(NOT EXISTS "${KERNEL_DRIVER_SYS}")
    set(KERNEL_DRIVER_SYS "${CMAKE_CURRENT_SOURCE_DIR}/PooDriver/MemReaderKdmp.sys")
endif()
if(EXISTS "${KERNEL_DRIVER_SYS}")
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${KERNEL_DRIVER_SYS}"
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>/${DRV_SYS_FILE_NAME}"
    )
endif()

set(KDMAPPER_RELEASE_EXE "${CMAKE_CURRENT_SOURCE_DIR}/kdmapper-master/x64/Release/kdmapper_Release.exe")
set(KDMAPPER_EXE "${CMAKE_CURRENT_SOURCE_DIR}/kdmapper-master/x64/Release/kdmapper.exe")
if(EXISTS "${KDMAPPER_RELEASE_EXE}")
    set(_mapper_src "${KDMAPPER_RELEASE_EXE}")
elseif(EXISTS "${KDMAPPER_EXE}")
    set(_mapper_src "${KDMAPPER_EXE}")
endif()
if(DEFINED _mapper_src)
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${_mapper_src}"
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>/${DRV_MAPPER_EXE_NAME}"
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

    # ── Performance: aggressive optimization for optimized configs ──────────────
    # Applied only to Release / RelWithDebInfo / MinSizeRel so debug builds keep
    # their debuggability.  These flags accelerate the float-heavy BSP sweep and
    # entity math without altering results:
    #   /O2 /Oi /Ot  – max speed, intrinsics, favour fast code
    #   /Gy          – function-level linking (lets /OPT:ICF/REF trim the binary)
    #   /arch:AVX2   – use AVX2/FMA codegen (ubiquitous on any CS2-capable CPU)
    #   /GL + /LTCG  – whole-program optimization / cross-TU inlining
    # NOTE: /fp:fast is deliberately NOT used — the memory-read validation relies
    # on std::isfinite() to reject garbage (NaN/Inf) pointers and positions.
    set(_opt_cfgs $<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>,$<CONFIG:MinSizeRel>>)
    target_compile_options(${PROJECT_NAME} PRIVATE
        $<${_opt_cfgs}:/O2>
        $<${_opt_cfgs}:/Oi>
        $<${_opt_cfgs}:/Ot>
        $<${_opt_cfgs}:/Gy>
        $<${_opt_cfgs}:/arch:AVX2>
        $<${_opt_cfgs}:/GL>)
    target_link_options(${PROJECT_NAME} PRIVATE
        $<${_opt_cfgs}:/LTCG>
        $<${_opt_cfgs}:/OPT:REF>
        $<${_opt_cfgs}:/OPT:ICF>
        $<${_opt_cfgs}:/DEBUG:NONE>)

    # Match optimization level on the ImGui static lib so menu rendering is fast
    # too and so LTCG can inline across it.
    target_compile_options(imgui_lib PRIVATE
        $<${_opt_cfgs}:/O2>
        $<${_opt_cfgs}:/arch:AVX2>
        $<${_opt_cfgs}:/GL>)
endif()

option(BUILD_CRYMORE_LOADER "Build crymore.pw distribution loader" ON)
if(BUILD_CRYMORE_LOADER)
    add_subdirectory(loader)
endif()
