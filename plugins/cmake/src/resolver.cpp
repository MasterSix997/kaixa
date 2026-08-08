#include <kaixa/plugin/cmake/resolver.hpp>

#include <kaixa/config/table_reader.hpp>

#include <filesystem>
#include <utility>

namespace kaixa::plugin::cmake {
    namespace {
        std::string configuration_name(const std::string& profile) {
            if (profile == "debug")
                return "Debug";
            if (profile == "release")
                return "Release";
            if (profile == "relwithdebinfo")
                return "RelWithDebInfo";
            if (profile == "minsizerel")
                return "MinSizeRel";
            return profile;
        }

        class ResolverImpl final : public Resolver {
        public:
            [[nodiscard]] ResolverInfo info() const override {
                return {"cmake", "adopts an existing CMake project"};
            }

            [[nodiscard]] Result<void> plan(
                const PackageNode& package,
                const BuildEnvironment& environment,
                BuildPlan& plan
            ) const override {
                std::filesystem::path source = package.directory;
                std::optional<std::string> generator;

                if (package.manifest && package.manifest->resolver_options) {
                    auto options_result = TableReader::bind(
                        *package.manifest->resolver_options,
                        "cmake"
                    );
                    if (!options_result)
                        return std::unexpected(options_result.error());
                    TableReader options = std::move(*options_result);

                    auto source_option = options.optional_string("source");
                    if (!source_option)
                        return std::unexpected(source_option.error());
                    if (*source_option)
                        source /= **source_option;

                    auto generator_option = options.optional_string("generator");
                    if (!generator_option)
                        return std::unexpected(generator_option.error());
                    generator = std::move(*generator_option);

                    auto finished = options.finish();
                    if (!finished)
                        return std::unexpected(finished.error());
                }

                const std::filesystem::path project = source / "CMakeLists.txt";
                if (!std::filesystem::is_regular_file(project)) {
                    SourceLocation location;
                    if (package.manifest)
                        location = package.manifest->location;
                    return std::unexpected(error_at(
                        std::move(location),
                        "CMake package `" + package.name + "` has no `"
                            + project.string() + "`"
                    ));
                }

                const std::filesystem::path output = environment.output / package.name;
                const std::string configuration = configuration_name(environment.profile);

                Action configure;
                configure.description = "configure " + package.name;
                configure.argv = {
                    "cmake",
                    "-S", source.string(),
                    "-B", output.string(),
                    "-DCMAKE_BUILD_TYPE=" + configuration
                };
                if (generator) {
                    configure.argv.push_back("-G");
                    configure.argv.push_back(*generator);
                }
                configure.working_directory = package.directory;
                configure.inputs.push_back(project);
                configure.outputs.push_back(output / "CMakeCache.txt");
                plan.add(std::move(configure));

                Action build;
                build.description = "build " + package.name;
                build.argv = {
                    "cmake",
                    "--build", output.string(),
                    "--config", configuration
                };
                build.working_directory = package.directory;
                build.inputs.push_back(output / "CMakeCache.txt");
                build.outputs.push_back(output);
                plan.add(std::move(build));
                return {};
            }
        };
    }

    std::unique_ptr<Resolver> make_resolver() {
        return std::make_unique<ResolverImpl>();
    }
}
