#
# - Find minizip-ng library, only if it has the compatibility layer with
#   the original minizip
#
#  MINIZIPNG_INCLUDE_DIRS - where to find minizip-ng headers.
#  MINIZIPNG_LIBRARIES    - List of libraries when using minizip-ng.
#  MINIZIPNG_FOUND        - True if minizip-ng is found.
#  MINIZIPNG_DLL_DIR      - (Windows) Path to the minizip-ng DLL.
#  MINIZIPNG_DLLS         - (Windows) Name of the minizip-ng DLL.

FindWSWinLibs("minizip-ng" "MINIZIPNG_HINTS")

if(NOT USE_REPOSITORY)
  find_package(PkgConfig QUIET)
  pkg_search_module(MINIZIPNG QUIET minizip-ng)
endif()

find_path(MINIZIPNG_INCLUDE_DIR
  NAMES
    unzip.h
    minizip-ng/unzip.h
  HINTS
    ${MINIZIPNG_INCLUDE_DIRS}
    "${MINIZIPNG_HINTS}/include"
)

get_filename_component(MINIZIPNG_PARENT_DIR ${MINIZIPNG_INCLUDE_DIR} DIRECTORY)
if(EXISTS "${MINIZIPNG_PARENT_DIR}/minizip-ng/unzip.h")
  set(MINIZIPNG_INCLUDE_DIR "${MINIZIPNG_PARENT_DIR}")
endif()

find_library(MINIZIPNG_LIBRARY
  NAMES
    libminizip-ng minizip-ng
  HINTS
    ${MINIZIPNG_LIBRARY_DIRS}
    ${MINIZIPNG_HINTS}/lib
  PATH
    /opt
    /opt/homebrew/lib
)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(Minizipng
  REQUIRED_VARS   MINIZIPNG_LIBRARY MINIZIPNG_INCLUDE_DIR
  VERSION_VAR     MINIZIPNG_VERSION)

if(MINIZIPNG_FOUND)
  set(MINIZIPNG_LIBRARIES ${MINIZIPNG_LIBRARY})

  find_library(BZ2_LIBRARY
    NAMES
      bz2
    HINTS
      ${MINIZIPNG_LIBRARY_DIRS}
      "${MINIZIPNG_HINTS}/lib"
  )
  list(APPEND MINIZIPNG_LIBRARIES ${BZ2_LIBRARY})

  find_library(LZMA_LIBRARY
    NAMES
      lzma
    HINTS
      ${MINIZIPNG_LIBRARY_DIRS}
      "${MINIZIPNG_HINTS}/lib"
  )
  list(APPEND MINIZIPNG_LIBRARIES ${LZMA_LIBRARY})

  find_library(ZSTD_LIBRARY
    NAMES
      zstd
    HINTS
      ${MINIZIPNG_LIBRARY_DIRS}
      "${MINIZIPNG_HINTS}/lib"
  )
  list(APPEND MINIZIPNG_LIBRARIES ${ZSTD_LIBRARY})

  if(WIN32)
    find_library(Bcrypt_LIBRARY
      NAMES
        Bcrypt
    )

    list(APPEND MINIZIPNG_LIBRARIES ${Bcrypt_LIBRARY})
  endif()

  # do we need openssl on *nix*

  # message(STATUS "Minizip-ng, MINIZIPNG_LIBRARIES ${MINIZIPNG_LIBRARIES}")

  set(MINIZIPNG_INCLUDE_DIRS ${MINIZIPNG_INCLUDE_DIR})
  set(HAVE_MINIZIPNG ON)

  # Some distributions have minizip-ng code instead of the original zlib contrib
  # library but keep the old minizip name (because minizip-ng is
  # better maintained and provides a compatibility layer). However the
  # minizip-ng compatibility layer has some issues. We need to check
  # for renamed struct members to avoid an endless game of whack-a-mole.
  include(CheckStructHasMember)
  check_struct_has_member("zip_fileinfo" "dos_date" "minizip-ng/zip.h" HAVE_MZCOMPAT_DOS_DATE)

  if(WIN32)
    set(MINIZIPNG_DLL_DIR "${MINIZIPNG_HINTS}/bin"
      CACHE PATH "Path to Minizip DLL"
    )

    # minizip-ng from vcpkg provides bz2, zstd, zlib (*not* zlib-ng),
    # and liblzma DLLs. liblzma used to be provided in the vcpkg-export
    # (glib, libxml2, and zlib) bundle, but since vcpkg tag 2025.04.09
    # isn't since libxml2 no longer depends on it by default.
    # XXX - This can causes this zstd.dll to be used instead of the one
    # from the separately packaged zstd
    AddWSWinDLLS(MINIZIPNG MINIZIPNG_HINTS "bz2*" "liblzma*" "zstd*")

    mark_as_advanced(MINIZIPNG_DLL_DIR MINIZIPNG_DLLS MINIZIPNG_PDBS)
  endif()
  if(MINIZIPNG_INCLUDE_DIR)
    set(_version_regex "^#define[ \t]+MZ_VERSION[ \t(]+\"([^\"]+)\".*")
    file(STRINGS "${MINIZIPNG_INCLUDE_DIR}/minizip-ng/mz.h" MINIZIPNG_VERSION REGEX "${_version_regex}")
    string(REGEX REPLACE "${_version_regex}" "\\1" MINIZIPNG_VERSION "${MINIZIPNG_VERSION}")
    unset(_version_regex)
  endif()
else()
  set(MINIZIPNG_LIBRARIES)
  set(MINIZIPNG_INCLUDE_DIRS)
  set(MINIZIPNG_DLL_DIR)
  set(MINIZIPNG_DLLS)
endif()

mark_as_advanced(MINIZIPNG_LIBRARIES MINIZIPNG_INCLUDE_DIRS)
