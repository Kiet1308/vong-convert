cmake_minimum_required(VERSION 3.20)
project(VongConvert LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_executable(VongConvert WIN32
  src/main.cpp
  src/util.cpp
  src/gfx.cpp
  src/svgpath.cpp
  src/catalog.cpp
  src/jobs.cpp
  src/actions.cpp
  src/wheel.cpp
  src/bubble.cpp
  src/settings.cpp
  src/crop.cpp
  src/pin.cpp
  src/perf.cpp
  res/app.rc
)

target_compile_definitions(VongConvert PRIVATE
  UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN
  _WIN32_WINNT=0x0A00 WINVER=0x0A00 NTDDI_VERSION=0x0A000006)

if(MINGW)
  target_compile_options(VongConvert PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)
  target_link_options(VongConvert PRIVATE -static -static-libgcc -static-libstdc++ -municode
    $<$<CONFIG:Release>:-s>)
endif()

target_link_libraries(VongConvert PRIVATE
  d2d1 d3d11 dxgi dcomp dwrite windowscodecs
  ole32 oleaut32 uuid shell32 shlwapi user32 gdi32 shcore dwmapi comctl32 advapi32 uiautomationcore)
