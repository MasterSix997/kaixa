#include <kaixa/model/file_set.hpp>

#include <algorithm>
#include <regex>
#include <system_error>

namespace kaixa {
    namespace {
        std::string normalized_pattern(std::string value) {
            std::ranges::replace(value, '\\', '/');
            while (value.starts_with("./"))
                value.erase(0, 2);

            return value;
        }

        std::string regex_pattern(const std::string_view glob) {
            constexpr std::string_view special = R"(\.^$|()[]+{})";
            std::string result = "^";
            for (std::size_t index = 0; index < glob.size(); ++index) {
                const char character = glob[index];
                if (character == '*') {
                    const bool recursive = index + 1 < glob.size() && glob[index + 1] == '*';
                    if (recursive) {
                        ++index;
                        if (index + 1 < glob.size() && glob[index + 1] == '/') {
                            ++index;
                            result += "(?:.*/)?";
                        } else {
                            result += ".*";
                        }
                    } else {
                        result += "[^/]*";
                    }
                } else if (character == '?') {
                    result += "[^/]";
                } else {
                    if (special.contains(character))
                        result.push_back('\\');

                    result.push_back(character);
                }
            }
            result += '$';
            return result;
        }

        std::filesystem::path search_root(const std::filesystem::path& root, const std::string_view pattern) {
            const std::size_t wildcard = pattern.find_first_of("*?");
            const std::size_t separator = pattern.rfind('/', wildcard);
            if (separator == std::string_view::npos)
                return root;

            return root / std::filesystem::path(pattern.substr(0, separator));
        }

        std::string relative_pattern_path(const std::filesystem::path& path, const std::filesystem::path& root) {
            return path.lexically_relative(root).generic_string();
        }

        bool matches_any(const std::string_view path, const std::vector<std::regex>& patterns) {
            return std::ranges::any_of(patterns, [&](const std::regex& pattern) {
                return std::regex_match(path.begin(), path.end(), pattern);
            });
        }

        std::filesystem::path output_path(const std::filesystem::path& path, const std::filesystem::path& relative_to) {
            const std::filesystem::path relative = path.lexically_relative(relative_to);
            return relative.empty() ? path.lexically_normal() : relative.lexically_normal();
        }
    }

    bool is_glob_pattern(const std::string_view value) noexcept {
        return !value.starts_with("$<") && value.find_first_of("*?") != std::string_view::npos;
    }

    Result<std::vector<std::filesystem::path>> expand_file_set(
        const FileSet& files,
        const std::filesystem::path& root,
        const std::filesystem::path& relative_to,
        const bool allow_unmatched
    ) {
        std::vector<std::regex> exclusions;
        exclusions.reserve(files.exclude.size());
        try {
            for (const std::string& pattern: files.exclude)
                exclusions.emplace_back(regex_pattern(normalized_pattern(pattern)));
        } catch (const std::regex_error& failure) {
            return std::unexpected(
                error_at(files.location, "invalid file exclusion pattern: " + std::string(failure.what()))
            );
        }

        std::vector<std::filesystem::path> result;
        for (const std::string& declared: files.include) {
            const std::string pattern = normalized_pattern(declared);
            if (!is_glob_pattern(pattern)) {
                const std::filesystem::path path = std::filesystem::path(pattern).is_absolute()
                                                       ? std::filesystem::path(pattern)
                                                       : root / std::filesystem::path(pattern);
                if (!matches_any(relative_pattern_path(path, root), exclusions))
                    result.push_back(output_path(path, relative_to));

                continue;
            }

            std::regex matcher;
            try {
                matcher = std::regex(regex_pattern(pattern));
            } catch (const std::regex_error& failure) {
                return std::unexpected(
                    error_at(files.location, "invalid file pattern `" + declared + "`: " + failure.what())
                );
            }

            const std::filesystem::path directory = search_root(root, pattern);
            std::error_code failure;
            std::filesystem::recursive_directory_iterator iterator(directory, failure);
            const std::filesystem::recursive_directory_iterator end;
            if (failure) {
                return std::unexpected(
                    error_at(files.location, "cannot inspect file pattern `" + declared + "`: " + failure.message())
                );
            }

            bool matched = false;
            while (iterator != end) {
                const std::filesystem::directory_entry& entry = *iterator;
                const bool regular = entry.is_regular_file(failure);
                if (failure) {
                    return std::unexpected(
                        error_at(files.location, "cannot inspect `" + entry.path().string() + "`: " + failure.message())
                    );
                }

                if (regular) {
                    const std::string relative = relative_pattern_path(entry.path(), root);
                    if (std::regex_match(relative, matcher)) {
                        matched = true;
                        if (!matches_any(relative, exclusions))
                            result.push_back(output_path(entry.path(), relative_to));
                    }
                }
                iterator.increment(failure);
                if (failure) {
                    return std::unexpected(
                        error_at(files.location, "cannot inspect file pattern `" + declared + "`: " + failure.message())
                    );
                }
            }

            if (!matched && !allow_unmatched) {
                return std::unexpected(error_at(files.location, "file pattern `" + declared + "` matched no files"));
            }
        }

        std::ranges::sort(result, {}, [](const std::filesystem::path& path) { return path.generic_string(); });
        result.erase(std::ranges::unique(result).begin(), result.end());
        return result;
    }
}
