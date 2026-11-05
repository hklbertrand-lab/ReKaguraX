cmake_minimum_required(VERSION 3.20)
project(Ikaruga CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# fmt (dans XenonRecomp) oublie <cstdlib> avec les libc++ récentes
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -include cstdlib -include cstring")

# Outils XenonRecomp + bibliothèque XenonUtils (chargement du XEX)
add_subdirectory(thirdparty/XenonRecomp EXCLUDE_FROM_ALL)

# SDL3 : copie MinGW locale sous Windows, paquet système sous Linux
if(WIN32)
    list(APPEND CMAKE_PREFIX_PATH "${CMAKE_SOURCE_DIR}/thirdparty/SDL3")
endif()
find_package(SDL3 REQUIRED CONFIG)

# zstd + smol-v (fournis avec XenosRecomp) pour décompresser le cache SPIR-V
set(XENOS_THIRDPARTY ${CMAKE_SOURCE_DIR}/thirdparty/XenosRecomp/thirdparty)
set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(${XENOS_THIRDPARTY}/zstd/build/cmake ${CMAKE_BINARY_DIR}/zstd EXCLUDE_FROM_ALL)

# Runtime Xbox 360 commun (noyau, GPU Xenos -> Vulkan, audio XMA, réglages) : dossier frère, ou X360_RUNTIME_DIR
set(X360_RUNTIME_DIR ${CMAKE_SOURCE_DIR}/../Xbox360Runtime CACHE PATH "Dossier de Xbox360Runtime")
include(${X360_RUNTIME_DIR}/runtime.cmake)

file(GLOB GAME_SOURCES ${CMAKE_SOURCE_DIR}/game/*.cpp)
x360_add_game(ikaruga GAME_DIR ${CMAKE_SOURCE_DIR} SOURCES ${GAME_SOURCES})
