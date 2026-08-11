#include <kaixa/kaixa.hpp>

namespace kaixa {
    inline constexpr std::string_view KAIXA_VERSION = "0.2.0-dev";

    std::string_view version() noexcept {
        return KAIXA_VERSION;
    }
}
