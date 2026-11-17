cmake_minimum_required(VERSION 3.20)

# Universal (Apple Silicon + Intel) by default; override with -DCMAKE_OSX_ARCHITECTURES=arm64
if(APPLE AND NOT CMAKE_OSX_ARCHITECTURES)
  set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "Target architectures")
endif()
if(APPLE AND NOT CMAKE_OSX_DEPLOYMENT_TARGET)
  set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version")
endif()

project(senseless VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(SENSELESS_BUILD_TOOLS    "Build senseless and senseless-mpe" ON)
option(SENSELESS_BUILD_EXAMPLES "Build Sensel's original C examples" ON)
option(SENSELESS_BUILD_TESTS    "Build tests" ON)

include(GNUInstallDirs)

# ------------------------------------------------------------------ LibSensel

set(SENSEL_SRC lib/sensel/src)
set(LIBSENSEL_SOURCES
  ${SENSEL_SRC}/sensel.c
  ${SENSEL_SRC}/sensel_register.c
  ${SENSEL_SRC}/sensel_decompress.c
  ${SENSEL_SRC}/senseless_ext.c)
if(APPLE)
  list(APPEND LIBSENSEL_SOURCES ${SENSEL_SRC}/sensel_serial_mac.c)
else()
  list(APPEND LIBSENSEL_SOURCES ${SENSEL_SRC}/sensel_serial_linux.c)
endif()

set(LIBSENSEL_PUBLIC_HEADERS
  ${SENSEL_SRC}/sensel.h
  ${SENSEL_SRC}/sensel_types.h
  ${SENSEL_SRC}/sensel_device.h
  ${SENSEL_SRC}/sensel_protocol.h
  ${SENSEL_SRC}/sensel_decompress.h
  ${SENSEL_SRC}/senseless_ext.h)

add_library(sensel SHARED ${LIBSENSEL_SOURCES})
target_include_directories(sensel PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/${SENSEL_SRC}>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
# SENSEL_PRESSURE: force frames are always available now that decompression is open.
target_compile_definitions(sensel PRIVATE SENSEL_EXPORTS SENSEL_PRESSURE
  SENSELESS_VERSION="${PROJECT_VERSION}")
target_compile_options(sensel PRIVATE -Wall -fvisibility=hidden)
# Bit-exact float results across architectures (validated against Sensel's binary).
set_source_files_properties(${SENSEL_SRC}/sensel_decompress.c PROPERTIES COMPILE_OPTIONS "-ffp-contract=off")
set_target_properties(sensel PROPERTIES
  OUTPUT_NAME Sensel                      # libSensel.dylib, same name as Sensel's installer
  PUBLIC_HEADER "${LIBSENSEL_PUBLIC_HEADERS}")
if(APPLE)
  target_link_libraries(sensel PRIVATE "-framework CoreFoundation" "-framework IOKit")
  # Drop-in for binaries linked against Sensel's /usr/local/lib/libSensel.dylib
  # (compatibility_version 1.0.0).
  set_target_properties(sensel PROPERTIES
    INSTALL_NAME_DIR "${CMAKE_INSTALL_FULL_LIBDIR}"
    MACHO_COMPATIBILITY_VERSION 1.0.0
    MACHO_CURRENT_VERSION ${PROJECT_VERSION})
endif()

# Compatibility shim: Sensel's installer shipped decompression as a second library that
# programs (and the Python wrapper on Windows) linked or loaded explicitly. Re-export
# the symbols from libSensel so those programs keep working.
if(APPLE)
  file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/sensel_decompress_shim.c
       "/* re-exports senselDecompress* from libSensel */\n")
  add_library(sensel_decompress_compat SHARED ${CMAKE_CURRENT_BINARY_DIR}/sensel_decompress_shim.c)
  target_link_options(sensel_decompress_compat PRIVATE "LINKER:-reexport_library,$<TARGET_FILE:sensel>")
  add_dependencies(sensel_decompress_compat sensel)
  set_target_properties(sensel_decompress_compat PROPERTIES
    OUTPUT_NAME SenselDecompress
    INSTALL_NAME_DIR "${CMAKE_INSTALL_FULL_LIBDIR}"
    MACHO_COMPATIBILITY_VERSION 1.0.0
    MACHO_CURRENT_VERSION ${PROJECT_VERSION})
endif()

install(TARGETS sensel
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  PUBLIC_HEADER DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
if(TARGET sensel_decompress_compat)
  install(TARGETS sensel_decompress_compat LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
endif()

# ------------------------------------------------------------------ tools

if(SENSELESS_BUILD_TOOLS)
  add_library(senselmap STATIC tools/senselmap.c)
  target_include_directories(senselmap PUBLIC tools)
  target_compile_options(senselmap PRIVATE -Wall -Wextra)

  add_executable(senseless tools/senseless.c)
  target_link_libraries(senseless PRIVATE sensel senselmap)
  target_compile_options(senseless PRIVATE -Wall -Wextra)

  if(APPLE)
    add_executable(senseless-mpe tools/senseless-mpe.c)
    target_link_libraries(senseless-mpe PRIVATE sensel "-framework CoreMIDI" "-framework CoreFoundation")
    target_compile_options(senseless-mpe PRIVATE -Wall -Wextra)
    install(TARGETS senseless-mpe RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
  endif()
  install(TARGETS senseless RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

# ------------------------------------------------------------------ examples

if(SENSELESS_BUILD_EXAMPLES)
  foreach(ex example-1-hello-sensel example-2-contacts example-3-forces example-4-multi)
    add_executable(${ex} examples/${ex}/main.c)
    target_link_libraries(${ex} PRIVATE sensel pthread)
    set_target_properties(${ex} PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/examples)
  endforeach()
endif()

# ------------------------------------------------------------------ tests

if(SENSELESS_BUILD_TESTS)
  enable_testing()
  add_executable(decompress_selfcheck tests/decompress_diff.c)
  target_include_directories(decompress_selfcheck PRIVATE ${SENSEL_SRC})
  add_test(NAME decompress_selfcheck
           COMMAND decompress_selfcheck $<TARGET_FILE:sensel> - 500)

  if(SENSELESS_BUILD_TOOLS)
    file(GLOB TEST_MAPS ${CMAKE_CURRENT_SOURCE_DIR}/tests/maps/*.senselmap)
    if(TEST_MAPS)
      add_test(NAME senselmap_compile COMMAND senseless map-check ${TEST_MAPS})
    endif()
  endif()
endif()
