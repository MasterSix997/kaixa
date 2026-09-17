#include <iostream>

#ifndef GENERATED_HELLO
#error "GENERATED_HELLO must be supplied by Kaixa CMake target configuration"
#endif

#define GENERATED_STRINGIFY_INNER(value) #value
#define GENERATED_STRINGIFY(value) GENERATED_STRINGIFY_INNER(value)

int main() {
    std::cout << "hello from " << GENERATED_STRINGIFY(GENERATED_HELLO_NAME) << '\n';
}
