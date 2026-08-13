cmake_minimum_required(VERSION 3.20)
project(test_single LANGUAGES CXX)

add_library(test_single_support STATIC src/support.cpp)
add_executable(test_single src/main.cpp)
