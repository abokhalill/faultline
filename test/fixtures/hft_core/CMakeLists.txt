cmake_minimum_required(VERSION 3.20)
project(hft_core_fixture LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_executable(hft_core
    src/main.cpp
    src/order_book.cpp
    src/matching_engine.cpp
    src/feed_handler.cpp
)
target_include_directories(hft_core PRIVATE include)
