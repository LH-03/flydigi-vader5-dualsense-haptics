cmake_minimum_required(VERSION 3.20)

project(VaderHaptics VERSION 0.1.0 LANGUAGES CXX)

include(CTest)

add_subdirectory(tools/launcher)
add_subdirectory(tools/m1/vader_ds5_bridge)

if(BUILD_TESTING)
    add_test(NAME flydigi_onboard_profile_tests
             COMMAND flydigi_onboard_profile_tests)
    add_test(NAME bridge_input_mapping_selftest
             COMMAND vader_ds5_bridge input-selftest)
endif()
