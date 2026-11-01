cmake_minimum_required(VERSION 3.16)

project(
  tig
  VERSION 0.1.0
  DESCRIPTION "Portable C library built on libgit2"
  LANGUAGES C
)

option(TIG_BUILD_EXAMPLES "Build example programs" ON)
option(TIG_BUILD_TESTS "Build tests" ON)
option(TIG_STANDALONE "Hide and embed libgit2 so consumers only link libtig" ON)
option(TIG_REQUIRE_STATIC_LIBGIT2 "Require a static libgit2 when building a standalone static libtig" ${TIG_STANDALONE})

set(TIG_LIBGIT2_SOURCE_DIR "" CACHE PATH "Optional path to a libgit2 source checkout to build into libtig")
set(TIG_LIBGIT2_STATIC_LIBRARY "" CACHE FILEPATH "Optional path to a prebuilt static libgit2 archive")
set(TIG_LIBGIT2_INCLUDE_DIR "" CACHE PATH "Optional path to libgit2 headers when TIG_LIBGIT2_STATIC_LIBRARY is set")

if(TIG_LIBGIT2_SOURCE_DIR)
  set(_tig_saved_build_shared_libs "${BUILD_SHARED_LIBS}")
  set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build shared libraries" FORCE)
  set(BUILD_CLAR OFF CACHE BOOL "Build libgit2 tests" FORCE)
  add_subdirectory("${TIG_LIBGIT2_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/_deps/libgit2" EXCLUDE_FROM_ALL)
  set(BUILD_SHARED_LIBS "${_tig_saved_build_shared_libs}" CACHE BOOL "Build shared libraries" FORCE)

  if(NOT TARGET libgit2::libgit2)
    if(TARGET libgit2package)
      # libgit2 1.4 and later: "libgit2" is an object library, and libgit2package is the library built from it
      target_include_directories(
        libgit2package
        INTERFACE
          $<BUILD_INTERFACE:${TIG_LIBGIT2_SOURCE_DIR}/include>
          $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/_deps/libgit2/include>
      )
      add_library(libgit2::libgit2 ALIAS libgit2package)
    elseif(TARGET git2)
      add_library(libgit2::libgit2 ALIAS git2)
    elseif(TARGET libgit2)
      add_library(libgit2::libgit2 ALIAS libgit2)
    else()
      message(FATAL_ERROR "TIG_LIBGIT2_SOURCE_DIR did not define a usable libgit2 target")
    endif()
  endif()
elseif(TIG_LIBGIT2_STATIC_LIBRARY)
  if(NOT TIG_LIBGIT2_INCLUDE_DIR)
    message(FATAL_ERROR "TIG_LIBGIT2_INCLUDE_DIR is required with TIG_LIBGIT2_STATIC_LIBRARY")
  endif()

  add_library(libgit2::libgit2 STATIC IMPORTED GLOBAL)
  set_target_properties(
    libgit2::libgit2
    PROPERTIES
      IMPORTED_LOCATION "${TIG_LIBGIT2_STATIC_LIBRARY}"
      INTERFACE_INCLUDE_DIRECTORIES "${TIG_LIBGIT2_INCLUDE_DIR}"
  )
  set(LIBGIT2_LIBRARY "${TIG_LIBGIT2_STATIC_LIBRARY}")
  set(LIBGIT2_INCLUDE_DIR "${TIG_LIBGIT2_INCLUDE_DIR}")
else()
  list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
  if(TIG_REQUIRE_STATIC_LIBGIT2)
    set(LIBGIT2_USE_STATIC_LIBS ON)
  endif()
  find_package(libgit2 1.5 REQUIRED MODULE)
endif()

add_library(tig src/tig.c src/telemetry.c)
add_library(tig::tig ALIAS tig)

