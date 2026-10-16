cmake_minimum_required(VERSION 3.20)

project(vaderhaptics_launcher LANGUAGES CXX RC)
include(CTest)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(VaderHaptics WIN32 main.cpp settings.cpp diagnostics.cpp launcher.rc)
target_compile_definitions(VaderHaptics PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(VaderHaptics PRIVATE comctl32 shell32 dwmapi msimg32 ole32 uuid)

if(MSVC)
    target_compile_options(VaderHaptics PRIVATE /W4 /WX /permissive- /utf-8)
    target_link_options(VaderHaptics PRIVATE /MANIFEST:NO)
endif()

if(BUILD_TESTING)
    add_executable(launcher_settings_tests ../../tests/launcher_settings_tests.cpp settings.cpp diagnostics.cpp)
    target_include_directories(launcher_settings_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_definitions(launcher_settings_tests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    if(MSVC)
        target_compile_options(launcher_settings_tests PRIVATE /W4 /WX /permissive- /utf-8)
    endif()
    add_test(NAME launcher_settings_tests COMMAND launcher_settings_tests)
endif()
