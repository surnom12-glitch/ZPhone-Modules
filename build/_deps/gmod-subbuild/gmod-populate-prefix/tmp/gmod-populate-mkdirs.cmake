# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file LICENSE.rst or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION ${CMAKE_VERSION}) # this file comes with cmake

# If CMAKE_DISABLE_SOURCE_CHANGES is set to true and the source directory is an
# existing directory in our source tree, calling file(MAKE_DIRECTORY) on it
# would cause a fatal error, even though it would be a no-op.
if(NOT EXISTS "C:/Users/duhas/Downloads/zphone_fix77_csc_secure/zphone_fix77_csc_secure/native_source/build/_deps/gmod-src")
  file(MAKE_DIRECTORY "C:/Users/duhas/Downloads/zphone_fix77_csc_secure/zphone_fix77_csc_secure/native_source/build/_deps/gmod-src")
endif()
file(MAKE_DIRECTORY
  "C:/Users/duhas/Downloads/zphone_fix77_csc_secure/zphone_fix77_csc_secure/native_source/build/_deps/gmod-build"
  "C:/Users/duhas/Downloads/zphone_fix77_csc_secure/zphone_fix77_csc_secure/native_source/build/_deps/gmod-subbuild/gmod-populate-prefix"
  "C:/Users/duhas/Downloads/zphone_fix77_csc_secure/zphone_fix77_csc_secure/native_source/build/_deps/gmod-subbuild/gmod-populate-prefix/tmp"
  "C:/Users/duhas/Downloads/zphone_fix77_csc_secure/zphone_fix77_csc_secure/native_source/build/_deps/gmod-subbuild/gmod-populate-prefix/src/gmod-populate-stamp"
  "C:/Users/duhas/Downloads/zphone_fix77_csc_secure/zphone_fix77_csc_secure/native_source/build/_deps/gmod-subbuild/gmod-populate-prefix/src"
  "C:/Users/duhas/Downloads/zphone_fix77_csc_secure/zphone_fix77_csc_secure/native_source/build/_deps/gmod-subbuild/gmod-populate-prefix/src/gmod-populate-stamp"
)

set(configSubDirs Debug)
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "C:/Users/duhas/Downloads/zphone_fix77_csc_secure/zphone_fix77_csc_secure/native_source/build/_deps/gmod-subbuild/gmod-populate-prefix/src/gmod-populate-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "C:/Users/duhas/Downloads/zphone_fix77_csc_secure/zphone_fix77_csc_secure/native_source/build/_deps/gmod-subbuild/gmod-populate-prefix/src/gmod-populate-stamp${cfgdir}") # cfgdir has leading slash
endif()
