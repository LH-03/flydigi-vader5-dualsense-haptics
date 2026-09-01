cmake_minimum_required(VERSION 3.20)

project(vaderhaptics_launcher LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(VaderHaptics WIN32 main.cpp launcher.rc)
target_compile_definitions(VaderHaptics PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(VaderHaptics PRIVATE comctl32 shell32 dwmapi msimg32)

if(MSVC)
    target_compile_options(VaderHaptics PRIVATE /W4 /WX /permissive- /utf-8)
    target_link_options(VaderHaptics PRIVATE /MANIFEST:NO)
endif()
