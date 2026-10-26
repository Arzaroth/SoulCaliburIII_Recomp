cmake_minimum_required(VERSION 3.21)
project(SoulCaliburIII_Recomp LANGUAGES C CXX)

set(SC3_GENERATED_DIR "${CMAKE_SOURCE_DIR}/generated" CACHE PATH
    "Output of ps2_recomp for SLUS_212.16 (scripts/recomp.sh), VU1 microcode in vu1/ (scripts/compile_vu1.sh)")

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
    set_source_files_properties(${SC3_VU1_SOURCES} PROPERTIES SKIP_UNITY_BUILD_INCLUSION ON)
else()
    message(STATUS "SoulCaliburIII_Recomp: no compiled VU1 microcode in ${SC3_GENERATED_DIR}/vu1, "
        "VU1 runs in the interpreter (scripts/compile_vu1.sh)")
endif()

target_sources(ps2EntryRunner PRIVATE ${SC3_GENERATED_SOURCES} ${SC3_RUNTIME_SOURCES} ${SC3_VU1_SOURCES})
# ps2_vu1_compile output includes "../lib/vu/ps2_vu1_step.h", a path relative to the runtime's runner directory.
target_include_directories(ps2EntryRunner PRIVATE
    "${SC3_GENERATED_DIR}"
    "${CMAKE_SOURCE_DIR}/PS2Recomp/ps2xRuntime/src/runner")

list(LENGTH SC3_GENERATED_SOURCES _sc3_count)
list(LENGTH SC3_VU1_SOURCES _sc3_vu1_count)
message(STATUS "SoulCaliburIII_Recomp: ${_sc3_count} generated sources, ${_sc3_vu1_count} compiled VU1 file(s)")
