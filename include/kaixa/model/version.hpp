#pragma once

#include <kaixa/foundation/diagnostic.hpp>

#include <string>
#include <string_view>

namespace kaixa {
    struct Version {
        std::string text;

        [[nodiscard]] bool operator==(const Version&) const = default;
    };

    struct VersionRequirement {
        std::string text;

        [[nodiscard]] bool operator==(const VersionRequirement&) const = default;
    };

    [[nodiscard]] Result<Version> parse_version(std::string_view text, const SourceLocation& location = {});
    [[nodiscard]] Result<VersionRequirement>
    parse_version_requirement(std::string_view text, const SourceLocation& location = {});
    [[nodiscard]] int compare_versions(const Version& left, const Version& right);
    [[nodiscard]] bool matches(const VersionRequirement& requirement, const Version& version);
}
