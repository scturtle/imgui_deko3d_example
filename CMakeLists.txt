cmake_minimum_required(VERSION 3.18.4)
# or use: /opt/devkitpro/portlibs/switch/bin/aarch64-none-elf-cmake
include(/opt/devkitpro/cmake/Switch.cmake)
project(imgui_deko3d_example VERSION 0.0.1 LANGUAGES C CXX)
set(PROJECT_AUTHOR "scturtle")

# FIXME: problem with -DNDEBUG
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "" FORCE)
endif()

set(TARGET ${PROJECT_NAME})
set(IMGUI_DIR third_parties/imgui)

add_executable(${TARGET}
  src/main.cc
  src/imgui_impl_deko3d.cpp
  ${IMGUI_DIR}/imgui.cpp
  ${IMGUI_DIR}/imgui_demo.cpp
  ${IMGUI_DIR}/imgui_draw.cpp
  ${IMGUI_DIR}/imgui_tables.cpp
  ${IMGUI_DIR}/imgui_widgets.cpp
  )
target_compile_features(${TARGET} PUBLIC cxx_std_17)
target_compile_options(${TARGET} PUBLIC -O2)

target_link_libraries(${TARGET} PUBLIC
  $<IF:$<CONFIG:Debug>,deko3dd,deko3d>
  )
target_include_directories(${TARGET} PUBLIC
  ${DEVKITPRO}/portlibs/switch/include
  ${IMGUI_DIR})

nx_add_shader_program(imgui_vsh src/imgui_vsh.glsl vert)
nx_add_shader_program(imgui_fsh src/imgui_fsh.glsl frag)
dkp_add_asset_target(${TARGET}_romfs ${CMAKE_CURRENT_BINARY_DIR}/romfs)
dkp_install_assets(${TARGET}_romfs
  DESTINATION shaders
  TARGETS imgui_vsh imgui_fsh)

nx_generate_nacp(${TARGET}.nacp
  NAME ${TARGET}
  AUTHOR ${PROJECT_AUTHOR}
  )
nx_create_nro(${TARGET}
  NACP ${TARGET}.nacp
  ROMFS ${TARGET}_romfs
  )
