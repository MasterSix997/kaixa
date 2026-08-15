#include <kaixa/model/version.hpp>

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace kaixa {
    namespace {
        struct PrereleaseIdentifier {
            std::string text;
            std::optional<std::uint64_t> number;
        };

        struct SemanticVersion {
            std::uint64_t major = 0;
            std::uint64_t minor = 0;
            std::uint64_t patch = 0;
            std::vector<PrereleaseIdentifier> prerelease;
        };

        enum class RequirementKind {
            any,
            equal,
            greater,
            greater_equal,
            less,
            less_equal,
            compatible,
            patch_compatible,
            wildcard
        };

        struct RequirementTerm {
            RequirementKind kind = RequirementKind::compatible;
            std::uint64_t major = 0;
            std::optional<std::uint64_t> minor;
            std::optional<std::uint64_t> patch;
            std::vector<PrereleaseIdentifier> prerelease;
        };

        std::string_view trim(std::string_view text) {
            while (!text.empty() && (text.front() == ' ' || text.front() == '\t'))
                text.remove_prefix(1);

            while (!text.empty() && (text.back() == ' ' || text.back() == '\t'))
                text.remove_suffix(1);

            return text;
        }

        bool valid_identifier_character(const char character) {
            return (character >= 'a' && character <= 'z')
                || (character >= 'A' && character <= 'Z')
                || (character >= '0' && character <= '9')
                || character == '-';
        }

        Result<std::uint64_t> parse_number(
            const std::string_view text,
            const SourceLocation& location,
            const bool reject_leading_zero = true
        ) {
            if (text.empty())
                return std::unexpected(error_at(location, "version number cannot be empty"));

            if (reject_leading_zero && text.size() > 1 && text.front() == '0') {
                return std::unexpected(error_at(
                    location,
                    "version number `" + std::string(text) + "` cannot contain a leading zero"
                ));
            }

            std::uint64_t result = 0;
            const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
            if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
                return std::unexpected(error_at(location, "invalid version number `" + std::string(text) + "`"));

            return result;
        }

        Result<std::vector<PrereleaseIdentifier>> parse_identifiers(
            const std::string_view text,
            const SourceLocation& location,
            const bool prerelease
        ) {
            std::vector<PrereleaseIdentifier> result;
            std::size_t begin = 0;
            while (begin <= text.size()) {
                const std::size_t end = text.find('.', begin);
                const std::string_view identifier = text.substr(
                    begin,
                    end == std::string_view::npos ? text.size() - begin : end - begin
                );
                if (identifier.empty())
                    return std::unexpected(error_at(location, "version identifiers cannot be empty"));

                if (!std::ranges::all_of(identifier, valid_identifier_character)) {
                    return std::unexpected(error_at(
                        location,
                        "invalid version identifier `" + std::string(identifier) + "`"
                    ));
                }

                PrereleaseIdentifier parsed{std::string(identifier), std::nullopt};
                const bool numeric = std::ranges::all_of(identifier, [](const char character) {
                    return character >= '0' && character <= '9';
                });
                if (numeric) {
                    auto number = parse_number(identifier, location, prerelease);
                    if (!number)
                        return std::unexpected(number.error());

                    parsed.number = *number;
                }
                result.push_back(std::move(parsed));

                if (end == std::string_view::npos)
                    break;

                begin = end + 1;
            }
            return result;
        }

        Result<SemanticVersion> parse_semantic_version(const std::string_view text, const SourceLocation& location) {
            if (text.empty())
                return std::unexpected(error_at(location, "version cannot be empty"));

            const std::size_t build_separator = text.find('+');
            if (build_separator != std::string_view::npos) {
                if (text.find('+', build_separator + 1) != std::string_view::npos)
                    return std::unexpected(error_at(location, "version contains more than one build separator"));

                auto build = parse_identifiers(text.substr(build_separator + 1), location, false);
                if (!build)
                    return std::unexpected(build.error());
            }

            const std::string_view without_build = text.substr(0, build_separator);
            const std::size_t prerelease_separator = without_build.find('-');
            const std::string_view core = without_build.substr(0, prerelease_separator);
            const std::string_view prerelease = prerelease_separator == std::string_view::npos
                ? std::string_view{}
                : without_build.substr(prerelease_separator + 1);

            const std::size_t first_dot = core.find('.');
            const std::size_t second_dot = first_dot == std::string_view::npos
                ? std::string_view::npos
                : core.find('.', first_dot + 1);
            if (first_dot == std::string_view::npos || second_dot == std::string_view::npos
                || core.find('.', second_dot + 1) != std::string_view::npos) {
                return std::unexpected(error_at(location, "versions must use `major.minor.patch`"));
            }

            auto major = parse_number(core.substr(0, first_dot), location);
            auto minor = parse_number(core.substr(first_dot + 1, second_dot - first_dot - 1), location);
            auto patch = parse_number(core.substr(second_dot + 1), location);
            if (!major)
                return std::unexpected(major.error());
            if (!minor)
                return std::unexpected(minor.error());
            if (!patch)
                return std::unexpected(patch.error());

            SemanticVersion result{*major, *minor, *patch, {}};
            if (!prerelease.empty()) {
                auto identifiers = parse_identifiers(prerelease, location, true);
                if (!identifiers)
                    return std::unexpected(identifiers.error());

                result.prerelease = std::move(*identifiers);
            } else if (prerelease_separator != std::string_view::npos) {
                return std::unexpected(error_at(location, "prerelease cannot be empty"));
            }
            return result;
        }

        int compare_identifiers(
            const std::vector<PrereleaseIdentifier>& left,
            const std::vector<PrereleaseIdentifier>& right
        ) {
            if (left.empty() || right.empty()) {
                if (left.empty() == right.empty())
                    return 0;

                return left.empty() ? 1 : -1;
            }

            const std::size_t count = std::min(left.size(), right.size());
            for (std::size_t index = 0; index < count; ++index) {
                const PrereleaseIdentifier& a = left[index];
                const PrereleaseIdentifier& b = right[index];
                if (a.number && b.number) {
                    if (*a.number != *b.number)
                        return *a.number < *b.number ? -1 : 1;
                } else if (a.number || b.number) {
                    return a.number ? -1 : 1;
                } else if (a.text != b.text) {
                    return a.text < b.text ? -1 : 1;
                }
            }
            if (left.size() == right.size())
                return 0;

            return left.size() < right.size() ? -1 : 1;
        }

        int compare(const SemanticVersion& left, const SemanticVersion& right) {
            if (left.major != right.major)
                return left.major < right.major ? -1 : 1;
            if (left.minor != right.minor)
                return left.minor < right.minor ? -1 : 1;
            if (left.patch != right.patch)
                return left.patch < right.patch ? -1 : 1;

            return compare_identifiers(left.prerelease, right.prerelease);
        }

        bool wildcard_component(const std::string_view text) {
            return text == "*" || text == "x" || text == "X";
        }

        Result<RequirementTerm> parse_requirement_term(std::string_view text, const SourceLocation& location) {
            text = trim(text);
            if (text.empty())
                return std::unexpected(error_at(location, "version requirement contains an empty comparator"));

            RequirementTerm result;
            if (text.starts_with(">=")) {
                result.kind = RequirementKind::greater_equal;
                text.remove_prefix(2);
            } else if (text.starts_with("<=")) {
                result.kind = RequirementKind::less_equal;
                text.remove_prefix(2);
            } else if (text.starts_with('>')) {
                result.kind = RequirementKind::greater;
                text.remove_prefix(1);
            } else if (text.starts_with('<')) {
                result.kind = RequirementKind::less;
                text.remove_prefix(1);
            } else if (text.starts_with('=')) {
                result.kind = RequirementKind::equal;
                text.remove_prefix(1);
            } else if (text.starts_with('~')) {
                result.kind = RequirementKind::patch_compatible;
                text.remove_prefix(1);
            } else if (text.starts_with('^')) {
                result.kind = RequirementKind::compatible;
                text.remove_prefix(1);
            }
            text = trim(text);

            if (wildcard_component(text)) {
                result.kind = RequirementKind::any;
                return result;
            }

            const std::size_t build_separator = text.find('+');
            if (build_separator != std::string_view::npos) {
                auto build = parse_identifiers(text.substr(build_separator + 1), location, false);
                if (!build)
                    return std::unexpected(build.error());

                text = text.substr(0, build_separator);
            }

            const std::size_t prerelease_separator = text.find('-');
            const std::string_view core = text.substr(0, prerelease_separator);
            const std::string_view prerelease = prerelease_separator == std::string_view::npos
                ? std::string_view{}
                : text.substr(prerelease_separator + 1);

            std::vector<std::string_view> components;
            std::size_t begin = 0;
            while (begin <= core.size()) {
                const std::size_t end = core.find('.', begin);
                components.push_back(core.substr(
                    begin,
                    end == std::string_view::npos ? core.size() - begin : end - begin
                ));
                if (end == std::string_view::npos)
                    break;

                begin = end + 1;
            }
            if (components.empty() || components.size() > 3)
                return std::unexpected(error_at(location, "invalid version requirement `" + std::string(text) + "`"));

            bool wildcard = false;
            for (std::size_t index = 0; index < components.size(); ++index) {
                if (wildcard_component(components[index])) {
                    wildcard = true;
                    for (std::size_t tail = index + 1; tail < components.size(); ++tail) {
                        if (!wildcard_component(components[tail])) {
                            return std::unexpected(error_at(
                                location,
                                "wildcards must be trailing version components"
                            ));
                        }
                    }
                    components.resize(index);
                    break;
                }
            }

            if (components.empty()) {
                result.kind = RequirementKind::any;
                return result;
            }

            auto major = parse_number(components[0], location);
            if (!major)
                return std::unexpected(major.error());

            result.major = *major;
            if (components.size() > 1) {
                auto minor = parse_number(components[1], location);
                if (!minor)
                    return std::unexpected(minor.error());

                result.minor = *minor;
            }
            if (components.size() > 2) {
                auto patch = parse_number(components[2], location);
                if (!patch)
                    return std::unexpected(patch.error());

                result.patch = *patch;
            }

            if (wildcard)
                result.kind = RequirementKind::wildcard;

            if (prerelease_separator != std::string_view::npos) {
                if (!result.minor || !result.patch || prerelease.empty()) {
                    return std::unexpected(error_at(
                        location,
                        "prerelease requirements require a complete version"
                    ));
                }
                auto identifiers = parse_identifiers(prerelease, location, true);
                if (!identifiers)
                    return std::unexpected(identifiers.error());

                result.prerelease = std::move(*identifiers);
            }
            return result;
        }

        Result<std::vector<RequirementTerm>> parse_requirement(const std::string_view text, const SourceLocation& location) {
            if (trim(text).empty())
                return std::unexpected(error_at(location, "version requirement cannot be empty"));
            if (text.contains("||"))
                return std::unexpected(error_at(location, "OR version requirements are not supported"));

            std::vector<RequirementTerm> result;
            std::size_t begin = 0;
            while (begin <= text.size()) {
                const std::size_t end = text.find(',', begin);
                auto term = parse_requirement_term(
                    text.substr(begin, end == std::string_view::npos ? text.size() - begin : end - begin),
                    location
                );
                if (!term)
                    return std::unexpected(term.error());

                result.push_back(std::move(*term));
                if (end == std::string_view::npos)
                    break;

                begin = end + 1;
            }
            return result;
        }

        SemanticVersion lower_bound(const RequirementTerm& term) {
            return {term.major, term.minor.value_or(0), term.patch.value_or(0), term.prerelease};
        }

        SemanticVersion compatible_upper(const RequirementTerm& term) {
            if (!term.minor)
                return {term.major + 1, 0, 0, {}};
            if (term.major > 0)
                return {term.major + 1, 0, 0, {}};
            if (!term.patch || *term.minor > 0)
                return {0, *term.minor + 1, 0, {}};

            return {0, 0, *term.patch + 1, {}};
        }

        SemanticVersion patch_compatible_upper(const RequirementTerm& term) {
            if (!term.minor)
                return {term.major + 1, 0, 0, {}};

            return {term.major, *term.minor + 1, 0, {}};
        }

        bool matches_term(const RequirementTerm& term, const SemanticVersion& version) {
            if (term.kind == RequirementKind::any)
                return true;

            const SemanticVersion lower = lower_bound(term);
            const int relation = compare(version, lower);
            switch (term.kind) {
                case RequirementKind::any: return true;
                case RequirementKind::equal:
                    if (!term.minor)
                        return version.major == term.major;
                    if (!term.patch)
                        return version.major == term.major && version.minor == *term.minor;
                    return relation == 0;
                case RequirementKind::greater: return relation > 0;
                case RequirementKind::greater_equal: return relation >= 0;
                case RequirementKind::less: return relation < 0;
                case RequirementKind::less_equal: return relation <= 0;
                case RequirementKind::compatible:
                    return relation >= 0 && compare(version, compatible_upper(term)) < 0;
                case RequirementKind::patch_compatible:
                    return relation >= 0 && compare(version, patch_compatible_upper(term)) < 0;
                case RequirementKind::wildcard:
                    if (!term.minor)
                        return version.major == term.major;
                    if (!term.patch)
                        return version.major == term.major && version.minor == *term.minor;
                    return relation == 0;
            }
            return false;
        }
    }

    Result<Version> parse_version(const std::string_view text, SourceLocation location) {
        auto parsed = parse_semantic_version(text, location);
        if (!parsed)
            return std::unexpected(parsed.error());

        return Version{std::string(text)};
    }

    Result<VersionRequirement> parse_version_requirement(const std::string_view text, SourceLocation location) {
        auto parsed = parse_requirement(text, location);
        if (!parsed)
            return std::unexpected(parsed.error());

        return VersionRequirement{std::string(text)};
    }

    int compare_versions(const Version& left, const Version& right) {
        auto parsed_left = parse_semantic_version(left.text, {});
        auto parsed_right = parse_semantic_version(right.text, {});
        if (!parsed_left || !parsed_right)
            return left.text.compare(right.text);

        return compare(*parsed_left, *parsed_right);
    }

    bool matches(const VersionRequirement& requirement, const Version& version) {
        auto parsed_requirement = parse_requirement(requirement.text, {});
        auto parsed_version = parse_semantic_version(version.text, {});
        if (!parsed_requirement || !parsed_version)
            return false;

        const bool prerelease_allowed = parsed_version->prerelease.empty()
            || std::ranges::any_of(*parsed_requirement, [&](const RequirementTerm& term) {
                return !term.prerelease.empty()
                    && term.major == parsed_version->major
                    && term.minor == parsed_version->minor
                    && term.patch == parsed_version->patch;
            });
        return prerelease_allowed && std::ranges::all_of(*parsed_requirement, [&](const RequirementTerm& term) {
            return matches_term(term, *parsed_version);
        });
    }
}
