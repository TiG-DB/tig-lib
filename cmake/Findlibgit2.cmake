include(FindPackageHandleStandardArgs)

find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
  pkg_check_modules(PC_LIBGIT2 QUIET libgit2)
endif()

find_path(
  LIBGIT2_INCLUDE_DIR
  NAMES git2.h
  HINTS
    ${PC_LIBGIT2_INCLUDE_DIRS}
    /opt/homebrew/opt/libgit2
    /usr/local/opt/libgit2
  PATH_SUFFIXES include
)

if(LIBGIT2_USE_STATIC_LIBS)
  set(_libgit2_original_suffixes ${CMAKE_FIND_LIBRARY_SUFFIXES})
  set(CMAKE_FIND_LIBRARY_SUFFIXES ${CMAKE_STATIC_LIBRARY_SUFFIX})
endif()

find_library(
  LIBGIT2_LIBRARY
  NAMES git2 libgit2
  HINTS
    ${PC_LIBGIT2_LIBRARY_DIRS}
    /opt/homebrew/opt/libgit2
    /usr/local/opt/libgit2
  PATH_SUFFIXES lib
)

if(LIBGIT2_USE_STATIC_LIBS)
  set(CMAKE_FIND_LIBRARY_SUFFIXES ${_libgit2_original_suffixes})
endif()

if(LIBGIT2_INCLUDE_DIR AND EXISTS "${LIBGIT2_INCLUDE_DIR}/git2/version.h")
  file(STRINGS "${LIBGIT2_INCLUDE_DIR}/git2/version.h" _libgit2_version_lines
       REGEX "^#define LIBGIT2_VER(SION)?_(MAJOR|MINOR|REVISION) +[0-9]+")

  foreach(_part MAJOR MINOR REVISION)
    # newer libgit2 releases spell these LIBGIT2_VERSION_* rather than LIBGIT2_VER_*
    string(REGEX MATCH "#define LIBGIT2_VER(SION)?_${_part} +([0-9]+)" _match
           "${_libgit2_version_lines}")
    set(_libgit2_${_part} "${CMAKE_MATCH_2}")
  endforeach()

  if(DEFINED _libgit2_MAJOR AND DEFINED _libgit2_MINOR AND DEFINED _libgit2_REVISION)
    set(LIBGIT2_VERSION
        "${_libgit2_MAJOR}.${_libgit2_MINOR}.${_libgit2_REVISION}")
  endif()
endif()

find_package_handle_standard_args(
  libgit2
  REQUIRED_VARS LIBGIT2_LIBRARY LIBGIT2_INCLUDE_DIR
  VERSION_VAR LIBGIT2_VERSION
)

if(libgit2_FOUND AND NOT TARGET libgit2::libgit2)
  add_library(libgit2::libgit2 UNKNOWN IMPORTED)
  set_target_properties(
    libgit2::libgit2
    PROPERTIES
      IMPORTED_LOCATION "${LIBGIT2_LIBRARY}"
      INTERFACE_INCLUDE_DIRECTORIES "${LIBGIT2_INCLUDE_DIR}"
  )

  if(PC_LIBGIT2_CFLAGS_OTHER)
    set_property(TARGET libgit2::libgit2 PROPERTY
                 INTERFACE_COMPILE_OPTIONS "${PC_LIBGIT2_CFLAGS_OTHER}")
  endif()

  if(PC_LIBGIT2_LDFLAGS_OTHER)
    set_property(TARGET libgit2::libgit2 PROPERTY
                 INTERFACE_LINK_OPTIONS "${PC_LIBGIT2_LDFLAGS_OTHER}")
  endif()
endif()

mark_as_advanced(LIBGIT2_INCLUDE_DIR LIBGIT2_LIBRARY)
