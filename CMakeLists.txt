cmake_minimum_required(VERSION 3.16)
project(morton CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "-O2 -g")
set(CMAKE_CXX_FLAGS_RELEASE "-O3")

add_compile_options(-Wall -Wextra -Wno-unused-parameter)

option(MORTON_ASAN "Build with address and undefined sanitizers" OFF)
if(MORTON_ASAN)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()

find_package(Threads REQUIRED)

file(GLOB_RECURSE MORTON_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/src/*.cpp)

add_library(morton_core STATIC ${MORTON_SOURCES})
target_include_directories(morton_core PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(morton_core PUBLIC Threads::Threads)

function(morton_app name)
    add_executable(${name} ${ARGN})
    target_link_libraries(${name} PRIVATE morton_core)
endfunction()

enable_testing()

function(morton_test name)
    add_executable(${name} ${ARGN})
    target_link_libraries(${name} PRIVATE morton_core)
    target_include_directories(${name} PRIVATE ${CMAKE_SOURCE_DIR})
    add_test(NAME ${name} COMMAND ${name})
endfunction()

add_subdirectory(apps)
add_subdirectory(tests)
