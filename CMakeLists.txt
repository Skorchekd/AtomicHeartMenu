cmake_minimum_required(VERSION 3.20)
project(AtomicHeartMenu LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Static CRT: the injected DLL then needs no VC++ redistributable on the target.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# Use /EHa (async exceptions) so catch(...) also traps SEH access violations.
# This is our last-line firewall against wrong game offsets crashing the title.
string(REPLACE "/EHsc" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")

set(IMGUI   "${CMAKE_SOURCE_DIR}/deps/imgui")
set(MINHOOK "${CMAKE_SOURCE_DIR}/deps/minhook")

add_library(AtomicHeartMenu SHARED
    # --- our code ---
    src/dllmain.cpp
    src/core/exception_guard.cpp
    src/core/log.cpp
    src/core/memory.cpp
    src/sdk/scanner.cpp
    src/sdk/ue4.cpp
    src/sdk/reflect.cpp
    src/hooks/wndproc_hook.cpp
    src/hooks/dx12_hook.cpp
    src/menu/menu.cpp
    src/features/features.cpp
    # --- Dear ImGui ---
    ${IMGUI}/imgui.cpp
    ${IMGUI}/imgui_draw.cpp
    ${IMGUI}/imgui_tables.cpp
    ${IMGUI}/imgui_widgets.cpp
    ${IMGUI}/backends/imgui_impl_win32.cpp
    ${IMGUI}/backends/imgui_impl_dx12.cpp
    # --- MinHook ---
    ${MINHOOK}/src/buffer.c
    ${MINHOOK}/src/hook.c
    ${MINHOOK}/src/trampoline.c
    ${MINHOOK}/src/hde/hde32.c
    ${MINHOOK}/src/hde/hde64.c
)

target_include_directories(AtomicHeartMenu PRIVATE
    ${IMGUI}
    ${IMGUI}/backends
    ${MINHOOK}/include
    ${CMAKE_SOURCE_DIR}/src
)

target_compile_definitions(AtomicHeartMenu PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _CRT_SECURE_NO_WARNINGS
    UNICODE
    _UNICODE
)

target_compile_options(AtomicHeartMenu PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/EHa>)

target_link_libraries(AtomicHeartMenu PRIVATE d3d12 dxgi dwmapi Psapi)

# Final artifact: AtomicHeartMenu.dll. Pin every config to bin\ so the multi-config
# VS generator doesn't bury it in bin\Release\.
set_target_properties(AtomicHeartMenu PROPERTIES
    OUTPUT_NAME "AtomicHeartMenu"
    RUNTIME_OUTPUT_DIRECTORY         "${CMAKE_SOURCE_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_SOURCE_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG   "${CMAKE_SOURCE_DIR}/bin"
)

# Companion LoadLibrary injector -> bin\injector.exe
add_executable(injector tools/injector.cpp)
set_target_properties(injector PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY         "${CMAKE_SOURCE_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_SOURCE_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG   "${CMAKE_SOURCE_DIR}/bin"
)
