cmake_minimum_required(VERSION 3.20)
project(steam_friends LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(IMGUI third_party/imgui)

add_executable(steam_friends
  src/main.cpp
  ${IMGUI}/imgui.cpp
  ${IMGUI}/imgui_draw.cpp
  ${IMGUI}/imgui_tables.cpp
  ${IMGUI}/imgui_widgets.cpp
  ${IMGUI}/backends/imgui_impl_win32.cpp
  ${IMGUI}/backends/imgui_impl_dx11.cpp
)

target_include_directories(steam_friends PRIVATE ${IMGUI} ${IMGUI}/backends)
target_compile_definitions(steam_friends PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(steam_friends PRIVATE d3d11 dxgi winhttp crypt32)

if(MSVC)
  target_link_options(steam_friends PRIVATE "/SUBSYSTEM:WINDOWS" "/ENTRY:mainCRTStartup")
endif()

add_custom_command(TARGET steam_friends POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          $<TARGET_FILE:steam_friends>
          ${CMAKE_SOURCE_DIR}/steam_friends.exe
)