target_compile_features(tig PUBLIC c_std_99)
target_include_directories(
  tig
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_link_libraries(tig PRIVATE libgit2::libgit2)
target_compile_definitions(
  tig
  PUBLIC $<$<BOOL:${BUILD_SHARED_LIBS}>:TIG_SHARED>
  PRIVATE
    TIG_BUILDING_LIBRARY
    TIG_VERSION_STRING="${PROJECT_VERSION}"
)

if(TIG_STANDALONE)
  set_property(TARGET tig PROPERTY INTERFACE_LINK_LIBRARIES "")
endif()

if(TIG_STANDALONE AND NOT BUILD_SHARED_LIBS)
  get_target_property(_tig_libgit2_type libgit2::libgit2 TYPE)
  get_target_property(_tig_libgit2_location libgit2::libgit2 IMPORTED_LOCATION)

  if(_tig_libgit2_type STREQUAL "SHARED_LIBRARY")
    message(FATAL_ERROR "TIG_STANDALONE static builds require a static libgit2 target")
  endif()

  if(_tig_libgit2_location AND NOT _tig_libgit2_location MATCHES "\\.(a|lib)$")
    message(FATAL_ERROR "TIG_STANDALONE static builds require a static libgit2 archive")
  endif()

  if(_tig_libgit2_location)
    set(_tig_libgit2_archive "${_tig_libgit2_location}")
  else()
    set(_tig_libgit2_archive "$<TARGET_FILE:libgit2::libgit2>")
  endif()

  add_custom_command(
    TARGET tig
    POST_BUILD
    COMMAND
      ${CMAKE_COMMAND}
      -DOUTPUT=$<TARGET_FILE:tig>
      -DPRIMARY=$<TARGET_FILE:tig>
      -DDEPENDENCY=${_tig_libgit2_archive}
      -DCMAKE_AR=${CMAKE_AR}
      -DCMAKE_RANLIB=${CMAKE_RANLIB}
      -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/CombineStaticArchive.cmake"
    VERBATIM
  )
endif()

include(GNUInstallDirs)

install(
  TARGETS tig
  EXPORT tigTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(
  EXPORT tigTargets
  NAMESPACE tig::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/tig
)

include(CMakePackageConfigHelpers)

write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/tigConfigVersion.cmake"
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion
)

configure_package_config_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake/tigConfig.cmake.in"
  "${CMAKE_CURRENT_BINARY_DIR}/tigConfig.cmake"
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/tig
)

install(
  FILES
    "${CMAKE_CURRENT_BINARY_DIR}/tigConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/tigConfigVersion.cmake"
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/tig
)

if(TIG_BUILD_EXAMPLES)
  add_executable(tig-info examples/tig_info.c)
  target_link_libraries(tig-info PRIVATE tig::tig)

  add_executable(tig-create examples/tig_create.c)
  target_link_libraries(tig-create PRIVATE tig::tig)
endif()

if(TIG_BUILD_TESTS)
  enable_testing()

  add_executable(tig-smoke-test tests/smoke_test.c)
  target_link_libraries(tig-smoke-test PRIVATE tig::tig)

  add_test(NAME tig-smoke-test COMMAND tig-smoke-test)

  add_executable(tig-telemetry-test tests/telemetry_test.c)
  target_link_libraries(tig-telemetry-test PRIVATE tig::tig)

  add_test(NAME tig-telemetry-test-clean COMMAND ${CMAKE_COMMAND} -E remove_directory tig-telemetry-repo)
  add_test(NAME tig-telemetry-test COMMAND tig-telemetry-test)
  set_tests_properties(tig-telemetry-test-clean PROPERTIES FIXTURES_SETUP tig-telemetry-repo)
  set_tests_properties(tig-telemetry-test PROPERTIES FIXTURES_REQUIRED tig-telemetry-repo)

  # signs through ssh-keygen and checks with git, as tig-agent does
  find_program(TIG_SSH_KEYGEN ssh-keygen)
  find_program(TIG_GIT git)
  if(NOT WIN32 AND TIG_SSH_KEYGEN AND TIG_GIT)
    add_executable(tig-signing-test tests/signing_test.c)
    target_link_libraries(tig-signing-test PRIVATE tig::tig)
    add_test(NAME tig-signing-test COMMAND tig-signing-test)
  else()
    message(STATUS "Skipping tig-signing-test: needs ssh-keygen and git")
  endif()
endif()
