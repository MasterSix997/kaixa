cmake_minimum_required(VERSION 3.20)
project(demo_math LANGUAGES CXX)

add_library(demo_math STATIC src/math.cpp)
target_include_directories(demo_math PUBLIC include)
target_compile_features(demo_math PUBLIC cxx_std_23)
