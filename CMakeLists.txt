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

# zstd + smol-v (fournis avec XenosRecomp) pour décompresser le cache SPIR-V
set(XENOS_THIRDPARTY ${CMAKE_SOURCE_DIR}/thirdparty/XenosRecomp/thirdparty)
set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(${XENOS_THIRDPARTY}/zstd/build/cmake ${CMAKE_BINARY_DIR}/zstd EXCLUDE_FROM_ALL)

add_executable(ikaruga ${RUNTIME_SOURCES} ${PPC_SOURCES}
    ${CMAKE_SOURCE_DIR}/thirdparty/volk/volk.c
    ${CMAKE_SOURCE_DIR}/build/shaders/shader_cache.cpp
    ${XENOS_THIRDPARTY}/smol-v/source/smolv.cpp)
target_include_directories(ikaruga PRIVATE
    ${CMAKE_SOURCE_DIR}/ppc
    ${CMAKE_SOURCE_DIR}/runtime
    ${CMAKE_SOURCE_DIR}/thirdparty/XenonRecomp/thirdparty/simde
    ${CMAKE_SOURCE_DIR}/thirdparty/volk
    ${CMAKE_SOURCE_DIR}/thirdparty/Vulkan-Headers/include
    ${XENOS_THIRDPARTY}/smol-v/source
    ${XENOS_THIRDPARTY}/xxHash
    ${XENOS_THIRDPARTY}/zstd/lib)
target_compile_definitions(ikaruga PRIVATE VK_NO_PROTOTYPES)
# FFmpeg réduit au décodeur XMA (thirdparty/ffmpeg-build, voir setup.sh)
set(FFMPEG_BUILD ${CMAKE_SOURCE_DIR}/thirdparty/ffmpeg-build)
target_include_directories(ikaruga PRIVATE ${CMAKE_SOURCE_DIR}/thirdparty/ffmpeg-include)
target_link_libraries(ikaruga PRIVATE ${FFMPEG_BUILD}/libavcodec/libavcodec.a ${FFMPEG_BUILD}/libavutil/libavutil.a $<$<PLATFORM_ID:Windows>:bcrypt>)
target_link_libraries(ikaruga PRIVATE XenonUtils SDL3::SDL3 libzstd_static $<$<PLATFORM_ID:Windows>:winmm>)
target_compile_options(ikaruga PRIVATE -march=x86-64-v3 -Wno-unused-label -Wno-unused-variable)
target_link_options(ikaruga PRIVATE -Wl,-Map=${CMAKE_BINARY_DIR}/ikaruga.map)
set_source_files_properties(${PPC_SOURCES} PROPERTIES COMPILE_OPTIONS "-w;-include;${CMAKE_SOURCE_DIR}/runtime/mmio.h")

if(WIN32)
    add_custom_command(TARGET ikaruga POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:SDL3::SDL3> $<TARGET_FILE_DIR:ikaruga>)
endif()
