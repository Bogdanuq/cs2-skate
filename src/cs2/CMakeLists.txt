# cs2skate.dll for lua/cs2skate.lua (Astral). Build:
#   cmake -S . -B build -A x64
#   cmake --build build --config Release        -> build/Release/cs2skate.dll
cmake_minimum_required(VERSION 3.21)
project(cs2skate LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")  # static CRT
add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
include(FetchContent)
FetchContent_Declare(minhook GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git GIT_TAG master GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(minhook)
add_library(cs2skate_minhook STATIC ${minhook_SOURCE_DIR}/src/buffer.c ${minhook_SOURCE_DIR}/src/hook.c
    ${minhook_SOURCE_DIR}/src/trampoline.c ${minhook_SOURCE_DIR}/src/hde/hde32.c ${minhook_SOURCE_DIR}/src/hde/hde64.c)
target_include_directories(cs2skate_minhook PUBLIC ${minhook_SOURCE_DIR}/include)
if(MSVC)
    add_compile_options($<$<COMPILE_LANGUAGE:CXX>:/W4> /permissive- /EHsc /utf-8)
endif()
add_library(cs2skate SHARED src/cs2skate.cpp)
target_link_libraries(cs2skate PRIVATE cs2skate_minhook d3d11 dxgi d3dcompiler ole32 oleaut32 uuid)
