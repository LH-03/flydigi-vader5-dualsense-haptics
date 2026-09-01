cmake_minimum_required(VERSION 3.20)

project(vaderhaptics_m1_vader_ds5_bridge LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(vader_ds5_bridge main.cpp ../../m0/vaderhid.cpp
               ../../m2/vpcm_parser.cpp ../../m2/capture_analysis.cpp
               ../../m3/haptics.cpp
               ../../../src/runtime/mode_contract.cpp
               ../../../src/runtime/session_state.cpp
               ../../../src/profiles/runtime_profile.cpp
               ../../../src/devices/haptic_device_profile.cpp
               ../../../src/input/flydigi_onboard_profile.cpp)
target_include_directories(vader_ds5_bridge PRIVATE ../.. ../../../src)

if(MSVC)
    target_compile_options(vader_ds5_bridge PRIVATE
        /Zc:__cplusplus /W4 /WX /permissive- /utf-8
        /wd4189 /wd4267 /wd4456 /wd4996)
endif()

target_link_libraries(vader_ds5_bridge ws2_32 xinput9_1_0 cfgmgr32 setupapi)

add_executable(flydigi_onboard_profile_tests
               ../../../tests/flydigi_onboard_profile_tests.cpp
               ../../../src/input/flydigi_onboard_profile.cpp)
target_include_directories(flydigi_onboard_profile_tests PRIVATE ../../../src)

if(MSVC)
    target_compile_options(flydigi_onboard_profile_tests PRIVATE
        /Zc:__cplusplus /W4 /WX /permissive- /utf-8
        /wd4189 /wd4267 /wd4456 /wd4996)
endif()
