cmake_minimum_required(VERSION 3.20)

set(LOADER_TARGET crymore_loader)
set(CORE_TARGET crymore_core)

set(_SRC_ROOT "${CMAKE_SOURCE_DIR}/src")

# ── Shared payload / logo staging ────────────────────────────────────────────

set(_LOADER_LOGO "${CMAKE_SOURCE_DIR}/assets/crymore_logo.png")
if(NOT EXISTS "${_LOADER_LOGO}")
    set(_LOADER_LOGO "${CMAKE_SOURCE_DIR}/assets/logo.png")
endif()
if(EXISTS "${CMAKE_SOURCE_DIR}/assets/crymore logo.png")
    set(_LOADER_LOGO "${CMAKE_SOURCE_DIR}/assets/crymore logo.png")
endif()

set(_LOADER_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${_LOADER_GEN_DIR}")

set(_PAYLOAD_PLAIN "${_LOADER_GEN_DIR}/payload_plain.bin")
set(_PAYLOAD_BIN "${_LOADER_GEN_DIR}/payload.bin")
set(_LOGO_COPY "${_LOADER_GEN_DIR}/brand_logo.png")

option(CRYMORE_SERVER_PAYLOAD "Embed empty payload stub; overlay delivered from server at launch" OFF)

if(EXISTS "${_LOADER_LOGO}")
    configure_file("${_LOADER_LOGO}" "${_LOGO_COPY}" COPYONLY)
else()
    file(WRITE "${_LOGO_COPY}" "")
endif()

if(CRYMORE_SERVER_PAYLOAD)
    add_custom_command(
        OUTPUT "${_PAYLOAD_PLAIN}"
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/tools/write_empty_payload.ps1"
            -OutBin "${_PAYLOAD_PLAIN}"
        COMMENT "Writing empty payload stub (server-delivered overlay)"
        VERBATIM
    )
else()
    add_custom_command(
        OUTPUT "${_PAYLOAD_PLAIN}"
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/tools/pack_payload.ps1"
            -ReleaseDir "$<TARGET_FILE_DIR:${PROJECT_NAME}>"
            -OutBin "${_PAYLOAD_PLAIN}"
        DEPENDS ${PROJECT_NAME}
        COMMENT "Packing overlay release into payload archive"
        VERBATIM
    )
endif()

add_custom_command(
    OUTPUT "${_PAYLOAD_BIN}"
    COMMAND powershell -NoProfile -ExecutionPolicy Bypass
        -File "${CMAKE_CURRENT_SOURCE_DIR}/tools/encrypt_payload.ps1"
        -InBin "${_PAYLOAD_PLAIN}"
        -OutBin "${_PAYLOAD_BIN}"
    DEPENDS "${_PAYLOAD_PLAIN}"
    COMMENT "Encrypting payload archive (PAYLOAD_KEY or loader/.payload_key)"
    VERBATIM
)

add_custom_target(crymore_loader_payload DEPENDS "${_PAYLOAD_BIN}")

function(_loader_stage_resources target)
    set_source_files_properties(resources/loader.rc PROPERTIES
        OBJECT_DEPENDS "${_PAYLOAD_BIN};${_LOGO_COPY}"
        VS_RESOURCE_GENERATOR "rc"
    )
    add_dependencies(${target} crymore_loader_payload ${PROJECT_NAME})
    add_custom_command(TARGET ${target} PRE_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_PAYLOAD_BIN}" "${CMAKE_CURRENT_SOURCE_DIR}/resources/payload.bin"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_LOGO_COPY}" "${CMAKE_CURRENT_SOURCE_DIR}/resources/brand_logo.png"
        COMMENT "Staging loader resources for ${target}"
    )
endfunction()

function(_loader_backend_sources out_var)
    set(${out_var}
        src/loader_app.cpp
        src/debug_log.cpp
        src/env_check.cpp
        src/steam_launch.cpp
        src/log_sanitize.cpp
        src/auth.cpp
        src/protect.cpp
        src/payload.cpp
        src/payload_crypto.cpp
        src/launch_handshake.cpp
        src/loader_config.cpp
        resources/loader.rc
        PARENT_SCOPE
    )
