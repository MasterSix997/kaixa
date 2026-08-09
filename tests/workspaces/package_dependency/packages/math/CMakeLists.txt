cmake_minimum_required(VERSION 3.20)
project(test_package_math LANGUAGES CXX)

add_library(test_package_math STATIC src/math.cpp)
target_include_directories(test_package_math PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
set_target_properties(test_package_math PROPERTIES EXPORT_NAME math)

install(TARGETS test_package_math
    EXPORT test_package_math_targets
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin
)
install(DIRECTORY include/ DESTINATION include)
install(EXPORT test_package_math_targets
    FILE test_package_mathConfig.cmake
    NAMESPACE test_package::
    DESTINATION lib/cmake/test_package_math
)
