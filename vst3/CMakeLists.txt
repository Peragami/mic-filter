cmake_minimum_required(VERSION 3.25)
project(MicFilter VERSION 0.23.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# --- VST3 SDK ---------------------------------------------------------------
# Resolution order:
#   1. -DVST3_SDK_PATH=<path to vst3sdk checkout>
#   2. ./vst3sdk next to this file (git clone --recurse-submodules)
#   3. FetchContent clone from Steinberg's official GitHub
set(VST3_SDK_PATH "" CACHE PATH "Path to a Steinberg VST3 SDK checkout")
if(NOT VST3_SDK_PATH AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/vst3sdk/CMakeLists.txt")
    set(VST3_SDK_PATH "${CMAKE_CURRENT_SOURCE_DIR}/vst3sdk")
endif()
if(NOT VST3_SDK_PATH)
    include(FetchContent)
    FetchContent_Declare(vst3sdk
        GIT_REPOSITORY https://github.com/steinbergmedia/vst3sdk.git
        GIT_TAG        v3.8.1_build_84
        GIT_SHALLOW    TRUE
        GIT_SUBMODULES_RECURSE TRUE)
    FetchContent_MakeAvailable(vst3sdk)
    set(VST3_SDK_PATH "${vst3sdk_SOURCE_DIR}")
endif()
message(STATUS "MicFilter: using VST3 SDK at ${VST3_SDK_PATH}")

# No VSTGUI (no custom GUI by design), no SDK examples: keep the build minimal.
set(SMTG_ENABLE_VSTGUI_SUPPORT OFF CACHE BOOL "" FORCE)
set(SMTG_ENABLE_VST3_PLUGIN_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SMTG_ENABLE_VST3_HOSTING_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SMTG_CREATE_PLUGIN_LINK OFF CACHE BOOL "" FORCE)
# SMTG_RUN_VST_VALIDATOR stays at its default (ON): the SDK runs its validator
# as a post-build step when the validator target exists.
set(vst3sdk_SOURCE_DIR "${VST3_SDK_PATH}")

add_subdirectory(${VST3_SDK_PATH} ${CMAKE_BINARY_DIR}/vst3sdk)
smtg_enable_vst3_sdk()

# --- Plugin -----------------------------------------------------------------
smtg_add_vst3plugin(MicFilter
    source/factory.cpp
    source/processor.h
    source/processor.cpp
    source/controller.h
    source/controller.cpp
    source/paramids.h
    source/version.h
)

# PureDsp.h is included as "Source/dsp/PureDsp.h" from the mic-filter root.
target_include_directories(MicFilter PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(MicFilter PRIVATE sdk)

# --- RNNoise (AI noise suppression, BSD-3-Clause, Xiph) ----------------------
set(RNNOISE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../thirdparty/rnnoise)
set(RNNOISE_SOURCES
    ${RNNOISE_DIR}/src/denoise.c ${RNNOISE_DIR}/src/rnn.c ${RNNOISE_DIR}/src/pitch.c
    ${RNNOISE_DIR}/src/kiss_fft.c ${RNNOISE_DIR}/src/celt_lpc.c ${RNNOISE_DIR}/src/nnet.c
    ${RNNOISE_DIR}/src/nnet_default.c ${RNNOISE_DIR}/src/parse_lpcnet_weights.c
    ${RNNOISE_DIR}/src/rnnoise_data.c ${RNNOISE_DIR}/src/rnnoise_tables.c
)
target_sources(MicFilter PRIVATE ${RNNOISE_SOURCES})
target_include_directories(MicFilter PRIVATE
    ${RNNOISE_DIR}/include ${RNNOISE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../thirdparty/rnnoise-build)

# MinGW cross-builds: statically link the GCC runtimes so the .vst3 loads on
# a stock Windows install (no libgcc_s_seh-1.dll / libstdc++-6.dll needed).
if(MINGW)
    target_link_options(MicFilter PRIVATE -static-libgcc -static-libstdc++)
endif()

smtg_target_configure_version_file(MicFilter)
