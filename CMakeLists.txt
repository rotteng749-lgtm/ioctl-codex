# ioctl-codex — build file for the Zygisk module, the ImGui overlay and the dumper.
#
# Targets:
#   ioctl_codex_zygisk  (SHARED)  the Zygisk module, loaded by Zygisk Next/Magisk
#   ioctl_codex_menu    (SHARED)  the overlay (ImGui + hooks + capabilities)
#   ioctl_codex_dumper  (SHARED)  the IL2CPP dumper, loaded as a custom lib
#
# Supporting static libs: imgui, shadowhook_static.
#
# 16 KB page-size compliance (Android 15+ requirement, mandatory on 16 KB
# devices): every shared object is linked with max-page-size=16384. Verified in
# CI with `readelf -lW`.

cmake_minimum_required(VERSION 3.22)
project(ioctl-codex C CXX ASM)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_C_VISIBILITY_PRESET hidden)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# ---------------------------------------------------------------------------
# Global flags
# ---------------------------------------------------------------------------
add_compile_options(-fvisibility=hidden -fno-exceptions -fno-rtti -Wall -Wextra)

# LINK options (note: max-page-size is a LINKER flag, not a compile flag — the
# old stub wrongly used add_compile_options for this).
add_link_options(-Wl,-z,max-page-size=16384 -Wl,-z,separate-code)
add_link_options(-Wl,--gc-sections -Wl,--exclude-libs,ALL)

set(THIRD_PARTY ${CMAKE_CURRENT_SOURCE_DIR}/third_party)

# ---------------------------------------------------------------------------
# Dear ImGui (static). Docking branch, GLES3 backend, no demo.
# ---------------------------------------------------------------------------
set(IMGUI_DIR ${THIRD_PARTY}/imgui)
add_library(imgui STATIC
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
)
target_include_directories(imgui PUBLIC ${IMGUI_DIR} ${IMGUI_DIR}/backends)
target_compile_definitions(imgui PUBLIC
    IMGUI_IMPL_OPENGL_ES3=1
    IMGUI_DISABLE_OBSOLETE_FUNCTIONS=1
)
# ImGui is third-party; do not let its warnings fail our build.
target_compile_options(imgui PRIVATE -w)

# ---------------------------------------------------------------------------
# ShadowHook (static) — inline hooking engine (ByteDance).
# We build it as a STATIC lib and link it into the menu so there is only ONE
# .so to stage into the app data dir (simpler deployment, no RPATH juggling).
# ---------------------------------------------------------------------------
set(SH_DIR ${THIRD_PARTY}/shadowhook/shadowhook/src/main/cpp)
if(ANDROID_ABI STREQUAL "arm64-v8a")
    set(SH_ARCH "arm64")
elseif(ANDROID_ABI STREQUAL "armeabi-v7a")
    set(SH_ARCH "arm")
else()
    message(FATAL_ERROR "ioctl-codex supports arm64-v8a and armeabi-v7a (got ${ANDROID_ABI})")
endif()
file(GLOB SH_SRC
    ${SH_DIR}/*.c
    ${SH_DIR}/arch/${SH_ARCH}/*.c
    ${SH_DIR}/arch/${SH_ARCH}/*.S
    ${SH_DIR}/common/*.c
    ${SH_DIR}/third_party/*/*.c
)
add_library(shadowhook_static STATIC ${SH_SRC})
target_compile_features(shadowhook_static PRIVATE c_std_11)
target_include_directories(shadowhook_static PRIVATE
    ${SH_DIR}
    ${SH_DIR}/include
    ${SH_DIR}/arch/${SH_ARCH}
    ${SH_DIR}/common
    ${SH_DIR}/third_party/xdl
    ${SH_DIR}/third_party/bsd
    ${SH_DIR}/third_party/lss
)
# Relax the strict upstream flags (they use -Weverything -Werror, which is only
# meant for their own CI).
target_compile_options(shadowhook_static PRIVATE -w)
target_link_libraries(shadowhook_static PUBLIC log)

# ---------------------------------------------------------------------------
# Zygisk module
# ---------------------------------------------------------------------------
add_library(ioctl_codex_zygisk SHARED
    src/zygisk/module.cpp
)
target_include_directories(ioctl_codex_zygisk PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/common
    ${THIRD_PARTY}
)
target_compile_options(ioctl_codex_zygisk PRIVATE -fno-rtti)
target_link_libraries(ioctl_codex_zygisk PRIVATE log dl)
# Keep only zygisk_module_entry / zygisk_companion_entry exported.
target_link_options(ioctl_codex_zygisk PRIVATE
    -Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/zygisk.map.txt)

# ---------------------------------------------------------------------------
# Overlay / menu
# ---------------------------------------------------------------------------
add_library(ioctl_codex_menu SHARED
    src/menu/menu.cpp
    src/menu/overlay.cpp
    src/menu/egl_hook.cpp
    src/menu/input.cpp
    src/menu/globals.cpp
    src/menu/storage_bridge.cpp
    src/customlib/loader.cpp
    src/menu/cap/cap_java_dump.cpp
    src/menu/cap/cap_native_dump.cpp
    src/menu/cap/cap_string_dump.cpp
    src/menu/cap/cap_mem_scan.cpp
    src/menu/cap/cap_ssl_bypass.cpp
    src/menu/cap/cap_tls_capture.cpp
    src/menu/cap/cap_crypto_intercept.cpp
    src/menu/cap/cap_storage_dump.cpp
    src/menu/cap/cap_detection_bypass.cpp
    src/menu/cap/cap_frida_runner.cpp
    src/menu/cap/cap_il2cpp_dump.cpp
)
target_include_directories(ioctl_codex_menu PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/common
    ${CMAKE_CURRENT_SOURCE_DIR}/src/menu
    ${CMAKE_CURRENT_SOURCE_DIR}/src/menu/cap
    ${SH_DIR}/include
)
target_compile_options(ioctl_codex_menu PRIVATE -fno-rtti -fexceptions)
target_link_libraries(ioctl_codex_menu PRIVATE
    imgui
    shadowhook_static
    EGL
    GLESv3
    android
    log
    dl
)
# Export the C ABI entry points even though visibility is hidden.
target_link_options(ioctl_codex_menu PRIVATE
    -Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/menu.map.txt)

# ---------------------------------------------------------------------------
# Dumper (custom lib)
# ---------------------------------------------------------------------------
add_library(ioctl_codex_dumper SHARED
    src/dumper/dumper.cpp
)
target_include_directories(ioctl_codex_dumper PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/common
)
target_compile_options(ioctl_codex_dumper PRIVATE -fno-rtti -fexceptions)
target_link_libraries(ioctl_codex_dumper PRIVATE log dl)
target_link_options(ioctl_codex_dumper PRIVATE
    -Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/dumper.map.txt)

# ---------------------------------------------------------------------------
# Convenience: stage everything the module needs into build/libs/.
# ---------------------------------------------------------------------------
add_custom_target(stage ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/libs
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:ioctl_codex_zygisk> ${CMAKE_BINARY_DIR}/libs/
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:ioctl_codex_menu>   ${CMAKE_BINARY_DIR}/libs/
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:ioctl_codex_dumper> ${CMAKE_BINARY_DIR}/libs/
    DEPENDS ioctl_codex_zygisk ioctl_codex_menu ioctl_codex_dumper
    COMMENT "Staging built .so files into build/libs/"
)
