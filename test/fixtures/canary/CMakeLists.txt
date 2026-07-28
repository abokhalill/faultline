cmake_minimum_required(VERSION 3.20)
project(canary_fixture LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_executable(canary src/canary.cpp)
find_package(Threads REQUIRED)
target_link_libraries(canary PRIVATE Threads::Threads)
