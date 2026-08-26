#include <kaixa/kaixa.hpp>
#include <kaixa/plugin/bundle.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <system_error>
#include <vector>

namespace {
    using Clock = std::chrono::steady_clock;

    class TemporaryWorkspace {
    public:
        TemporaryWorkspace() {
            const auto stamp = Clock::now().time_since_epoch().count();
            root = std::filesystem::temp_directory_path() / ("kaixa-workspace-planning-" + std::to_string(stamp));
            std::filesystem::create_directories(root);
        }

        ~TemporaryWorkspace() {
            std::error_code failure;
            std::filesystem::remove_all(root, failure);
        }

        bool write(const std::filesystem::path& relative, const std::string& contents) const {
            const std::filesystem::path path = root / relative;
            std::error_code failure;
            std::filesystem::create_directories(path.parent_path(), failure);
            if (failure)
                return false;

            std::ofstream output(path, std::ios::binary | std::ios::trunc);
            output << contents;
            return static_cast<bool>(output);
        }

        std::filesystem::path root;
    };

    std::string package_name(const std::size_t index) {
        return "package" + std::to_string(index);
    }

    bool populate(TemporaryWorkspace& workspace, const std::size_t package_count, const std::size_t files_per_package) {
        const std::string root_manifest = "[package-set]\n"
                                          "name = \"planning-benchmark\"\n"
                                          "members = [\"packages/*\"]\n"
                                          "default = [\""
            + package_name(package_count - 1)
            + "\"]\n";
        if (!workspace.write("Kaixa.toml", root_manifest))
            return false;

        for (std::size_t package = 0; package < package_count; ++package) {
            const std::string name = package_name(package);
            std::string manifest = "[package]\n"
                                   "name = \""
                + name
                + "\"\n"
                  "version = \"0.1.0\"\n"
                  "resolver = \"cmake\"\n";
            if (package != 0) {
                manifest += "\n[dependencies]\n" + package_name(package - 1) + " = \"0.1.0\"\n";
            }
            manifest += "\n[lib]\n"
                        "type = \"static\"\n"
                        "sources = [\"src/**/*.cpp\"]\n"
                        "headers = [\"include/**/*.hpp\"]\n"
                        "public-include = [\"include\"]\n"
                        "\n[cmake]\n"
                        "generation = \"state\"\n";
            if (!workspace.write(std::filesystem::path("packages") / name / "Kaixa.toml", manifest))
                return false;

            for (std::size_t file = 0; file < files_per_package; ++file) {
                const std::string stem = name + '_' + std::to_string(file);
                if (!workspace.write(
                        std::filesystem::path("packages") / name / "src" / (stem + ".cpp"),
                        "int " + stem + "() { return " + std::to_string(file) + "; }\n"
                    )) {
                    return false;
                }
                if (!workspace.write(std::filesystem::path("packages") / name / "include" / (stem + ".hpp"), "int " + stem + "();\n")) {
                    return false;
                }
            }
        }
        return true;
    }

    template <typename Duration> double milliseconds(const Duration duration) {
        return std::chrono::duration<double, std::milli>(duration).count();
    }
}

int main() {
    constexpr std::size_t package_count = 48;
    constexpr std::size_t files_per_package = 8;
    constexpr std::size_t iterations = 9;
#ifdef NDEBUG
    constexpr double resolution_budget_ms = 500.0;
    constexpr double plan_budget_ms = 150.0;
#else
    constexpr double resolution_budget_ms = 1'500.0;
    constexpr double plan_budget_ms = 750.0;
#endif

    TemporaryWorkspace workspace;
    if (!populate(workspace, package_count, files_per_package)) {
        std::cerr << "cannot create planning benchmark workspace\n";
        return 1;
    }

    kaixa::ExtensionRegistry registry = kaixa::plugin::default_registry();
    kaixa::ResolutionOptions resolution_options;
    resolution_options.extensions = &registry;
    resolution_options.write_lock = false;
    resolution_options.refresh_sources = false;

    const auto resolution_started = Clock::now();
    auto resolution = kaixa::resolve_workspace(workspace.root, resolution_options);
    const double resolution_ms = milliseconds(Clock::now() - resolution_started);
    if (!resolution) {
        std::cerr << kaixa::format_diagnostic(resolution.error()) << '\n';
        return 1;
    }

    const kaixa::BuildEnvironment environment{workspace.root, workspace.root / ".kaixa"};
    std::vector<double> samples;
    samples.reserve(iterations);
    std::size_t actions = 0;
    std::size_t generated_files = 0;
    for (std::size_t iteration = 0; iteration < iterations; ++iteration) {
        const auto started = Clock::now();
        auto plan = kaixa::plan_build(resolution->graph, registry, environment);
        samples.push_back(milliseconds(Clock::now() - started));
        if (!plan) {
            std::cerr << kaixa::format_diagnostic(plan.error()) << '\n';
            return 1;
        }

        actions = plan->actions().size();
        generated_files = plan->generated_files().size();
    }

    std::ranges::sort(samples);
    const double median_ms = samples[samples.size() / 2];
    const double maximum_ms = samples.back();
    std::cout << package_count << " packages resolved in " << resolution_ms << " ms\n";
    std::cout << iterations << " plans: median " << median_ms << " ms, maximum " << maximum_ms << " ms\n";
    std::cout << actions << " actions, " << generated_files << " generated files\n";
    if (resolution_ms > resolution_budget_ms) {
        std::cerr << "workspace resolution exceeded " << resolution_budget_ms << " ms budget\n";
        return 1;
    }
    if (median_ms > plan_budget_ms) {
        std::cerr << "planning median exceeded " << plan_budget_ms << " ms budget\n";
        return 1;
    }
}
