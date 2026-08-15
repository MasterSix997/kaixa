#pragma once

#include <kaixa/foundation/diagnostic.hpp>
#include <kaixa/model/manifest.hpp>

#include <string>
#include <vector>

namespace kaixa {
    struct ProviderInfo {
        std::string name;
        std::string driver;
        bool is_default = false;
    };

    struct PackageCandidate {
        std::string package;
        Version version;
        std::string authority;
        SourceLocator source;
    };

    class PackageProvider {
    public:
        virtual ~PackageProvider() = default;

        [[nodiscard]] virtual ProviderInfo info() const = 0;
        [[nodiscard]] virtual Result<std::vector<PackageCandidate>> candidates(const PackageRequest& request) const = 0;
    };
}
