cmake_minimum_required(VERSION 3.20)
project(package_math LANGUAGES CXX)

add_library(package_math STATIC src/math.cpp)
target_include_directories(package_math PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_compile_features(package_math PUBLIC cxx_std_23)
set_target_properties(package_math PROPERTIES EXPORT_NAME math)

install(TARGETS package_math
    EXPORT package_math_targets
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin
)
install(DIRECTORY include/ DESTINATION include)
install(EXPORT package_math_targets
    FILE package_mathConfig.cmake
    NAMESPACE package::
    DESTINATION lib/cmake/package_math
)
