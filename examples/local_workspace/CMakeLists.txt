cmake_minimum_required(VERSION 3.20)
project(demo_app LANGUAGES CXX)

add_executable(demo_app src/main.cpp)
target_compile_features(demo_app PRIVATE cxx_std_23)
target_link_libraries(demo_app PRIVATE demo_math)

enable_testing()
add_test(NAME demo_app COMMAND demo_app)
