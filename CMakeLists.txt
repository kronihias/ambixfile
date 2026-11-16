cmake_minimum_required(VERSION 3.15)
project(ambixfile VERSION 0.1.0 LANGUAGES CXX)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  set(_ambixfile_top_level ON)
else()
  set(_ambixfile_top_level OFF)
endif()

option(AMBIXFILE_BUILD_TESTS "Build the ambixfile tests" ${_ambixfile_top_level})
# Optional: path to a libambix source tree (the directory holding its
# CMakeLists.txt). Used only by the interoperability test, never by the library.
set(AMBIXFILE_LIBAMBIX_DIR "" CACHE PATH "libambix source tree for the interop test")

add_library(ambixfile
  src/stream.cpp
  src/matrix.cpp
  src/reader.cpp
  src/writer.cpp
)
add_library(ambixfile::ambixfile ALIAS ambixfile)
target_compile_features(ambixfile PUBLIC cxx_std_17)
target_include_directories(ambixfile PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)
set_target_properties(ambixfile PROPERTIES POSITION_INDEPENDENT_CODE ON CXX_EXTENSIONS OFF)

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

  foreach(name test_matrix test_roundtrip)
    add_executable(${name} tests/${name}.cpp)
    target_link_libraries(${name} PRIVATE ambixfile)
    target_compile_options(${name} PRIVATE ${_ambixfile_warnings})
    add_test(NAME ${name} COMMAND ${name})
  endforeach()

  # Single-header build: generate it, then compile it in two translation units.
  find_package(Python3 COMPONENTS Interpreter QUIET)
  if(Python3_FOUND)
    set(_single ${CMAKE_CURRENT_BINARY_DIR}/single_include/ambixfile.hpp)
    add_custom_command(
      OUTPUT ${_single}
      COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/scripts/amalgamate.py ${_single}
      DEPENDS scripts/amalgamate.py include/ambixfile/ambixfile.hpp src/internal.hpp
              src/stream.cpp src/matrix.cpp src/reader.cpp src/writer.cpp
      COMMENT "Generating single-header ambixfile.hpp")
    add_executable(test_single_header
      tests/test_single_header.cpp tests/single_header_second_tu.cpp ${_single})
    target_include_directories(test_single_header PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/single_include)
    target_compile_features(test_single_header PRIVATE cxx_std_17)
    target_compile_options(test_single_header PRIVATE ${_ambixfile_warnings})
    add_test(NAME test_single_header COMMAND test_single_header)
  endif()

  if(AMBIXFILE_LIBAMBIX_DIR)
    enable_language(C)
    set(LIBAMBIX_ENABLE_WAVPACK OFF CACHE BOOL "" FORCE)
    add_subdirectory(${AMBIXFILE_LIBAMBIX_DIR} ${CMAKE_CURRENT_BINARY_DIR}/libambix EXCLUDE_FROM_ALL)
    add_executable(test_interop_libambix tests/test_interop_libambix.cpp)
    target_link_libraries(test_interop_libambix PRIVATE ambixfile ambix)
    add_test(NAME test_interop_libambix COMMAND test_interop_libambix ${CMAKE_CURRENT_BINARY_DIR})
  endif()
endif()
