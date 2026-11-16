cmake_minimum_required(VERSION 3.15)
project(ambixfile VERSION 0.2.0 LANGUAGES CXX)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  set(_ambixfile_top_level ON)
else()
  set(_ambixfile_top_level OFF)
endif()

option(AMBIXFILE_BUILD_TESTS "Build the ambixfile tests" ${_ambixfile_top_level})
option(AMBIXFILE_ENABLE_WAVPACK "Support WavPack-compressed ambiX files (needs the WavPack library)" OFF)
# Optional: a WavPack source tree to build and link statically. Without it an
# existing 'wavpack' target, a WavPack CMake package or pkg-config is used.
set(AMBIXFILE_WAVPACK_DIR "" CACHE PATH "WavPack source tree to build (optional)")
# Optional: path to a libambix source tree (the directory holding its
# CMakeLists.txt). Used only by the interoperability test, never by the library.
set(AMBIXFILE_LIBAMBIX_DIR "" CACHE PATH "libambix source tree for the interop test")

add_library(ambixfile
  src/stream.cpp
  src/matrix.cpp
  src/caf.cpp
  src/wavpack.cpp
  src/reader.cpp
  src/writer.cpp
  src/c_api.cpp
)
add_library(ambixfile::ambixfile ALIAS ambixfile)
target_compile_features(ambixfile PUBLIC cxx_std_17)
target_include_directories(ambixfile PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)
set_target_properties(ambixfile PROPERTIES POSITION_INDEPENDENT_CODE ON CXX_EXTENSIONS OFF)

if(AMBIXFILE_ENABLE_WAVPACK)
  if(NOT TARGET wavpack AND AMBIXFILE_WAVPACK_DIR)
    set(WAVPACK_BUILD_PROGRAMS           OFF CACHE BOOL "" FORCE)
    set(WAVPACK_BUILD_DOCS               OFF CACHE BOOL "" FORCE)
    set(WAVPACK_ENABLE_LEGACY            OFF CACHE BOOL "" FORCE)
    set(WAVPACK_ENABLE_DSD               OFF CACHE BOOL "" FORCE)
    set(WAVPACK_ENABLE_LIBICONV          OFF CACHE BOOL "" FORCE)
    set(WAVPACK_ENABLE_ASM               OFF CACHE BOOL "" FORCE)
    set(WAVPACK_INSTALL_CMAKE_MODULE     OFF CACHE BOOL "" FORCE)
    set(WAVPACK_INSTALL_DOCS             OFF CACHE BOOL "" FORCE)
    set(WAVPACK_INSTALL_PKGCONFIG_MODULE OFF CACHE BOOL "" FORCE)
    set(_ambixfile_saved_shared ${BUILD_SHARED_LIBS})
    set(_ambixfile_saved_testing ${BUILD_TESTING})
    set(BUILD_SHARED_LIBS OFF)
    set(BUILD_TESTING OFF)
    set(CMAKE_POLICY_DEFAULT_CMP0077 NEW) # let the two settings above apply
    enable_language(C)
    add_subdirectory(${AMBIXFILE_WAVPACK_DIR} ${CMAKE_CURRENT_BINARY_DIR}/wavpack EXCLUDE_FROM_ALL)
    set(BUILD_SHARED_LIBS ${_ambixfile_saved_shared})
    set(BUILD_TESTING ${_ambixfile_saved_testing})
  endif()
  if(TARGET wavpack)
    target_link_libraries(ambixfile PRIVATE wavpack)
  else()
    find_package(WavPack CONFIG QUIET)
    if(TARGET WavPack::WavPack)
      target_link_libraries(ambixfile PRIVATE WavPack::WavPack)
    else()
      find_package(PkgConfig REQUIRED)
      pkg_check_modules(AMBIXFILE_WAVPACK REQUIRED IMPORTED_TARGET wavpack)
      target_link_libraries(ambixfile PRIVATE PkgConfig::AMBIXFILE_WAVPACK)
    endif()
  endif()
  target_compile_definitions(ambixfile PRIVATE AMBIXFILE_ENABLE_WAVPACK)
