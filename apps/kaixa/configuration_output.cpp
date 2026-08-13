#include "configuration_output.hpp"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <optional>
#include <sstream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace kaixa::cli {
    namespace {
        std::string display_path(const std::filesystem::path& path, const std::filesystem::path& workspace) {
            const std::filesystem::path relative = path.lexically_relative(workspace);
            if (!relative.empty() && !relative.is_absolute() && *relative.begin() != "..")
                return relative.generic_string();

            return path.string();
        }

        std::string quote(const std::string_view value) {
            std::string output = "\"";
            for (const char character: value) {
                if (character == '\\' || character == '"')
                    output += '\\';

                if (character == '\n') {
                    output += "\\n";
                    continue;
                }

                output += character;
            }
            output += '"';
            return output;
        }

        std::string format_value(const Value& value) {
            if (const bool* boolean = value.as_boolean())
                return *boolean ? "true" : "false";

            if (const std::int64_t* integer = value.as_integer())
                return std::to_string(*integer);

            if (const double* floating = value.as_floating()) {
                std::ostringstream output;
                output << *floating;
                return output.str();
            }

            if (const std::string* string = value.as_string())
                return quote(*string);

            if (const std::vector<Value>* array = value.as_array()) {
                std::string output = "[";
                for (std::size_t index = 0; index < array->size(); ++index) {
                    if (index != 0)
                        output += ", ";

                    output += format_value((*array)[index]);
                }
                return output + ']';
            }
            return value.is_table() ? "{}" : "none";
        }

        std::string format_arguments(const std::span<const std::string> arguments) {
            std::string output = "[";
            for (std::size_t index = 0; index < arguments.size(); ++index) {
                if (index != 0)
                    output += ", ";

                output += quote(arguments[index]);
            }
            return output + ']';
        }

        std::string origin_text(const SourceLocation& location, const std::filesystem::path& workspace) {
            if (location.source.empty())
                return "built-in";

            if (location.source == "command line")
                return location.source;

            std::string output = display_path(location.source, workspace);
            if (location.line != 0)
                output += ':' + std::to_string(location.line);

            return output;
        }

        void print_entries(
            const Value& value,
            const std::string_view prefix,
            const std::filesystem::path& workspace
        ) {
            const std::vector<TableEntry>* table = value.as_table();
            if (!table) {
                std::cout << "    " << prefix << " = " << format_value(value)
                    << " [origin: " << origin_text(value.location(), workspace) << "]\n";
                return;
            }

            for (const TableEntry& entry: *table)
                print_entries(entry.value, join_config_path(prefix, entry.key), workspace);
        }

        struct DisplayOption {
            std::string value;
            SourceLocation origin;
        };

        struct CmakeDisplayOptions {
            std::optional<DisplayOption> generator;
            std::optional<DisplayOption> c_compiler;
            std::optional<DisplayOption> cxx_compiler;
            std::optional<DisplayOption> toolchain;
        };

        struct LocatedArgument {
            std::string_view value;
            SourceLocation origin;
        };

        void read_option(
            const Value& settings,
            const std::string_view key,
            std::optional<DisplayOption>& destination
        ) {
            const Value* value = settings.find(key);
            if (value && value->as_string())
                destination = DisplayOption{*value->as_string(), value->location()};
        }

        void append_arguments(const Value* value, std::vector<LocatedArgument>& destination) {
            if (!value || !value->as_array())
                return;

            for (const Value& argument: *value->as_array()) {
                if (argument.as_string())
                    destination.push_back({*argument.as_string(), argument.location()});
            }
        }

        void append_arguments(const std::span<const std::string> arguments, std::vector<LocatedArgument>& destination) {
            for (const std::string& argument: arguments)
                destination.push_back({argument, SourceLocation{.source = "command line"}});
        }

        void apply_definition(
            const LocatedArgument& argument,
            const std::string_view name,
            std::optional<DisplayOption>& destination
        ) {
            const std::string prefix = "-D" + std::string(name) + '=';
            if (argument.value.starts_with(prefix)) {
                destination = DisplayOption{
                    std::string(argument.value.substr(prefix.size())),
                    argument.origin
                };
            }
        }

        CmakeDisplayOptions effective_cmake_options(const ResolverBuildConfiguration& resolver) {
            CmakeDisplayOptions result;
            std::vector<LocatedArgument> arguments;
            if (resolver.settings) {
                read_option(*resolver.settings, "generator", result.generator);
                read_option(*resolver.settings, "c-compiler", result.c_compiler);
                read_option(*resolver.settings, "cxx-compiler", result.cxx_compiler);
                read_option(*resolver.settings, "toolchain", result.toolchain);
                append_arguments(resolver.settings->find("arguments"), arguments);
                append_arguments(resolver.settings->find("configure-arguments"), arguments);
            }

            append_arguments(resolver.arguments, arguments);
            for (const ResolverArgumentGroup& group: resolver.scoped_arguments) {
                if (group.scope == "configure")
                    append_arguments(group.arguments, arguments);
            }

            bool found_generator = false;
            for (std::size_t index = 0; index < arguments.size(); ++index) {
                const LocatedArgument& argument = arguments[index];
                if (!found_generator && (argument.value == "-G" || argument.value == "--generator")
                    && index + 1 < arguments.size()) {
                    result.generator = DisplayOption{
                        std::string(arguments[index + 1].value),
                        argument.origin
                    };
                    found_generator = true;
                } else if (!found_generator && argument.value.starts_with("-G")
                    && argument.value.size() > 2) {
                    result.generator = DisplayOption{
                        std::string(argument.value.substr(2)),
                        argument.origin
                    };
                    found_generator = true;
                } else if (!found_generator && argument.value.starts_with("--generator=")) {
                    result.generator = DisplayOption{
                        std::string(argument.value.substr(std::string_view("--generator=").size())),
                        argument.origin
                    };
                    found_generator = true;
                }

                apply_definition(argument, "CMAKE_C_COMPILER", result.c_compiler);
                apply_definition(argument, "CMAKE_CXX_COMPILER", result.cxx_compiler);
                apply_definition(argument, "CMAKE_TOOLCHAIN_FILE", result.toolchain);
            }
            return result;
        }

        void print_option(
            const std::string_view name,
            const std::optional<DisplayOption>& option,
            const std::filesystem::path& workspace,
            const std::string_view fallback
        ) {
            std::cout << "  " << name << " = ";
            if (!option) {
                std::cout << fallback << '\n';
                return;
            }

            std::cout << quote(option->value) << " [origin: "
                << origin_text(option->origin, workspace) << "]\n";
        }

        void print_effective_cmake(
            const EffectiveBuildConfiguration& configuration,
            const std::filesystem::path& workspace
        ) {
            ResolverBuildConfiguration defaults;
            defaults.resolver = "cmake";
            const ResolverBuildConfiguration* resolver = configuration.find("cmake");
            const CmakeDisplayOptions options = effective_cmake_options(
                resolver ? *resolver : defaults
            );

            std::cout << "effective CMake:\n";
            print_option("generator", options.generator, workspace, "<CMake default>");
            print_option("c-compiler", options.c_compiler, workspace, "<CMake default>");
            print_option("cxx-compiler", options.cxx_compiler, workspace, "<CMake default>");
            print_option("toolchain", options.toolchain, workspace, "none");
        }

        void print_selected_configurations(
            const EffectiveBuildConfiguration& configuration,
            const std::vector<ConfigurationSource>& sources,
            const std::filesystem::path& workspace
        ) {
            if (configuration.selected.empty()) {
                std::cout << "selected configurations: none\n";
                return;
            }

            std::cout << "selected configurations:\n";
            for (const std::string& selected: configuration.selected) {
                std::cout << "  " << selected;
                bool wrote_origin = false;
                for (const ConfigurationSource& source: sources) {
                    for (const ConfigurationDefinition& definition: source.configurations.definitions) {
                        if (definition.name != selected)
                            continue;

                        std::cout << (wrote_origin ? ", " : " [origin: ")
                            << origin_text(definition.location, workspace);
                        wrote_origin = true;
                    }
                }
                if (wrote_origin)
                    std::cout << ']';

                std::cout << '\n';
            }
        }

        void print_resolver_settings(
            const EffectiveBuildConfiguration& configuration,
            const std::filesystem::path& workspace
        ) {
            if (configuration.resolvers.empty()) {
                std::cout << "resolver settings: none\n";
                return;
            }

            std::cout << "resolver settings:\n";
            for (const ResolverBuildConfiguration& resolver: configuration.resolvers) {
                std::cout << "  " << resolver.resolver << ":\n";
                if (resolver.settings)
                    print_entries(*resolver.settings, {}, workspace);

                if (!resolver.arguments.empty()) {
                    std::cout << "    configure-arguments += "
                        << format_arguments(resolver.arguments) << " [origin: command line]\n";
                }
                for (const ResolverArgumentGroup& arguments: resolver.scoped_arguments) {
                    std::cout << "    " << arguments.scope << "-arguments += "
                        << format_arguments(arguments.arguments) << " [origin: command line]\n";
                }
            }
        }
    }

    void print_configuration_list(
        const std::vector<ConfigurationSource>& sources,
        const EffectiveBuildConfiguration& configuration,
        const std::filesystem::path& workspace
    ) {
        std::vector<std::string> names;
        for (const ConfigurationSource& source: sources) {
            for (const ConfigurationDefinition& definition: source.configurations.definitions) {
                if (std::ranges::find(names, definition.name) == names.end())
                    names.push_back(definition.name);
            }
        }

        if (names.empty()) {
            std::cout << "no build configurations\n";
            return;
        }

        for (const std::string& name: names) {
            const bool selected = std::ranges::find(configuration.selected, name)
                != configuration.selected.end();
            std::cout << name << (selected ? " [default]" : "") << '\n';
            for (const ConfigurationSource& source: sources) {
                for (const ConfigurationDefinition& definition: source.configurations.definitions) {
                    if (definition.name == name) {
                        std::cout << "  " << source.name << ": "
                            << origin_text(definition.location, workspace) << '\n';
                    }
                }
            }
        }
    }

    void print_effective_configuration(
        const EffectiveBuildConfiguration& configuration,
        const std::vector<ConfigurationSource>& sources,
        const std::string_view root_resolver,
        const std::filesystem::path& workspace
    ) {
        std::cout << "profile: " << configuration.profile << " [origin: "
            << origin_text(configuration.profile_origin, workspace) << "]\n";
        print_selected_configurations(configuration, sources, workspace);
        print_resolver_settings(configuration, workspace);
        if (root_resolver == "cmake")
            print_effective_cmake(configuration, workspace);
    }
}
