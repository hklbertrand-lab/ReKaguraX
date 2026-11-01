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

file(GLOB PPC_SOURCES ${CMAKE_SOURCE_DIR}/ppc/*.cpp)
file(GLOB RUNTIME_SOURCES ${CMAKE_SOURCE_DIR}/runtime/*.cpp)

add_executable(ikaruga ${RUNTIME_SOURCES} ${PPC_SOURCES})
target_include_directories(ikaruga PRIVATE
    ${CMAKE_SOURCE_DIR}/ppc
    ${CMAKE_SOURCE_DIR}/runtime
    ${CMAKE_SOURCE_DIR}/thirdparty/XenonRecomp/thirdparty/simde)
target_link_libraries(ikaruga PRIVATE XenonUtils SDL3::SDL3)
target_compile_options(ikaruga PRIVATE -march=x86-64-v3 -Wno-unused-label -Wno-unused-variable)
set_source_files_properties(${PPC_SOURCES} PROPERTIES COMPILE_OPTIONS "-w")

if(WIN32)
    add_custom_command(TARGET ikaruga POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:SDL3::SDL3> $<TARGET_FILE_DIR:ikaruga>)
endif()
