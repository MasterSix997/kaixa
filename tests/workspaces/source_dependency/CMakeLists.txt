cmake_minimum_required(VERSION 3.20)
project(test_source_app LANGUAGES CXX)

add_executable(test_source_app src/main.cpp)
target_link_libraries(test_source_app PRIVATE test_source_math)
