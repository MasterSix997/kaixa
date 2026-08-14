#pragma once

#include <kaixa/model/package.hpp>

#include <filesystem>
#include <optional>
#include <string>

namespace kaixa {
    enum class ProductKind {
        executable,
        static_library,
        shared_library,
        module_library,
        object_library,
        interface_library,
        utility
    };

    enum class ProductPurpose {
        primary,
        test,
        example,
        benchmark
    };

    struct BuildProduct {
        std::string name;
        ProductKind kind = ProductKind::utility;
        ProductPurpose purpose = ProductPurpose::primary;
        PackageId package;
        std::optional<std::filesystem::path> artifact;
    };
}
