cmake_minimum_required(VERSION 3.10)
project(gt2psp C)

# One runtime build per disc profile (the launcher in launcher/ starts one of them): GT2_GAME=sim (generated/,
# SCUS_944.88) or arc (generated/arcade/ from `python -m gt2psp recomp --disc arc`, SCUS_944.55); GT2_GEN_DIR picks
# another recompiled disc (profiles sim_plus, arc_plus, ...). The disc's constants are in the generated game.h.
set(GT2_GAME "sim" CACHE STRING "disc of this build: sim or arc")
set(GT2_GEN_DIR "" CACHE PATH "generated code of the disc (default: generated/ or generated/arcade/)")
if(GT2_GEN_DIR)
  set(GEN_DIR "${GT2_GEN_DIR}")
elseif(GT2_GAME STREQUAL "arc")
  set(GEN_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../generated/arcade")
else()
  set(GEN_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../generated")
endif()
file(GLOB RT_SRC "${CMAKE_CURRENT_SOURCE_DIR}/src/*.c")
file(GLOB GEN_SRC "${GEN_DIR}/*.c")

add_executable(gt2psp ${RT_SRC} ${GEN_SRC})
target_include_directories(gt2psp PRIVATE include src "${GEN_DIR}")
# Media Engine library (mcidclan/psp-media-engine-custom-core, MIT), built by scripts/me/build-sample.sh
set(ME_LIB_DIR "/root/mepoc/build/mecorelib" CACHE PATH "built me-core libraries")
set(ME_INC_DIR "/root/meinc" CACHE PATH "include dir with me-core-mapper/")
target_include_directories(gt2psp PRIVATE "${ME_INC_DIR}")
target_compile_options(gt2psp PRIVATE -O2 -G0 -ffunction-sections -fno-strict-aliasing -Wall -Wno-unused-label -Wno-unused-variable)
set(ALL_EXTRA "" CACHE STRING "extra compile and link options for everything (profile-guided optimization)")
separate_arguments(ALL_EXTRA_L UNIX_COMMAND "${ALL_EXTRA}")
target_compile_options(gt2psp PRIVATE ${ALL_EXTRA_L})
target_link_options(gt2psp PRIVATE ${ALL_EXTRA_L})
set(GEN_EXTRA "" CACHE STRING "extra compile options for the generated code (experiments)")
separate_arguments(GEN_EXTRA_L UNIX_COMMAND "${GEN_EXTRA}")
set_source_files_properties(${GEN_SRC} PROPERTIES COMPILE_OPTIONS "-w;${GEN_EXTRA_L}")
set(RT_EXTRA "" CACHE STRING "extra compile options for ge.c, gpu.c, io.c, gte.c, core.c (experiments)")
separate_arguments(RT_EXTRA_L UNIX_COMMAND "${RT_EXTRA}")
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/src/ge.c" "${CMAKE_CURRENT_SOURCE_DIR}/src/gpu.c" "${CMAKE_CURRENT_SOURCE_DIR}/src/io.c" "${CMAKE_CURRENT_SOURCE_DIR}/src/gte.c" "${CMAKE_CURRENT_SOURCE_DIR}/src/core.c" PROPERTIES COMPILE_OPTIONS "${RT_EXTRA_L}")
target_link_libraries(gt2psp PRIVATE "${ME_LIB_DIR}/libme-core.a" "${ME_LIB_DIR}/libme-core-lib.a" pspsdk pspgu psputility pspaudio pspdebug pspdisplay pspge psprtc pspctrl psppower)
create_pbp_file(TARGET gt2psp TITLE "GT2 recomp" BUILD_PRX)   # the PRX is what the launcher starts

# Own copy of the SDK's PRX linker script with the hot functions first (scripts/make_linkfile.py)
get_target_property(GT2_LINK_OPTS gt2psp LINK_OPTIONS)
list(TRANSFORM GT2_LINK_OPTS REPLACE "-T.*/linkfile\.prx" "-T${CMAKE_CURRENT_SOURCE_DIR}/ld/linkfile.prx")
set_target_properties(gt2psp PROPERTIES LINK_OPTIONS "${GT2_LINK_OPTS}")
set_property(TARGET gt2psp APPEND PROPERTY LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/ld/linkfile.prx")
