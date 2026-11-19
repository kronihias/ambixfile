cmake_minimum_required(VERSION 3.15)
project(ambixfile VERSION 0.3.0 LANGUAGES CXX)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  set(_ambixfile_top_level ON)
else()
  set(_ambixfile_top_level OFF)
endif()

option(AMBIXFILE_BUILD_TESTS "Build the ambixfile tests" ${_ambixfile_top_level})
option(AMBIXFILE_BUILD_TOOLS "Build the ambixfile-info command-line tool" ${_ambixfile_top_level})
option(AMBIXFILE_BUILD_FUZZERS "Build the libFuzzer targets (needs Clang)" OFF)
option(AMBIXFILE_INSTALL "Add install rules" ${_ambixfile_top_level})
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

if(AMBIXFILE_BUILD_TOOLS)
  add_executable(ambixfile-info tools/ambixfile-info.cpp)
  target_link_libraries(ambixfile-info PRIVATE ambixfile)
  target_compile_options(ambixfile-info PRIVATE ${_ambixfile_warnings})
endif()

if(AMBIXFILE_BUILD_FUZZERS)
  # The library is built with the sanitizers too, so set them for everything:
  #   -DCMAKE_CXX_FLAGS="-fsanitize=fuzzer-no-link,address,undefined"
  add_executable(fuzz_reader fuzz/fuzz_reader.cpp)
  target_link_libraries(fuzz_reader PRIVATE ambixfile)
  target_link_options(fuzz_reader PRIVATE -fsanitize=fuzzer)
  add_executable(make_seeds fuzz/make_seeds.cpp) # starting corpus: make_seeds <dir>
  target_link_libraries(make_seeds PRIVATE ambixfile)
endif()

if(AMBIXFILE_INSTALL)
  include(GNUInstallDirs)
  include(CMakePackageConfigHelpers)
  install(TARGETS ambixfile EXPORT ambixfileTargets
          ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
          LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
          RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
  install(DIRECTORY include/ambixfile DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
  if(AMBIXFILE_BUILD_TOOLS)
    install(TARGETS ambixfile-info RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
  endif()
  # A static ambixfile with WavPack needs its consumers to find WavPack as
  # well; that part is not exported, so the package files are only installed
  # for builds a consumer can link on their own.
  get_target_property(_ambixfile_type ambixfile TYPE)
  if(NOT AMBIXFILE_ENABLE_WAVPACK OR NOT _ambixfile_type STREQUAL "STATIC_LIBRARY")
    install(EXPORT ambixfileTargets NAMESPACE ambixfile:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ambixfile)
    file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/ambixfileConfig.cmake
         "include(\"\${CMAKE_CURRENT_LIST_DIR}/ambixfileTargets.cmake\")\n")
    write_basic_package_version_file(${CMAKE_CURRENT_BINARY_DIR}/ambixfileConfigVersion.cmake
                                     VERSION ${PROJECT_VERSION} COMPATIBILITY SameMinorVersion)
    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/ambixfileConfig.cmake
                  ${CMAKE_CURRENT_BINARY_DIR}/ambixfileConfigVersion.cmake
            DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ambixfile)
  endif()
endif()

if(AMBIXFILE_BUILD_TESTS)
  enable_testing()
  enable_language(C)

  foreach(name test_matrix test_roundtrip test_metadata test_wavpack test_reduced_sets)
    add_executable(${name} tests/${name}.cpp)
    target_link_libraries(${name} PRIVATE ambixfile)
    target_compile_options(${name} PRIVATE ${_ambixfile_warnings})
    add_test(NAME ${name} COMMAND ${name} ${CMAKE_CURRENT_BINARY_DIR})
  endforeach()

  # Inputs the fuzzer once crashed on, run through the fuzz target.
  add_executable(test_fuzz_regressions tests/test_fuzz_regressions.cpp fuzz/fuzz_reader.cpp)
  target_link_libraries(test_fuzz_regressions PRIVATE ambixfile)
  add_test(NAME test_fuzz_regressions
           COMMAND test_fuzz_regressions ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz_regressions
                   wavpack_failed_seek.bin caf_data_size_overflow.bin)

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
      DEPENDS scripts/amalgamate.py include/ambixfile/ambixfile.hpp src/internal.hpp src/hemisphere_tables.hpp
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
