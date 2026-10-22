cmake_minimum_required(VERSION 3.22)
project(CodyPitch VERSION 0.5.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "")

option(COPY_PLUGIN_AFTER_BUILD "Copy AU and VST3 bundles after building the plug-in target" OFF)

if(MSVC)
    set(TUNA_COMPILER_OPTIONS /W4)
else()
    set(TUNA_COMPILER_OPTIONS -Wall -Wextra)
endif()

include(FetchContent)
set(WORLD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(WORLD_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_Declare(world
    GIT_REPOSITORY https://github.com/mmorise/World.git
    GIT_TAG c794856b537758a3d300757db2e5e31685a822c2)
FetchContent_MakeAvailable(world)

# Prefer a local JUCE checkout for day-to-day development. A fresh clone remains buildable:
# when third_party/JUCE is absent CMake fetches the exact release used by this project.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/JUCE/CMakeLists.txt")
    add_subdirectory(third_party/JUCE)
else()
    FetchContent_Declare(JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG 8.0.9
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(JUCE)
endif()

add_library(cody_pitch_engine STATIC Source/PitchEngine.cpp Source/RealtimeEngine.cpp Source/WorldRenderer.cpp
    Source/EpochTracker.cpp Source/WaveformRenderer.cpp)
target_include_directories(cody_pitch_engine PUBLIC Source)
target_compile_options(cody_pitch_engine PRIVATE ${TUNA_COMPILER_OPTIONS})
target_link_libraries(cody_pitch_engine PUBLIC world::core)

set(TUNA_FORMATS VST3)
set(TUNA_AU_ARGS)
if(APPLE)
    list(PREPEND TUNA_FORMATS AU)
    list(APPEND TUNA_AU_ARGS AU_MAIN_TYPE kAudioUnitType_Effect)
endif()

juce_add_plugin(CodyPitch
    PRODUCT_NAME "Tuna Tuner"
    COMPANY_NAME "codycodie"
    BUNDLE_ID "com.c0dypeng.tunatuner"
    PLUGIN_MANUFACTURER_CODE Cdpg
    PLUGIN_CODE Tuna
    FORMATS ${TUNA_FORMATS}
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    VST3_CATEGORIES Fx PitchShift
    ${TUNA_AU_ARGS}
    COPY_PLUGIN_AFTER_BUILD ${COPY_PLUGIN_AFTER_BUILD})

target_sources(CodyPitch PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp)
juce_add_binary_data(CodyPitchAssets SOURCES
    Resources/fonts/Inter-SemiBold.ttf
    Resources/fonts/JetBrainsMono-Medium.ttf)
target_link_libraries(CodyPitch PRIVATE cody_pitch_engine CodyPitchAssets juce::juce_audio_utils
    PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)
target_compile_definitions(CodyPitch PUBLIC JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_VST3_CAN_REPLACE_VST2=0)

# JUCE writes VST3 moduleinfo.json after linking; sign the finished bundles, not the binary alone.
if(APPLE)
    foreach(format AU VST3)
        add_custom_command(TARGET CodyPitch_${format} POST_BUILD
            COMMAND /usr/bin/codesign --force --deep --sign - "$<TARGET_BUNDLE_DIR:CodyPitch_${format}>"
            VERBATIM)
    endforeach()
endif()

enable_testing()
add_executable(realtime_pacer_test tests/RealtimePacerTest.cpp)
add_test(NAME realtime_pacer_test COMMAND realtime_pacer_test)
add_executable(cody_pitch_test tests/PitchEngineTest.cpp)
target_link_libraries(cody_pitch_test PRIVATE cody_pitch_engine)
add_test(NAME cody_pitch_test COMMAND cody_pitch_test)
add_executable(realtime_engine_test tests/RealtimeEngineTest.cpp)
target_link_libraries(realtime_engine_test PRIVATE cody_pitch_engine)
add_test(NAME realtime_engine_test COMMAND realtime_engine_test)
add_executable(waveform_renderer_test tests/WaveformRendererTest.cpp)
target_link_libraries(waveform_renderer_test PRIVATE cody_pitch_engine)
target_compile_definitions(waveform_renderer_test PRIVATE WAVEFORM_RESULTS_DIR="${CMAKE_BINARY_DIR}")
add_test(NAME waveform_renderer_test COMMAND waveform_renderer_test)
add_executable(voicing_bridge_test tests/VoicingBridgeTest.cpp)
target_link_libraries(voicing_bridge_test PRIVATE cody_pitch_engine)
add_test(NAME voicing_bridge_test COMMAND voicing_bridge_test)
add_executable(latency_test tests/LatencyTest.cpp)
target_link_libraries(latency_test PRIVATE cody_pitch_engine)
add_test(NAME latency_test COMMAND latency_test)
add_test(NAME latency_test_world COMMAND latency_test)
set_tests_properties(latency_test_world PROPERTIES ENVIRONMENT "TUNA_RENDERER=world")
add_executable(ui_string_test tests/UiStringTest.cpp)
target_compile_definitions(ui_string_test PRIVATE SOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}")
add_test(NAME ui_string_test COMMAND ui_string_test)

add_executable(processor_contract_test tests/ProcessorContractTest.cpp)
target_include_directories(processor_contract_test PRIVATE Source)
target_link_libraries(processor_contract_test PRIVATE CodyPitch juce::juce_audio_utils)
add_test(NAME processor_contract_test COMMAND processor_contract_test)

# Editor snapshot tool. This does not create a standalone plugin format.
add_executable(editor_preview tools/EditorPreview.cpp)
target_include_directories(editor_preview PRIVATE Source)
target_link_libraries(editor_preview PRIVATE CodyPitch juce::juce_audio_utils)
