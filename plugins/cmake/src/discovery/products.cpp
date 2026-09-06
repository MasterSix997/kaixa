#include "products.hpp"

#include <kaixa/config/parser.hpp>
#include <kaixa/foundation/filesystem.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace kaixa::plugin::cmake::detail {
    namespace {
        Result<ProductKind> product_kind(const std::string_view type) {
            if (type == "EXECUTABLE")
                return ProductKind::executable;

            if (type == "STATIC_LIBRARY")
                return ProductKind::static_library;

            if (type == "SHARED_LIBRARY")
                return ProductKind::shared_library;

            if (type == "MODULE_LIBRARY")
                return ProductKind::module_library;

            if (type == "OBJECT_LIBRARY")
                return ProductKind::object_library;

            if (type == "INTERFACE_LIBRARY")
                return ProductKind::interface_library;

            if (type == "UTILITY")
                return ProductKind::utility;

            return std::unexpected(error("unsupported CMake target type `" + std::string(type) + "`"));
        }

        ProductPurpose product_purpose(const PackageNode& package, const std::string_view name) {
            if (!package.manifest())
                return ProductPurpose::primary;

            const auto target = std::ranges::find_if(package.targets, [&](const PackageTarget& candidate) {
                return candidate.name == name;
            });
            if (target == package.targets.end())
                return ProductPurpose::primary;

            switch (target->kind) {
            case PackageTargetKind::test: return ProductPurpose::test;
            case PackageTargetKind::example: return ProductPurpose::example;
            case PackageTargetKind::benchmark: return ProductPurpose::benchmark;
            }
            return ProductPurpose::primary;
        }

    }

    Result<std::vector<BuildProduct>> read_products(const PackageNode& package, const BuildContext& context) {
        const std::filesystem::path directory = product_metadata_directory(context) / context.configuration;
        std::error_code failure;
        const bool exists = std::filesystem::exists(directory, failure);
        if (failure) {
            return std::unexpected(error("cannot inspect CMake products in `" + directory.string() + "`: " + failure.message()));
        }
        if (!exists) {
            return std::unexpected(
                error("CMake target information is unavailable").add_note("run `kaixa generate` to configure the workspace")
            );
        }
        if (!std::filesystem::is_directory(directory, failure) || failure) {
            return std::unexpected(error("CMake product metadata is not a directory: " + directory.string()));
        }

        std::vector<BuildProduct> products;
        std::filesystem::directory_iterator entries(directory, failure);
        if (failure) {
            return std::unexpected(error("cannot list CMake products in `" + directory.string() + "`: " + failure.message()));
        }

        for (const std::filesystem::directory_entry& entry: entries) {
            if (entry.path().extension() != ".product")
                continue;

            std::ifstream input(entry.path(), std::ios::binary);
            std::string name;
            std::string type;
            std::string artifact;
            if (!input || !std::getline(input, name) || !std::getline(input, type) || !std::getline(input, artifact)) {
                return std::unexpected(error("cannot read CMake product metadata `" + entry.path().string() + "`"));
            }
            if (!name.empty() && name.back() == '\r')
                name.pop_back();

            if (!type.empty() && type.back() == '\r')
                type.pop_back();

            if (!artifact.empty() && artifact.back() == '\r')
                artifact.pop_back();

            if (name.empty() || type.empty()) {
                return std::unexpected(error("invalid CMake product metadata `" + entry.path().string() + "`"));
            }

            auto kind = product_kind(type);
            if (!kind)
                return std::unexpected(std::move(kind).error().add_note("in `" + entry.path().string() + "`"));

            const ProductPurpose purpose = product_purpose(package, name);
            BuildProduct product{std::move(name), *kind, purpose, package.id, std::nullopt};
            if (!artifact.empty())
                product.artifact = std::move(artifact);

            products.push_back(std::move(product));
        }

        std::ranges::sort(products, {}, &BuildProduct::name);
        return products;
    }
}
