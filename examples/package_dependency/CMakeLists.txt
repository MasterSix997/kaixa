cmake_minimum_required(VERSION 3.20)
project(package_app LANGUAGES CXX)

find_package(package_math CONFIG REQUIRED)

add_executable(package_app src/main.cpp)
target_compile_features(package_app PRIVATE cxx_std_23)
target_link_libraries(package_app PRIVATE package::math)
