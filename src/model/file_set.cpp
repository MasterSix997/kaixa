#include <kaixa/model/file_set.hpp>

#include <algorithm>
#include <map>
#include <optional>
#include <system_error>
#include <utility>

namespace kaixa {
    namespace {
        std::string normalized_pattern(std::string value) {
            std::ranges::replace(value, '\\', '/');
            while (value.starts_with("./"))
                value.erase(0, 2);

            return value;
        }

        std::vector<std::string> path_components(const std::string_view path) {
            std::vector<std::string> result;
            std::size_t begin = 0;
            while (begin < path.size()) {
                const std::size_t end = path.find('/', begin);
                const std::string_view component = path.substr(begin, end == std::string_view::npos ? path.size() - begin : end - begin);
                if (!component.empty())
                    result.emplace_back(component);

                if (end == std::string_view::npos)
                    break;

                begin = end + 1;
            }
            return result;
        }

        bool matches_component(const std::string_view pattern, const std::string_view value) {
            std::size_t pattern_index = 0;
            std::size_t value_index = 0;
            std::size_t star = std::string_view::npos;
            std::size_t retry = 0;
            while (value_index < value.size()) {
                if (pattern_index < pattern.size() && (pattern[pattern_index] == '?' || pattern[pattern_index] == value[value_index])) {
                    ++pattern_index;
                    ++value_index;
                } else if (pattern_index < pattern.size() && pattern[pattern_index] == '*') {
                    star = pattern_index++;
                    retry = value_index;
                } else if (star != std::string_view::npos) {
                    pattern_index = star + 1;
                    value_index = ++retry;
                } else {
                    return false;
                }
            }
            while (pattern_index < pattern.size() && pattern[pattern_index] == '*')
                ++pattern_index;

            return pattern_index == pattern.size();
        }

        std::size_t component_count(const std::string_view path) {
            std::size_t count = 0;
            bool inside = false;
            for (const char character: path) {
                if (character == '/') {
                    inside = false;
                } else if (!inside) {
                    inside = true;
                    ++count;
                }
            }
            return count;
        }

        std::string_view path_component(const std::string_view path, const std::size_t selected) {
            std::size_t index = 0;
            std::size_t begin = 0;
            while (begin < path.size()) {
                while (begin < path.size() && path[begin] == '/')
                    ++begin;

                const std::size_t end = path.find('/', begin);
                if (index == selected)
                    return path.substr(begin, end == std::string_view::npos ? path.size() - begin : end - begin);

                if (end == std::string_view::npos)
                    break;

                begin = end + 1;
                ++index;
            }
            return {};
        }

        class GlobPattern {
        public:
            explicit GlobPattern(const std::string_view pattern)
                : m_components(path_components(pattern)) {}

            [[nodiscard]] bool matches(const std::string_view path) const {
                const std::size_t path_size = component_count(path);
                std::size_t pattern_index = 0;
                std::size_t path_index = 0;
                std::size_t recursive_pattern = std::string_view::npos;
                std::size_t retry_path = 0;
                while (path_index < path_size) {
                    if (pattern_index < m_components.size() && m_components[pattern_index] == "**") {
                        recursive_pattern = pattern_index++;
                        retry_path = path_index;
                    } else if (
                        pattern_index < m_components.size()
                        && matches_component(m_components[pattern_index], path_component(path, path_index))
                    ) {
                        ++pattern_index;
                        ++path_index;
                    } else if (recursive_pattern != std::string_view::npos) {
                        pattern_index = recursive_pattern + 1;
                        path_index = ++retry_path;
                    } else {
                        return false;
                    }
                }
                while (pattern_index < m_components.size() && m_components[pattern_index] == "**")
                    ++pattern_index;

                return pattern_index == m_components.size();
            }

        private:
            std::vector<std::string> m_components;
        };

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

        bool matches_any(const std::string_view path, const std::vector<GlobPattern>& patterns) {
            return std::ranges::any_of(patterns, [&](const GlobPattern& pattern) { return pattern.matches(path); });
        }

        std::filesystem::path output_path(const std::filesystem::path& path, const std::filesystem::path& relative_to) {
            const std::filesystem::path relative = path.lexically_relative(relative_to);
            return relative.empty() ? path.lexically_normal() : relative.lexically_normal();
        }

        std::optional<std::size_t> maximum_pattern_depth(const std::string_view pattern) {
            if (pattern.find("**") != std::string_view::npos)
                return std::nullopt;

            return static_cast<std::size_t>(std::ranges::distance(std::filesystem::path(pattern)));
        }

        std::size_t path_depth(const std::filesystem::path& path) {
            return static_cast<std::size_t>(std::ranges::distance(path));
        }
    }

    struct FileCatalog::Impl {
        struct Entry {
            std::filesystem::path path;
            bool directory = false;
            bool recurse = false;
            bool regular = false;
        };

        struct Directory {
            std::vector<Entry> entries;
            std::error_code failure;
            std::filesystem::path failure_path;
        };

