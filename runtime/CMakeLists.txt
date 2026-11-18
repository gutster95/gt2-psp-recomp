cmake_minimum_required(VERSION 3.10)
project(gt2psp C)

set(GEN_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../generated")
file(GLOB RT_SRC "${CMAKE_CURRENT_SOURCE_DIR}/src/*.c")
file(GLOB GEN_SRC "${GEN_DIR}/*.c")

add_executable(gt2psp ${RT_SRC} ${GEN_SRC})
target_include_directories(gt2psp PRIVATE include src "${GEN_DIR}")
target_compile_options(gt2psp PRIVATE -O2 -G0 -fno-strict-aliasing -Wall -Wno-unused-label -Wno-unused-variable)
set_source_files_properties(${GEN_SRC} PROPERTIES COMPILE_OPTIONS "-w")
target_link_libraries(gt2psp PRIVATE pspgu psputility pspaudio pspdebug pspdisplay pspge psprtc pspctrl psppower)
create_pbp_file(TARGET gt2psp TITLE "GT2 recomp" BUILD_PRX)
