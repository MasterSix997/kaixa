cmake_minimum_required(VERSION 3.20)
project(test_source_math LANGUAGES CXX)

add_library(test_source_math STATIC src/math.cpp)
target_include_directories(test_source_math PUBLIC include)
