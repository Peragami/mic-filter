cmake_minimum_required(VERSION 3.20)
project(MicFilterClient VERSION 0.5.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---------------------------------------------------------------------------
# VST3 SDK location.
# The SDK itself is fetched/owned by the vst3/ workstream, NOT vendored here.
# Point this at the SDK root (the folder that contains pluginterfaces/ and
# base/):
#   cmake -B build -DVST3_SDK_PATH=C:/dev/vst3sdk
# Default search (relative to this file):
#   ../vst3/VST3_SDK , ../vst3/sdk , ../vst3/vst3sdk , $ENV{VST3_SDK_PATH}
# If no SDK is found the client still builds, but the VST3 host becomes a
# stub that fails to load a plugin with a clear error. Build with the SDK
# for the real host.
# ---------------------------------------------------------------------------
set(VST3_SDK_PATH "" CACHE PATH "Path to the VST3 SDK root")
if(NOT VST3_SDK_PATH AND DEFINED ENV{VST3_SDK_PATH})
    set(VST3_SDK_PATH "$ENV{VST3_SDK_PATH}")
endif()
if(NOT VST3_SDK_PATH)
    foreach(candidate
            "${CMAKE_CURRENT_SOURCE_DIR}/../vst3/VST3_SDK"
            "${CMAKE_CURRENT_SOURCE_DIR}/../vst3/sdk"
            "${CMAKE_CURRENT_SOURCE_DIR}/../vst3/vst3sdk")
        if(EXISTS "${candidate}/pluginterfaces/vst/ivstcomponent.h")
            set(VST3_SDK_PATH "${candidate}")
            break()
        endif()
    endforeach()
endif()

set(MICFILTER_HAS_VST3_SDK OFF)
if(VST3_SDK_PATH AND EXISTS "${VST3_SDK_PATH}/pluginterfaces/vst/ivstcomponent.h")
    set(MICFILTER_HAS_VST3_SDK ON)
    message(STATUS "MicFilterClient: using VST3 SDK at ${VST3_SDK_PATH}")
else()
    message(WARNING "MicFilterClient: VST3 SDK not found (VST3_SDK_PATH='${VST3_SDK_PATH}'). "
                    "Building with a stub VST3 host. Set -DVST3_SDK_PATH=<sdk root> for the real host.")
endif()

add_executable(MicFilterClient WIN32
    src/log.cpp
    src/log.h
    src/main.cpp
    src/wasapi_engine.cpp
    src/wasapi_engine.h
    src/vst3_host.cpp
    src/vst3_host.h
)

# ClientConfig.h lives in ../Source and is the single source of defaults
# (sample rate, buffer sizes, WASAPI mode priority, sound mode).
target_include_directories(MicFilterClient PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/.."
)
if(MICFILTER_HAS_VST3_SDK)
    target_include_directories(MicFilterClient PRIVATE "${VST3_SDK_PATH}")
    target_compile_definitions(MicFilterClient PRIVATE MICFILTER_HAS_VST3_SDK=1)
    # The interface IIDs (IComponent::iid etc.) are *defined* in these four
    # pluginterfaces translation units; they are the only SDK sources a host
    # needs to compile. Nothing else from the SDK is linked.
    target_sources(MicFilterClient PRIVATE
        "${VST3_SDK_PATH}/pluginterfaces/base/conststringtable.cpp"
        "${VST3_SDK_PATH}/pluginterfaces/base/coreiids.cpp"
        "${VST3_SDK_PATH}/pluginterfaces/base/funknown.cpp"
        "${VST3_SDK_PATH}/pluginterfaces/base/ustring.cpp"
    )
endif()

if(MINGW)
    target_link_options(MicFilterClient PRIVATE -municode) # wWinMain entry point
endif()

target_compile_definitions(MicFilterClient PRIVATE
    UNICODE _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _WIN32_WINNT=0x0A00   # Windows 10 (IAudioClient3 low-latency shared mode)
)

if(MSVC)
    target_compile_options(MicFilterClient PRIVATE /utf-8 /W3)
else()
    target_compile_options(MicFilterClient PRIVATE -Wall -Wno-unused-parameter)
endif()

# MinGW cross-builds: statically link the GCC runtimes so the .exe runs on a
# stock Windows install (no libgcc_s_seh-1.dll / libstdc++-6.dll needed).
if(MINGW)
    target_link_options(MicFilterClient PRIVATE -static-libgcc -static-libstdc++)
endif()

target_link_libraries(MicFilterClient PRIVATE
    ole32        # COM / WASAPI
    oleaut32
    avrt         # AvSetMmThreadCharacteristics (Pro Audio task)
    winmm        # timeBeginPeriod not used; kept for multimedia timers if needed
    comctl32
    comdlg32
    user32
    gdi32
    ksuser       # KSDATAFORMAT_SUBTYPE_* GUIDs
    uuid
)
