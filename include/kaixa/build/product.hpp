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

    struct BuildProduct {
        std::string name;
        ProductKind kind = ProductKind::utility;
        PackageId package;
        std::optional<std::filesystem::path> artifact;
    };
}
