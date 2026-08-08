cmake_minimum_required(VERSION 3.20)
project(demo_app LANGUAGES CXX)

add_executable(demo_app src/main.cpp)
target_compile_features(demo_app PRIVATE cxx_std_23)