endif()

if(_ambixfile_top_level)
  if(MSVC)
    set(_ambixfile_warnings /W4)
  else()
    set(_ambixfile_warnings -Wall -Wextra -Wpedantic -Wshadow)
  endif()
  target_compile_options(ambixfile PRIVATE ${_ambixfile_warnings})
endif()

if(AMBIXFILE_BUILD_TESTS)
  enable_testing()
  enable_language(C)

  foreach(name test_matrix test_roundtrip test_metadata test_wavpack)
    add_executable(${name} tests/${name}.cpp)
    target_link_libraries(${name} PRIVATE ambixfile)
    target_compile_options(${name} PRIVATE ${_ambixfile_warnings})
    add_test(NAME ${name} COMMAND ${name} ${CMAKE_CURRENT_BINARY_DIR})
  endforeach()

  # The C API, used from a plain C program.
  add_executable(test_c_api tests/test_c_api.c)
  target_link_libraries(test_c_api PRIVATE ambixfile)
  set_target_properties(test_c_api PROPERTIES C_STANDARD 99 C_EXTENSIONS OFF LINKER_LANGUAGE CXX)
  target_compile_options(test_c_api PRIVATE ${_ambixfile_warnings})
  add_test(NAME test_c_api COMMAND test_c_api ${CMAKE_CURRENT_BINARY_DIR})

  # Single-header build: generate it, then compile it in two translation units.
  find_package(Python3 COMPONENTS Interpreter QUIET)
  if(Python3_FOUND)
    set(_single ${CMAKE_CURRENT_BINARY_DIR}/single_include/ambixfile.hpp)
    add_custom_command(
      OUTPUT ${_single}
      COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/scripts/amalgamate.py ${_single}
      DEPENDS scripts/amalgamate.py include/ambixfile/ambixfile.hpp src/internal.hpp
              src/stream.cpp src/matrix.cpp src/caf.cpp src/wavpack.cpp src/reader.cpp src/writer.cpp
      COMMENT "Generating single-header ambixfile.hpp")
    add_executable(test_single_header
      tests/test_single_header.cpp tests/single_header_second_tu.cpp ${_single})
    target_include_directories(test_single_header PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/single_include)
    target_compile_features(test_single_header PRIVATE cxx_std_17)
    target_compile_options(test_single_header PRIVATE ${_ambixfile_warnings})
    if(AMBIXFILE_ENABLE_WAVPACK)
      # The implementation in the single header then needs WavPack itself.
      get_target_property(_ambixfile_wavpack ambixfile LINK_LIBRARIES)
      target_link_libraries(test_single_header PRIVATE ${_ambixfile_wavpack})
      target_compile_definitions(test_single_header PRIVATE AMBIXFILE_ENABLE_WAVPACK)
    endif()
    add_test(NAME test_single_header COMMAND test_single_header)
  endif()

  if(AMBIXFILE_LIBAMBIX_DIR)
    if(AMBIXFILE_ENABLE_WAVPACK AND TARGET wavpack)
      set(LIBAMBIX_ENABLE_WAVPACK ON CACHE BOOL "" FORCE)
    else()
      set(LIBAMBIX_ENABLE_WAVPACK OFF CACHE BOOL "" FORCE)
    endif()
    add_subdirectory(${AMBIXFILE_LIBAMBIX_DIR} ${CMAKE_CURRENT_BINARY_DIR}/libambix EXCLUDE_FROM_ALL)
    add_executable(test_interop_libambix tests/test_interop_libambix.cpp)
    target_link_libraries(test_interop_libambix PRIVATE ambixfile ambix)
    if(LIBAMBIX_ENABLE_WAVPACK)
      target_compile_definitions(test_interop_libambix PRIVATE INTEROP_WAVPACK)
    endif()
    add_test(NAME test_interop_libambix COMMAND test_interop_libambix ${CMAKE_CURRENT_BINARY_DIR})
  endif()
endif()
