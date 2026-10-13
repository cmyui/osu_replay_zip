cmake_minimum_required(VERSION 3.20)
project(osu_replay_zip LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(lzma457 STATIC
    Lib/C/7zCrc.c
    Lib/C/Alloc.c
    Lib/C/Compress/Lz/MatchFinder.c
    Lib/CPP/7zip/Common/InBuffer.cpp
    Lib/CPP/7zip/Common/OutBuffer.cpp
    Lib/CPP/7zip/Common/StreamUtils.cpp
    Lib/CPP/7zip/Compress/LZ/LZOutWindow.cpp
    Lib/CPP/7zip/Compress/LZMA/LZMADecoder.cpp
    Lib/CPP/7zip/Compress/LZMA/LZMAEncoder.cpp
    Lib/CPP/7zip/Compress/RangeCoder/RangeCoderBit.cpp
    lzma.cpp
    lzma_guids.cpp
)
target_include_directories(lzma457 PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(lzma457 PRIVATE NOMINMAX _CRT_SECURE_NO_WARNINGS)

option(ORZ_BUILD_REPLAY_TOOLS "Build the x86 replay CLI and integration tests" ON)
if(ORZ_BUILD_REPLAY_TOOLS)
    add_executable(orz Source.cpp)
    target_link_libraries(orz PRIVATE lzma457)
    if(MSVC)
        target_compile_options(orz PRIVATE /fp:precise)
    else()
        target_compile_options(orz PRIVATE -msse4.2 -ffp-contract=off)
        if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
            target_compile_options(orz PRIVATE -fms-extensions)
        endif()
    endif()
endif()

include(CTest)
if(BUILD_TESTING)
    add_executable(lzma_tests tests/lzma_tests.cpp)
    target_link_libraries(lzma_tests PRIVATE lzma457)
    add_test(NAME lzma_tests COMMAND lzma_tests ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures)
    if(ORZ_BUILD_REPLAY_TOOLS)
        add_executable(replay_tests tests/replay_tests.cpp)
        target_link_libraries(replay_tests PRIVATE lzma457)
        if(MSVC)
            target_compile_options(replay_tests PRIVATE /fp:precise)
        else()
            target_compile_options(replay_tests PRIVATE -msse4.2 -ffp-contract=off)
            if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
                target_compile_options(replay_tests PRIVATE -fms-extensions)
            endif()
        endif()
        add_test(NAME replay_tests COMMAND replay_tests ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures ${CMAKE_CURRENT_BINARY_DIR}/test-output)
    endif()
endif()