endfunction()

function(_loader_common_target_props target)
    target_include_directories(${target} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${_SRC_ROOT}
        ${CMAKE_CURRENT_BINARY_DIR}/generated
        ${CMAKE_SOURCE_DIR}/generated
    )
    target_link_libraries(${target} PRIVATE
        winhttp
        bcrypt
        advapi32
        shell32
    )
    target_compile_definitions(${target} PRIVATE
        LOADER_BUILD=1
        IDR_PAYLOAD=101
        IDR_BRAND_LOGO=102
        $<$<CONFIG:Debug>:LOADER_DEV_LAUNCH=1>
        $<$<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>,$<CONFIG:MinSizeRel>>:CRYMORE_RELEASE=1>
    )
    if(MSVC)
        target_compile_options(${target} PRIVATE /W3 /permissive- /Zc:__cplusplus)
        set(_opt_cfgs $<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>,$<CONFIG:MinSizeRel>>)
        target_compile_options(${target} PRIVATE
            $<${_opt_cfgs}:/O2 /Oi /Gy /GL>)
        target_link_options(${target} PRIVATE
            $<${_opt_cfgs}:/LTCG /OPT:REF /OPT:ICF>)
    endif()
    _loader_stage_resources(${target})
endfunction()

# ── Native core DLL (C# frontend backend) ──────────────────────────────────────

option(BUILD_LOADER_WPF "Build WPF loader frontend (Crymore.Loader)" ON)
option(BUILD_LOADER_NATIVE_UI "Build legacy D3D11 loader UI" OFF)

_loader_backend_sources(_BACKEND_SOURCES)

add_library(${CORE_TARGET} SHARED
    src/crymore_core.cpp
    ${_BACKEND_SOURCES}
)

set_target_properties(${CORE_TARGET} PROPERTIES
    OUTPUT_NAME "crymore_core"
)

target_compile_definitions(${CORE_TARGET} PRIVATE CRYMORE_CORE_EXPORTS)
_loader_common_target_props(${CORE_TARGET})

# ── Legacy native UI executable ──────────────────────────────────────────────

if(BUILD_LOADER_NATIVE_UI)
    set(LOADER_SOURCES
        src/main.cpp
        src/loader_ui.cpp
        ${_BACKEND_SOURCES}
        ${_SRC_ROOT}/overlay/renderer.cpp
        ${_SRC_ROOT}/gui/font.cpp
        ${_SRC_ROOT}/gui/gui.cpp
    )

    add_executable(${LOADER_TARGET} WIN32 ${LOADER_SOURCES})

    set_target_properties(${LOADER_TARGET} PROPERTIES
        OUTPUT_NAME "crymore_loader"
        WIN32_EXECUTABLE TRUE
    )

    target_include_directories(${LOADER_TARGET} PRIVATE
        ${imgui_SOURCE_DIR}
        ${imgui_SOURCE_DIR}/backends
    )

    target_link_libraries(${LOADER_TARGET} PRIVATE
        imgui_lib
        d3d11
        d3dcompiler
        dxgi
        dwmapi
        windowscodecs
        gdi32
        ole32
    )

    _loader_common_target_props(${LOADER_TARGET})
endif()

# ── WPF frontend ─────────────────────────────────────────────────────────────

if(BUILD_LOADER_WPF)
    set(_CSHARP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/csharp/Crymore.Loader")
    set(_CSHARP_OUT "$<TARGET_FILE_DIR:${CORE_TARGET}>")

    add_custom_target(crymore_loader_wpf ALL
        COMMAND dotnet build "${_CSHARP_DIR}/Crymore.Loader.csproj"
            -c $<IF:$<CONFIG:Debug>,Debug,Release>
            -o "${_CSHARP_OUT}"
            /p:CoreOutputDir="${_CSHARP_OUT}"
        DEPENDS ${CORE_TARGET}
        COMMENT "Building Crymore.Loader WPF frontend"
        VERBATIM
    )
endif()
