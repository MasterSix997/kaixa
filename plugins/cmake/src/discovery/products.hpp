#pragma once

#include <planning/build_context.hpp>

#include <kaixa/build/product.hpp>
#include <kaixa/foundation/diagnostic.hpp>
#include <kaixa/model/graph.hpp>

#include <vector>

namespace kaixa::plugin::cmake::detail {
    // Reading back what CMake actually produced for one configured build tree. Discovery reads
    // the metadata the generated project wrote; it recomputes no variant and plans no action.
    [[nodiscard]] Result<std::vector<BuildProduct>> read_products(const PackageNode& package, const BuildContext& context);
}
