cmake_minimum_required(VERSION 3.16)
project(dsc_gametest C CXX)
set(CMAKE_CXX_STANDARD 20)

set(REPO ${CMAKE_CURRENT_SOURCE_DIR}/../..)
set(MELONDS_DIR ${REPO}/tools/melonDS CACHE PATH "melonDS source")
set(RCHEEVOS ${REPO}/external/rcheevos)

set(BUILD_QT_SDL OFF CACHE BOOL "" FORCE)
set(ENABLE_JIT OFF CACHE BOOL "" FORCE)
set(ENABLE_OGLRENDERER OFF CACHE BOOL "" FORCE)
set(ENABLE_GDBSTUB ON CACHE BOOL "" FORCE)
add_subdirectory(${MELONDS_DIR} melonds EXCLUDE_FROM_ALL)

file(GLOB RC_SRC
  ${RCHEEVOS}/src/client/rc_client.c
  ${RCHEEVOS}/src/runtime/*.c
  ${RCHEEVOS}/src/api/rc_api_common.c ${RCHEEVOS}/src/api/rc_api_info.c
  ${RCHEEVOS}/src/api/rc_api_runtime.c ${RCHEEVOS}/src/api/rc_api_user.c
  ${RCHEEVOS}/src/util/*.c)
add_library(dscheevos STATIC ${REPO}/dscheevos/core/src/dscheevos.c ${RC_SRC})
target_include_directories(dscheevos PUBLIC ${REPO}/dscheevos/core/include ${RCHEEVOS}/include PRIVATE ${RCHEEVOS}/src)
target_compile_definitions(dscheevos PRIVATE RC_NO_THREADS DSC_CHECK_FAST_STEPS)

add_executable(dsc-gametest gametest.cpp ${MELONDS_DIR}/tools/dscheevos/headless/platform.cpp)
target_link_libraries(dsc-gametest PRIVATE core dscheevos pthread m)
# melonDS sets these per directory; this target must see the same class layouts
target_compile_definitions(dsc-gametest PRIVATE GDBSTUB_ENABLED ARCHITECTURE_x86_64=1)
