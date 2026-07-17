cmake_minimum_required(VERSION 4.0)

find_package(cmake-harden REQUIRED PATHS node_modules/cmake-harden)

project(gpuinfo C)

add_library(gpuinfo OBJECT)

harden(gpuinfo)

set_target_properties(
  gpuinfo
  PROPERTIES
  C_STANDARD 99
  POSITION_INDEPENDENT_CODE ON
)

target_sources(
  gpuinfo
  INTERFACE
    include/gpuinfo.h
)

if(APPLE)
  enable_language(OBJC)

  target_sources(
    gpuinfo
    PRIVATE
      src/darwin.m
  )

  target_link_libraries(
    gpuinfo
    PUBLIC
      "-framework Foundation"
      "-framework Metal"
      "-framework IOKit"
  )
elseif(ANDROID)
  target_sources(
    gpuinfo
    PRIVATE
      src/android.c
  )

  target_link_libraries(
    gpuinfo
    PUBLIC
      ${CMAKE_DL_LIBS}
  )
elseif(LINUX)
  target_sources(
    gpuinfo
    PRIVATE
      src/linux.c
  )

  target_link_libraries(
    gpuinfo
    PUBLIC
      ${CMAKE_DL_LIBS}
  )
elseif(WIN32)
  enable_language(CXX)

  target_sources(
    gpuinfo
    PRIVATE
      src/win32.cc
  )

  target_link_libraries(
    gpuinfo
    PUBLIC
      advapi32
      dxgi
      pdh
  )
endif()

target_include_directories(
  gpuinfo
  PUBLIC
    include
)

add_library(gpuinfo_shared SHARED)

set_target_properties(
  gpuinfo_shared
  PROPERTIES
  OUTPUT_NAME gpuinfo
  WINDOWS_EXPORT_ALL_SYMBOLS ON
)

target_link_libraries(
  gpuinfo_shared
  PUBLIC
    gpuinfo
)

add_library(gpuinfo_static STATIC)

set_target_properties(
  gpuinfo_static
  PROPERTIES
  OUTPUT_NAME gpuinfo
  PREFIX lib
)

target_link_libraries(
  gpuinfo_static
  PUBLIC
    gpuinfo
)

install(TARGETS gpuinfo_shared gpuinfo_static)

install(FILES include/gpuinfo.h DESTINATION include)

if(PROJECT_IS_TOP_LEVEL)
  enable_testing()

  add_subdirectory(test)
endif()
