cmake_minimum_required(VERSION 3.20)
project(pixelated_mods C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Enforce MSVC 32-bit target. ReShade in PSOBB is a 32-bit loader, so the
# add-on must be an x86 DLL. `vcvarsall.bat x86` sets up the cross
# compiler; invoking CMake from that environment makes CMAKE_SIZEOF_VOID_P
# == 4 automatically.
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR
        "pixelated_mods must be built for 32-bit. Run build.bat (which "
        "sources vcvarsall.bat x86) or configure manually from an x86 "
        "Developer Command Prompt.")
endif()

if(NOT MSVC)
    message(FATAL_ERROR
        "pixelated_mods is MSVC-only. ReShade, PsoBB.exe, and MinHook all "
        "target MSVC; we follow suit to match host C++ ABI and compile the "
        "vendored ReShade headers cleanly without patching.")
endif()

# Leave CMake's default /EHsc in place. We do not actually throw any
# exceptions in this code, but the MSVC STL (std::string, std::vector,
# std::unordered_map, etc.) is built assuming exception handling is
# enabled — mixing it with /EHs-c- causes silent crashes during DLL
# load when stdlib statics construct. GCC's libstdc++ is more forgiving
# of -fno-exceptions, which is why the previous MinGW build got away
# with it. On MSVC, accept the unwind-table overhead (a few KB) and
# keep the STL happy.

# ---------------------------------------------------------------------------
# MinHook: x86 inline hooking, statically linked.
# ---------------------------------------------------------------------------
set(MINHOOK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/deps/minhook")
add_library(minhook STATIC
    ${MINHOOK_DIR}/src/hook.c
    ${MINHOOK_DIR}/src/buffer.c
    ${MINHOOK_DIR}/src/trampoline.c
    ${MINHOOK_DIR}/src/hde/hde32.c
    ${MINHOOK_DIR}/src/hde/hde64.c
)
target_include_directories(minhook PUBLIC "${MINHOOK_DIR}/include")
# Vendored third-party — silence all warnings on this target only.
target_compile_options(minhook PRIVATE /w)

# ---------------------------------------------------------------------------
# pixelated_mods: the add-on DLL itself.
#
# Source layout:
#   pixelated_mods.cpp  - main, HUD + stats + config + DllMain
#   controller_chords.* - LT/RT + face button chord input hooks
#   pso_log.*           - thread-safe append logger + crash handler
#   pso_sigscan.*       - PE walker + AOB pattern scanner
#   pso_addresses.*     - runtime-resolved PSOBB global addresses
# ---------------------------------------------------------------------------
add_library(pixelated_mods SHARED
    pixelated_mods.cpp
    controller_chords.cpp
    pso_log.cpp
    pso_sigscan.cpp
    pso_addresses.cpp
    pso_filters.cpp
)

target_include_directories(pixelated_mods PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/deps/reshade/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/deps/reshade/deps/imgui"
)

target_link_libraries(pixelated_mods PRIVATE minhook winmm)

target_compile_definitions(pixelated_mods PRIVATE
    RESHADE_ADDON=1
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    # Silence MSVC's CRT "use the _s variant" nag. We use the C-standard
    # names (fopen, sprintf, strcpy, etc.) because the _s variants are
    # non-portable Microsoft extensions.
    _CRT_SECURE_NO_WARNINGS
    # Do NOT define IMGUI_DISABLE_OBSOLETE_FUNCTIONS / _KEYIO: they change
    # the ImGuiIO struct layout, which must match ReShade's build.
)

target_compile_options(pixelated_mods PRIVATE
    /W4            # high warning level (not /Wall, which floods us with
                   # nuisance warnings from Windows SDK headers)
    /GR-           # disable RTTI (safe with STL; unlike disabling
                   # exceptions, removing RTTI doesn't break stdlib ABI)
    /permissive-   # strict standards mode
    /Zc:__cplusplus # make __cplusplus report the actual C++ standard
)

# Static CRT so the DLL is self-contained. Applied to both targets so the
# final link sees a single CRT flavor.
set_property(TARGET pixelated_mods PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
set_property(TARGET minhook PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")

# ReShade's 32-bit loader selects add-ons by the .addon32 extension.
set_target_properties(pixelated_mods PROPERTIES
    PREFIX ""
    OUTPUT_NAME "pixelated_mods"
    SUFFIX ".addon32"
)

# Optional debugger addon — local-only, not shipped to users and
# gitignored. Enable this block when reverse engineering new entity
# layouts, then disable again so the released build doesn't carry
# a second DLL users don't need.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/debugger/CMakeLists.txt" AND
   DEFINED ENV{PIXELATED_BUILD_DEBUGGER})
    add_subdirectory(debugger)
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/audio_debugger/CMakeLists.txt" AND
   DEFINED ENV{PIXELATED_BUILD_AUDIO_DEBUGGER})
    add_subdirectory(audio_debugger)
endif()
