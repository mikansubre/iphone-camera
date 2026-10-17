cmake_minimum_required(VERSION 3.24)
project(iphonecam LANGUAGES CXX)

include(FetchContent)
set(WIL_BUILD_PACKAGING OFF CACHE BOOL "" FORCE)
set(WIL_BUILD_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(wil
    GIT_REPOSITORY https://github.com/microsoft/wil.git
    GIT_TAG v1.0.260126.7
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(wil)

add_library(iphonecam SHARED
    src/Activator.cpp
    src/Api.cpp
    src/Config.cpp
    src/dllmain.cpp
    src/FrameChannel.cpp
    src/Log.cpp
    src/MediaSource.cpp
    src/MediaStream.cpp
    src/iphonecam.def)

target_compile_features(iphonecam PRIVATE cxx_std_20)
target_compile_definitions(iphonecam PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
# C4324: WRL 内部の構造体パディング警告
target_compile_options(iphonecam PRIVATE /W4 /permissive- /utf-8 /EHsc /wd4324)
target_precompile_headers(iphonecam PRIVATE src/pch.h)
target_link_libraries(iphonecam PRIVATE WIL::WIL mfplat mfuuid mfsensorgroup ole32 cfgmgr32 advapi32 propsys runtimeobject)
# Frame Server に VC++ ランタイムの有無を気にさせないよう CRT は静的リンク
set_property(TARGET iphonecam PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
