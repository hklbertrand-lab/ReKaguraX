# Runtime Xbox 360 recompilé : include(chemin/vers/Xbox360Runtime/runtime.cmake) depuis le CMakeLists d'un jeu, puis
#   x360_add_game(<cible> GAME_DIR <dossier du jeu> SOURCES <game.cpp, hooks, imports_stub.cpp...>)
# Le dossier du jeu contient ppc/ (XenonRecomp), build/shaders/shader_cache.cpp (XenosRecomp) et thirdparty/
# (XenonRecomp, XenosRecomp, volk, Vulkan-Headers, SDL3 sous Windows, ffmpeg réduit au XMA : voir setup.sh d'Ikaruga).
set(X360_RUNTIME_DIR ${CMAKE_CURRENT_LIST_DIR})

function(x360_add_game target)
    cmake_parse_arguments(G "" "GAME_DIR" "SOURCES" ${ARGN})
    set(D ${G_GAME_DIR})
    set(R ${X360_RUNTIME_DIR}/src)
    file(GLOB PPC_SOURCES ${D}/ppc/*.cpp)
    file(GLOB RUNTIME_SOURCES ${R}/*.cpp)
    set(XENOS_THIRDPARTY ${D}/thirdparty/XenosRecomp/thirdparty)

    add_executable(${target} ${RUNTIME_SOURCES} ${G_SOURCES} ${PPC_SOURCES}
        ${D}/thirdparty/volk/volk.c
        ${D}/build/shaders/shader_cache.cpp
        ${XENOS_THIRDPARTY}/smol-v/source/smolv.cpp)
    target_include_directories(${target} PRIVATE
        ${D}/ppc
        ${R}
        ${D}/thirdparty/XenonRecomp/thirdparty/simde
        ${D}/thirdparty/volk
        ${D}/thirdparty/Vulkan-Headers/include
        ${XENOS_THIRDPARTY}/smol-v/source
        ${XENOS_THIRDPARTY}/xxHash
        ${XENOS_THIRDPARTY}/zstd/lib
        ${D}/thirdparty/ffmpeg-include)
    target_compile_definitions(${target} PRIVATE VK_NO_PROTOTYPES)
    set(FFMPEG_BUILD ${D}/thirdparty/ffmpeg-build)
    target_link_libraries(${target} PRIVATE ${FFMPEG_BUILD}/libavcodec/libavcodec.a ${FFMPEG_BUILD}/libavutil/libavutil.a $<$<PLATFORM_ID:Windows>:bcrypt>)
    target_link_libraries(${target} PRIVATE XenonUtils SDL3::SDL3 libzstd_static $<$<PLATFORM_ID:Windows>:winmm>)
    target_compile_options(${target} PRIVATE -march=x86-64-v3 -Wno-unused-label -Wno-unused-variable)
    target_link_options(${target} PRIVATE -Wl,-Map=${CMAKE_BINARY_DIR}/${target}.map)
    set_source_files_properties(${PPC_SOURCES} PROPERTIES COMPILE_OPTIONS "-w;-include;${R}/mmio.h")

    if(WIN32)
        add_custom_command(TARGET ${target} POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:SDL3::SDL3> $<TARGET_FILE_DIR:${target}>)
    endif()
endfunction()
