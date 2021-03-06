cmake_minimum_required(VERSION 3.13)
project(MetaLibClangVer)
set(CMAKE_CXX_STANDARD 17)
set(INCLUDE_DIR ${CMAKE_SOURCE_DIR}/include)
set(LIB_DIR ${CMAKE_SOURCE_DIR}/libs)
set(SRC_DIR ${CMAKE_SOURCE_DIR}/src)

SET(CMAKE_FIND_PACKAGE_SORT_ORDER NATURAL)
SET(CMAKE_FIND_PACKAGE_SORT_DIRECTION DEC)

include_directories("${INCLUDE_DIR}")
include_directories("${CMAKE_SOURCE_DIR}/third_party/library-metamorphic-testing/include")
link_directories("${LIB_DIR}")

find_package(Clang REQUIRED)
find_package(LLVM REQUIRED CONFIG)

include_directories(${LLVM_INCLUDE_DIRS})
include_directories(${CLANG_INCLUDE_DIRS})
add_definitions(${LLVM_DEFINITIONS})

add_executable(mtFuzzer
    ${SRC_DIR}/fuzzMetaTest.cpp
    ${SRC_DIR}/srcHelperFunctions.cpp
    ${SRC_DIR}/helperFuncStitch.cpp
    ${SRC_DIR}/parseFuzzerCalls.cpp
    ${SRC_DIR}/parseFuzzSpec.cpp
    ${SRC_DIR}/generateMetaTests.cpp
    ${SRC_DIR}/libSpecReader.cpp
    ${SRC_DIR}/metaSpecReader.cpp
    )

if (NOT LLVM_ENABLE_RTTI)
    message(STATUS "LLVM compiled with RTTI disabled...")
    target_compile_options(mtFuzzer PUBLIC -fno-rtti)
endif()

#target_compile_options(mtFuzzer PUBLIC -stdlib=libc++)
#target_link_options(mtFuzzer PUBLIC -stdlib=libc++ -lc++abi)
if (METALIB_ASAN)
    target_compile_options(mtFuzzer PUBLIC -fsanitize=address)
    target_link_options(mtFuzzer PUBLIC -fsanitize=address -fno-omit-frame-pointer)
endif()

target_link_libraries(mtFuzzer
    LLVM
    clangAST
    clangASTMatchers
    clangBasic
    clangDriver
    clangFrontend
    clangRewriteFrontend
    clangSerialization
    clangStaticAnalyzerFrontend
    clangTooling
    metalib_fuzz
    )
