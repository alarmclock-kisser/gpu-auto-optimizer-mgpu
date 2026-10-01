cmake_minimum_required(VERSION 3.28)
project(gpu_auto_optimizer CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Static CRT: gao.exe runs elevated, and the VC++ runtime DLLs
# (MSVCP140/VCRUNTIME140) are not KnownDLLs -- a copy planted next to the exe
# would load first. Static linking also drops the redistributable dependency.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# AddressSanitizer build (the `asan` preset), run on core_tests in CI. It
# applies to every target: MSVC refuses to link sanitized and unsanitized
# objects together (annotate_string/annotate_vector mismatch).
option(GAO_ASAN "Build everything with AddressSanitizer" OFF)
if(GAO_ASAN)
  add_compile_options(/fsanitize=address)
  add_link_options(/INCREMENTAL:NO)
endif()

# Exploit mitigations for code that runs elevated: Control Flow Guard (every
# indirect call, including the NVAPI function pointers in hw/), SDL checks.
# The linker half (/GUARD:CF /CETCOMPAT) is on the executable.
add_library(gao_hardening INTERFACE)
target_compile_options(gao_hardening INTERFACE /guard:cf /sdl)

# Warning set for our own code (cpp-best-practices MSVC list). Third-party
# headers are SYSTEM includes and stay quiet through /external:W0.
add_library(gao_warnings INTERFACE)
target_compile_options(gao_warnings INTERFACE
  /W4 /permissive- /external:W0 /external:anglebrackets
  /w14242 /w14254 /w14263 /w14265 /w14287 /we4289 /w14296 /w14311 /w14545 /w14546
  /w14547 /w14549 /w14555 /w14619 /w14640 /w14826 /w14905 /w14906 /w14928)

add_library(doctest INTERFACE)
target_include_directories(doctest SYSTEM INTERFACE third_party)
# Without this, doctest forward-declares std::string/ostream itself (its own
# comment calls this "not permitted by the C++ Standard") instead of including
# <string>/<ostream>, which breaks as soon as a test streams a real std type.
target_compile_definitions(doctest INTERFACE DOCTEST_CONFIG_USE_STD_HEADERS)

# Pure logic. No Windows, no driver, no D3D — this is what CI tests.
add_library(core STATIC
  src/core/version.hpp
  src/core/types.hpp
  src/core/objectives.hpp
  src/core/objectives.cpp
  src/core/fan_curve.hpp
  src/core/fan_curve.cpp
  src/core/config.hpp
  src/core/config.cpp
  src/core/stress_math.hpp
  src/core/stress_math.cpp
  src/core/stability.hpp
  src/core/stability.cpp
  src/core/journal.hpp
  src/core/journal.cpp
  src/core/search.hpp
  src/core/search.cpp
  src/core/boot.hpp
  src/core/boot.cpp
  src/core/manual.hpp
  src/core/manual.cpp
  src/core/task_xml.hpp
  src/core/task_xml.cpp
  src/core/watchdog.hpp
  src/core/watchdog.cpp
)
target_include_directories(core PUBLIC src)
target_include_directories(core SYSTEM PRIVATE third_party)
target_link_libraries(core PRIVATE gao_warnings gao_hardening)

# The only code allowed to touch hardware.
add_library(hw STATIC
  src/hw/nvml.hpp src/hw/nvml.cpp
  src/hw/nvapi.hpp src/hw/nvapi.cpp
  src/hw/gpu_control.hpp src/hw/gpu_control.cpp
  src/hw/stress.hpp src/hw/stress.cpp
  src/hw/app_files.hpp src/hw/app_files.cpp
  src/hw/boot_task.hpp src/hw/boot_task.cpp
)
target_include_directories(hw PUBLIC src)
target_link_libraries(hw PUBLIC core d3d11 dxgi d3dcompiler shell32 ole32 PRIVATE gao_warnings gao_hardening)

# UI-independent application logic shared by both executables.
add_library(app STATIC src/app/common.hpp src/app/common.cpp)
target_link_libraries(app PUBLIC core hw PRIVATE gao_warnings gao_hardening)

add_executable(gao src/app/main.cpp src/app/gao.manifest src/app/app.rc)
target_link_libraries(gao PRIVATE app gao_warnings gao_hardening)
# d3dcompiler_47.dll, d3d11.dll and dxgi.dll are not KnownDLLs: delay-load
# them so they resolve after SetDefaultDllDirectories(SYSTEM32) in main(),
# never from the exe's folder.
target_link_libraries(gao PRIVATE delayimp)
target_link_options(gao PRIVATE /DELAYLOAD:d3dcompiler_47.dll /DELAYLOAD:d3d11.dll /DELAYLOAD:dxgi.dll
                   /DEPENDENTLOADFLAG:0x800)
# Linker half of the mitigations: Control Flow Guard tables, CET shadow stack
# compatibility. (/DYNAMICBASE and /HIGHENTROPYVA are already x64 defaults.)
target_link_options(gao PRIVATE /GUARD:CF /CETCOMPAT)

# Dear ImGui (vendored, unmodified) as its own library, compiled with the
# compiler's defaults rather than our warning set.
add_library(imgui STATIC
  third_party/imgui/imgui.cpp third_party/imgui/imgui_draw.cpp
  third_party/imgui/imgui_tables.cpp third_party/imgui/imgui_widgets.cpp
  third_party/imgui/backends/imgui_impl_win32.cpp third_party/imgui/backends/imgui_impl_dx11.cpp
)
target_include_directories(imgui SYSTEM PUBLIC third_party/imgui)
target_link_libraries(imgui PUBLIC d3d11 dwmapi PRIVATE gao_hardening)

# The window and tray executable (Windows subsystem: no console).
add_executable(GpuAutoOptimizer WIN32
  src/app/gui/main_gui.cpp
  src/app/gui/ui.hpp src/app/gui/ui.cpp
  src/app/gui/worker.hpp src/app/gui/worker.cpp
  src/app/gao.manifest
  src/app/app.rc
)
target_link_libraries(GpuAutoOptimizer PRIVATE app imgui gao_warnings gao_hardening delayimp)
target_link_options(GpuAutoOptimizer PRIVATE /DELAYLOAD:d3dcompiler_47.dll /DELAYLOAD:d3d11.dll /DELAYLOAD:dxgi.dll
                   /DELAYLOAD:dwmapi.dll /DEPENDENTLOADFLAG:0x800 /GUARD:CF /CETCOMPAT)

enable_testing()
add_executable(core_tests
  tests/test_main.cpp
  tests/test_version.cpp
  tests/test_objectives.cpp
  tests/test_config.cpp
  tests/test_types.cpp
  tests/test_stress_math.cpp
  tests/test_stability.cpp
  tests/test_journal.cpp
  tests/test_search.cpp
  tests/test_boot.cpp
  tests/test_manual.cpp
  tests/test_task_xml.cpp
  tests/test_watchdog.cpp
  tests/test_fan_curve.cpp
)
target_link_libraries(core_tests PRIVATE core doctest gao_warnings)
add_test(NAME core_tests COMMAND core_tests)

set_target_properties(core hw app gao GpuAutoOptimizer core_tests PROPERTIES COMPILE_WARNING_AS_ERROR ON)
