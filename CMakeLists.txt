cmake_minimum_required(VERSION 3.21)
project(SoulCaliburIII_Recomp LANGUAGES C CXX)

set(SC3_GENERATED_DIR "${CMAKE_SOURCE_DIR}/generated" CACHE PATH
    "Output of ps2_recomp for SLUS_212.16 (scripts/recomp.sh), VU1 microcode compiled by ps2_vu_aot in vu1/")

set(PS2X_BUILD_RECOMP OFF CACHE BOOL "" FORCE)
set(PS2X_BUILD_ANALYZER OFF CACHE BOOL "" FORCE)
set(PS2X_BUILD_TEST OFF CACHE BOOL "" FORCE)
set(PS2X_BUILD_STUDIO OFF CACHE BOOL "" FORCE)
set(PS2X_ENABLE_AGRESSIVE_LOGS OFF CACHE BOOL "Enable very verbose/agressive PS2 runtime logs")

add_subdirectory(PS2Recomp)

file(GLOB SC3_GENERATED_SOURCES CONFIGURE_DEPENDS "${SC3_GENERATED_DIR}/*.cpp")
if(NOT SC3_GENERATED_SOURCES)
    message(FATAL_ERROR "No generated sources in ${SC3_GENERATED_DIR}. Run scripts/recomp.sh first.")
endif()

# The runtime's placeholder function table clashes with the generated register_functions.cpp.
get_target_property(_sc3_runner_sources ps2EntryRunner SOURCES)
list(FILTER _sc3_runner_sources EXCLUDE REGEX "src/runner/register_functions\\.cpp$")
set_property(TARGET ps2EntryRunner PROPERTY SOURCES ${_sc3_runner_sources})

file(GLOB SC3_RUNTIME_SOURCES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/runtime/*.cpp")

file(GLOB SC3_VU1_SOURCES CONFIGURE_DEPENDS "${SC3_GENERATED_DIR}/vu1/*.cpp")
if(SC3_VU1_SOURCES)
    set_property(GLOBAL APPEND PROPERTY JOB_POOLS sc3_vu1_aot=4)
    add_library(sc3_vu1_aot OBJECT ${SC3_VU1_SOURCES})
    target_link_libraries(sc3_vu1_aot PRIVATE ps2_runtime)
    set_target_properties(sc3_vu1_aot PROPERTIES UNITY_BUILD OFF JOB_POOL_COMPILE sc3_vu1_aot)
    target_compile_options(sc3_vu1_aot PRIVATE -O3 -g0)
    target_sources(ps2EntryRunner PRIVATE $<TARGET_OBJECTS:sc3_vu1_aot>)
endif()

# The game's IRX modules recompiled by PS2Recomp's tools/iop_recomp.py (scripts/iop_recomp.sh), run with PS2X_IOP_NATIVE=1.
file(GLOB SC3_IOP_SOURCES CONFIGURE_DEPENDS "${SC3_GENERATED_DIR}/iop/*.cpp")
if(SC3_IOP_SOURCES)
    add_library(sc3_iop_native OBJECT ${SC3_IOP_SOURCES})
    target_link_libraries(sc3_iop_native PRIVATE ps2_iop)
    set_target_properties(sc3_iop_native PROPERTIES UNITY_BUILD OFF)
    target_compile_options(sc3_iop_native PRIVATE -O2 -g0)
    target_sources(ps2EntryRunner PRIVATE $<TARGET_OBJECTS:sc3_iop_native>)
endif()

target_sources(ps2EntryRunner PRIVATE ${SC3_GENERATED_SOURCES} ${SC3_RUNTIME_SOURCES})
target_include_directories(ps2EntryRunner PRIVATE "${SC3_GENERATED_DIR}")

list(LENGTH SC3_GENERATED_SOURCES _sc3_count)
list(LENGTH SC3_VU1_SOURCES _sc3_vu1_count)
list(LENGTH SC3_IOP_SOURCES _sc3_iop_count)
message(STATUS "SoulCaliburIII_Recomp: ${_sc3_count} generated sources, ${_sc3_vu1_count} compiled VU1 file(s), ${_sc3_iop_count} recompiled IOP module(s)")
