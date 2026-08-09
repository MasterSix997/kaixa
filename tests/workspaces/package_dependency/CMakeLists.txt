cmake_minimum_required(VERSION 3.20)
project(test_package_app LANGUAGES CXX)

find_package(test_package_math CONFIG REQUIRED)

add_executable(test_package_app src/main.cpp)
target_link_libraries(test_package_app PRIVATE test_package::math)
