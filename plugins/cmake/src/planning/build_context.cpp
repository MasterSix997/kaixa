#include "build_context.hpp"

namespace kaixa::plugin::cmake::detail {
    std::filesystem::path product_metadata_directory(const BuildContext& context) {
        return context.directory / ".kaixa" / "products";
    }
}
