cmake_minimum_required(VERSION 3.20)
project(MicFilterClient VERSION 0.18.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Since v0.7 the client embeds the DSP chain (ChainV2) directly instead of
# hosting the .vst3 file: one self-contained .exe, identical sound (the
# parameter mapping is shared with the plugin via Source/ParamBridge.h).
# No VST3 SDK is needed to build the client anymore.

add_executable(MicFilterClient WIN32
    src/log.cpp
    src/log.h
    src/main.cpp
    src/mic_level.cpp
    src/mic_level.h
    src/passive_calibration.cpp
    src/passive_calibration.h
    src/passive_gain.h
    src/ui_shared.h
    src/updater.cpp
    src/updater.h
    src/update_check.h
    src/preset.h
    src/preset_json.cpp
    src/preset_store.cpp
    src/settings_window.cpp
    src/settings_window.h
    src/wasapi_engine.cpp
    src/wasapi_engine.h
)

# ClientConfig.h and the DSP headers live in ../Source; paramids.h is
# shared with the plugin under ../vst3/source.
target_include_directories(MicFilterClient PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/.."
)

# --- RNNoise (AI noise suppression stage of the embedded chain) -------------
set(RNNOISE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../thirdparty/rnnoise)
target_sources(MicFilterClient PRIVATE
    ${RNNOISE_DIR}/src/denoise.c ${RNNOISE_DIR}/src/rnn.c ${RNNOISE_DIR}/src/pitch.c
    ${RNNOISE_DIR}/src/kiss_fft.c ${RNNOISE_DIR}/src/celt_lpc.c ${RNNOISE_DIR}/src/nnet.c
    ${RNNOISE_DIR}/src/nnet_default.c ${RNNOISE_DIR}/src/parse_lpcnet_weights.c
    ${RNNOISE_DIR}/src/rnnoise_data.c ${RNNOISE_DIR}/src/rnnoise_tables.c
)
target_include_directories(MicFilterClient PRIVATE
    ${RNNOISE_DIR}/include ${RNNOISE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../thirdparty/rnnoise-build)

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
    winmm
    comctl32
    comdlg32
    user32
    gdi32
    ksuser       # KSDATAFORMAT_SUBTYPE_* GUIDs
    uuid
    winhttp      # self-update check / download
    bcrypt       # SHA-256 verification of the update package
)
