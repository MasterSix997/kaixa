#include <test_support.hpp>

#include <kaixa/kaixa.hpp>

#include <string>

KAIXA_TEST(library_reports_its_version) {
    context.check_equal(
        kaixa::version(),
        std::string_view("0.1.0"),
        "library version"
    );
}