        const Directory& read(const std::filesystem::path& declared_directory) {
            const std::filesystem::path directory = declared_directory.lexically_normal();
            if (const auto found = directories.find(directory); found != directories.end())
                return found->second;

            Directory loaded;
            std::error_code failure;
            std::filesystem::directory_iterator iterator(directory, failure);
            const std::filesystem::directory_iterator end;
            if (failure) {
                loaded.failure = failure;
                return directories.emplace(directory, std::move(loaded)).first->second;
            }

            while (iterator != end) {
                const std::filesystem::directory_entry& entry = *iterator;
                const std::filesystem::file_status link_status = entry.symlink_status(failure);
                if (failure) {
                    loaded.failure = failure;
                    loaded.failure_path = entry.path();
                    break;
                }

                const std::filesystem::file_status status = entry.status(failure);
                if (failure) {
                    loaded.failure = failure;
                    loaded.failure_path = entry.path();
                    break;
                }

                const bool directory_entry = std::filesystem::is_directory(status);
                loaded.entries.push_back(
                    {entry.path().lexically_normal(),
                        directory_entry,
                        directory_entry && !std::filesystem::is_symlink(link_status),
                        std::filesystem::is_regular_file(status)}
                );
                ++entries_read;

                iterator.increment(failure);
                if (failure) {
                    loaded.failure = failure;
                    loaded.failure_path = directory;
                    break;
                }
            }
            ++directories_read;
            return directories.emplace(directory, std::move(loaded)).first->second;
        }

        std::map<std::filesystem::path, Directory> directories;
        std::size_t directories_read = 0;
        std::size_t entries_read = 0;
    };

    FileCatalog::FileCatalog()
        : m_impl(std::make_unique<Impl>()) {}

    FileCatalog::~FileCatalog() = default;
    FileCatalog::FileCatalog(FileCatalog&&) noexcept = default;
    FileCatalog& FileCatalog::operator=(FileCatalog&&) noexcept = default;

    Result<std::vector<std::filesystem::path>> FileCatalog::expand(
        const FileSet& files,
        const std::filesystem::path& root,
        const std::filesystem::path& relative_to,
        const bool allow_unmatched
    ) {
        std::vector<GlobPattern> exclusions;
        exclusions.reserve(files.exclude.size());
        for (const std::string& pattern: files.exclude)
            exclusions.emplace_back(normalized_pattern(pattern));

        std::vector<std::filesystem::path> result;
        for (const std::string& declared: files.include) {
            const std::string pattern = normalized_pattern(declared);
            if (!is_glob_pattern(pattern)) {
                std::filesystem::path path = pattern;
                if (path.is_relative())
                    path = root / path;

                if (!matches_any(relative_pattern_path(path, root), exclusions))
                    result.push_back(output_path(path, relative_to));

                continue;
            }

            const GlobPattern matcher(pattern);
            const std::optional<std::size_t> maximum_depth = maximum_pattern_depth(pattern);
            std::vector<std::filesystem::path> pending{search_root(root, pattern)};
            bool matched = false;
            while (!pending.empty()) {
                std::filesystem::path directory = std::move(pending.back());
                pending.pop_back();

                const Impl::Directory& entries = m_impl->read(directory);
                if (entries.failure) {
                    if (allow_unmatched && entries.failure == std::errc::no_such_file_or_directory)
                        continue;

                    const std::string subject = entries.failure_path.empty() ? "file pattern `" + declared + "`"
                                                                             : "`" + entries.failure_path.string() + "`";
                    return std::unexpected(error_at(files.location, "cannot inspect " + subject + ": " + entries.failure.message()));
                }

                for (const Impl::Entry& entry: entries.entries) {
                    if (entry.directory) {
                        if (entry.recurse && (!maximum_depth || path_depth(entry.path.lexically_relative(root)) < *maximum_depth)) {
                            pending.push_back(entry.path);
                        }
                    } else if (entry.regular) {
                        const std::string relative = relative_pattern_path(entry.path, root);
                        if (matcher.matches(relative)) {
                            matched = true;
                            if (!matches_any(relative, exclusions))
                                result.push_back(output_path(entry.path, relative_to));
                        }
                    }
                }
            }

            if (!matched && !allow_unmatched)
                return std::unexpected(error_at(files.location, "file pattern `" + declared + "` matched no files"));
        }

        std::ranges::sort(result, {}, [](const std::filesystem::path& path) { return path.generic_string(); });
        result.erase(std::ranges::unique(result).begin(), result.end());
        return result;
    }

    std::size_t FileCatalog::directories_read() const noexcept {
        return m_impl->directories_read;
    }

    std::size_t FileCatalog::entries_read() const noexcept {
        return m_impl->entries_read;
    }

    bool is_glob_pattern(const std::string_view value) noexcept {
        return !value.starts_with("$<") && value.find_first_of("*?") != std::string_view::npos;
    }

    Result<std::vector<std::filesystem::path>> expand_file_set(
        const FileSet& files,
        const std::filesystem::path& root,
        const std::filesystem::path& relative_to,
        const bool allow_unmatched,
        FileCatalog* catalog
    ) {
        if (catalog)
            return catalog->expand(files, root, relative_to, allow_unmatched);

        FileCatalog local;
        return local.expand(files, root, relative_to, allow_unmatched);
    }
}
