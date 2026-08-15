#include <test_support.hpp>

#include <kaixa/kaixa.hpp>
#include <kaixa/plugin/bundle.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace {
    const std::filesystem::path workspaces_directory =
        std::filesystem::path(__FILE__).parent_path() / "workspaces";
}

KAIXA_TEST(single_package_workspace_loads_and_plans) {
    const std::filesystem::path workspace = workspaces_directory / "single_package";
    const auto graph = kaixa::load_workspace(workspace);
    context.check(graph.has_value(), "test workspace loads");
    if (!graph) {
        context.fail(kaixa::format_diagnostic(graph.error()));
        return;
    }

    context.check_equal(graph->size(), std::size_t{1}, "one package");
    context.check_equal((*graph)[graph->roots().front()].name, std::string("test_single"), "package name");

    const kaixa::ExtensionRegistry registry = kaixa::plugin::default_registry();
    const kaixa::BuildEnvironment environment{
        workspace,
        workspace / ".test-output",
        "debug"
    };
    const auto plan = kaixa::plan_build(*graph, registry, environment);
    context.check(plan.has_value(), "test workspace plans");
    if (plan) {
        context.check_equal(plan->actions().size(), std::size_t{2}, "configure and build");
        context.check_equal(
            plan->actions().front().description,
            std::string("configure test_single"),
            "configure action"
        );
    }
}

KAIXA_TEST(cmake_plans_each_selected_package_root) {
    const kaixa::testing::TempDirectory workspace("multiple-cmake-roots");
    workspace.write(
        "Kaixa.toml",
        "[package-set]\n"
        "members = [\"packages/*\"]\n"
        "default = [\"editor\", \"game_runner\"]\n"
    );
    workspace.write(
        "packages/editor/Kaixa.toml",
        "[package]\n"
        "name = \"editor\"\n"
        "resolver = \"cmake\"\n"
    );
    workspace.write(
        "packages/editor/CMakeLists.txt",
        "cmake_minimum_required(VERSION 3.20)\n"
        "project(editor LANGUAGES NONE)\n"
        "add_custom_target(editor)\n"
    );
    workspace.write(
        "packages/game_runner/Kaixa.toml",
        "[package]\n"
        "name = \"game_runner\"\n"
        "resolver = \"cmake\"\n"
    );
    workspace.write(
        "packages/game_runner/CMakeLists.txt",
        "cmake_minimum_required(VERSION 3.20)\n"
        "project(game_runner LANGUAGES NONE)\n"
        "add_custom_target(game_runner)\n"
    );

    const auto graph = kaixa::load_workspace(workspace.path());
    context.check(graph.has_value(), "multiple CMake roots load");
    if (!graph)
        return;

    const kaixa::ExtensionRegistry registry = kaixa::plugin::default_registry();
    const kaixa::BuildEnvironment environment{
        workspace.path(),
        workspace.path() / ".kaixa",
        "debug"
    };
    const auto plan = kaixa::plan_build(*graph, registry, environment);
    context.check(plan.has_value(), "multiple CMake roots plan");
    if (!plan) {
        context.fail(kaixa::format_diagnostic(plan.error()));
        return;
    }

    context.check_equal(plan->outputs().size(), std::size_t{2}, "one build output per root");
    for (const kaixa::PackageId root: graph->roots()) {
        const std::string description = "configure " + (*graph)[root].name;
        context.check(
            std::ranges::find(plan->actions(), description, &kaixa::Action::description)
                != plan->actions().end(),
            description
        );
    }

    const kaixa::PackageId game_runner = graph->roots().back();
    kaixa::BuildRequest request;
    request.packages.push_back({game_runner, {"game_runner"}, false});
    const auto selected = kaixa::plan_build(*graph, registry, environment, request);
    context.check(selected.has_value(), "one package root can receive explicit targets");
    if (!selected)
        return;

    context.check(
        std::ranges::none_of(selected->actions(), [&](const kaixa::Action& action) {
            return action.package == graph->roots().front();
        }),
        "unselected root receives no actions"
    );
    const auto selected_build = std::ranges::find_if(
        selected->actions(),
        [&](const kaixa::Action& action) {
            return action.package == game_runner && action.stage == kaixa::ActionStage::build;
        }
    );
    context.check(selected_build != selected->actions().end(), "selected root has a build action");
    if (selected_build != selected->actions().end()) {
        context.check(
            std::ranges::find(selected_build->argv, "game_runner") != selected_build->argv.end(),
            "explicit target reaches only its package"
        );
    }
}

KAIXA_TEST(adopted_cmake_project_exposes_products_and_run_targets) {
    const kaixa::testing::TempDirectory workspace("adopted-cmake-run");
    workspace.copy_from(workspaces_directory / "single_package");

    const auto graph = kaixa::load_workspace(workspace.path());
    context.check(graph.has_value(), "adopted workspace loads");
    if (!graph)
        return;

    const kaixa::ExtensionRegistry registry = kaixa::plugin::default_registry();
    const kaixa::BuildEnvironment environment{
        workspace.path(),
        workspace.path() / ".kaixa",
        "debug"
    };
    const auto plan = kaixa::plan_build(*graph, registry, environment);
    context.check(plan.has_value(), "adopted workspace plans");
    if (!plan)
        return;

    const auto generated = kaixa::generate(*plan);
    context.check(generated.has_value(), "adopted workspace configures");
    if (!generated)
        return;

    const auto targets = kaixa::discover_run_targets(*graph, registry, environment);
    context.check(targets.has_value(), "adopted CMake targets are discovered");
    if (targets) {
        context.check_equal(targets->size(), std::size_t{1}, "one adopted executable");
        if (!targets->empty()) {
            context.check_equal(
                targets->front().name,
                std::string("test_single"),
                "adopted executable target"
            );
        }
    }

    const auto products = kaixa::discover_products(*graph, registry, environment);
    context.check(products.has_value(), "adopted CMake products are discovered");
    if (products) {
        context.check_equal(products->size(), std::size_t{2}, "executable and library products");
        const auto library = std::ranges::find_if(*products, [](const kaixa::BuildProduct& product) {
            return product.name == "test_single_support";
        });
        context.check(library != products->end(), "adopted library target is discovered");
        if (library != products->end()) {
            context.check(
                library->kind == kaixa::ProductKind::static_library,
                "adopted library kind"
            );
            context.check(library->artifact.has_value(), "adopted library artifact is known");
        }
    }

    kaixa::EffectiveBuildConfiguration changed_configuration;
    changed_configuration.profile = "debug";
    changed_configuration.resolvers.push_back({
        "cmake",
        kaixa::Value::table({
            {"build-arguments", kaixa::Value::array({"--verbose"})}
        }),
        {},
        {}
    });
    const kaixa::BuildEnvironment changed_environment{
        workspace.path(),
        workspace.path() / ".kaixa",
        std::move(changed_configuration)
    };
    const auto changed_plan = kaixa::plan_build(*graph, registry, changed_environment);
    context.check(changed_plan.has_value(), "changed build arguments plan");
    if (changed_plan) {
        const auto state = kaixa::check(*changed_plan);
        context.check(state.has_value(), "changed build arguments can be checked");
        if (state) {
            context.check(
                !state->requires_synchronization(),
                "build arguments do not require CMake reconfiguration"
            );
        }
    }
}

KAIXA_TEST(source_dependency_workspace_models_managed_and_opaque_packages) {
    const std::filesystem::path workspace = workspaces_directory / "source_dependency";
    const auto graph = kaixa::load_workspace(workspace);
    context.check(graph.has_value(), "test workspace loads");
    if (!graph) {
        context.fail(kaixa::format_diagnostic(graph.error()));
        return;
    }

    context.check_equal(graph->size(), std::size_t{3}, "three packages");
    const auto math = graph->find_by_name("test_source_math");
    const auto assets = graph->find_by_name("assets");
    context.check(math.has_value(), "managed dependency exists");
    context.check(assets.has_value(), "opaque dependency exists");
    if (assets)
        context.check((*graph)[*assets].kind == kaixa::PackageKind::opaque, "assets are opaque");

    const auto order = graph->build_order();
    context.check(order.has_value(), "workspace has an order");
    if (order && math) {
        const auto math_position = std::ranges::find(*order, *math);
        const auto root_position = std::ranges::find(*order, graph->roots().front());
        context.check(math_position < root_position, "managed dependency precedes root");
    }

    const kaixa::ExtensionRegistry registry = kaixa::plugin::default_registry();
    const kaixa::BuildEnvironment environment{
        workspace,
        workspace / ".test-output",
        "debug"
    };
    const auto plan = kaixa::plan_build(*graph, registry, environment);
    context.check(plan.has_value(), "workspace example plans");
    if (plan) {
        context.check_equal(plan->actions().size(), std::size_t{2}, "one composed CMake build");
        context.check_equal(
            plan->generated_files().size(),
            std::size_t{3},
            "variant metadata, integration and File API query"
        );
        if (plan->actions().size() == 2 && !plan->generated_files().empty()) {
            context.check_equal(
                plan->actions()[0].description,
                std::string("configure test_source_app"),
                "root is configured once"
            );
            const auto integration = std::ranges::find_if(
                plan->generated_files(),
                [](const kaixa::GeneratedFile& generated) {
                    return generated.path.filename() == "dependencies.cmake";
                }
            );
            context.check(
                integration != plan->generated_files().end(),
                "dependency integration is generated"
            );
            if (integration != plan->generated_files().end()) {
                context.check_contains(
                    integration->content,
                    "add_subdirectory",
                    "source dependency is composed"
                );
            }

            const std::vector<std::string> configure = plan->actions()[0].argv;
            context.check(
                std::ranges::find_if(configure, [](const std::string& argument) {
                    return argument.starts_with("-DCMAKE_PROJECT_INCLUDE=");
                }) != configure.end(),
                "root receives the generated integration file"
            );
        }
    }
}

KAIXA_TEST(package_dependency_workspace_uses_install_and_find_package) {
    const std::filesystem::path workspace = workspaces_directory / "package_dependency";
    const auto graph = kaixa::load_workspace(workspace);
    if (!graph) {
        context.fail(kaixa::format_diagnostic(graph.error()));
        return;
    }

    const kaixa::ExtensionRegistry registry = kaixa::plugin::default_registry();
    const kaixa::BuildEnvironment environment{workspace, workspace / ".test-output", "debug"};
    const auto plan = kaixa::plan_build(*graph, registry, environment);
    context.check(plan.has_value(), "package dependency example plans");
    if (!plan)
        return;

    context.check_equal(plan->actions().size(), std::size_t{5}, "dependency installs before app");
    if (plan->actions().size() != 5)
        return;

    context.check_equal(
        plan->actions()[2].description,
        std::string("install test_package_math"),
        "provider is installed"
    );
    context.check_equal(
        plan->actions()[3].description,
        std::string("configure test_package_app"),
        "consumer configures after provider"
    );
    for (std::size_t index = 0; index < 4; ++index) {
        context.check(
            plan->actions()[index].stage == kaixa::ActionStage::synchronize,
            "provider preparation and consumer configuration synchronize"
        );
    }

    context.check(
        plan->actions()[4].stage == kaixa::ActionStage::build,
        "consumer compilation stays in the build stage"
    );

    const std::string artifact_root =
        (environment.state_root / "cache" / "cmake").string();
    const std::vector<std::string> configure = plan->actions()[3].argv;
    context.check(
        std::ranges::find_if(configure, [&](const std::string& argument) {
            return argument.starts_with("-DKAIXA_CMAKE_PREFIX_PATH=")
                && argument.contains(artifact_root)
                && argument.ends_with("test_package_math");
        }) != configure.end(),
        "consumer receives the package prefix"
    );
}

KAIXA_TEST(generated_project_workspace_builds_from_kaixa_toml) {
    const kaixa::testing::TempDirectory workspace("generated-cmake-project");
    workspace.copy_from(workspaces_directory / "generated_project");

    const auto graph = kaixa::load_workspace(workspace.path());
    if (!graph) {
        context.fail(kaixa::format_diagnostic(graph.error()));
        return;
    }

    const kaixa::ExtensionRegistry registry = kaixa::plugin::default_registry();
    const kaixa::BuildEnvironment environment{
        workspace.path(),
        workspace.path() / ".kaixa",
        "debug"
    };
    const kaixa::TestRequest request{"gener", "test_generated"};
    const auto plan = kaixa::plan_tests(*graph, registry, environment, request);
    context.check(plan.has_value(), "generated project test plans");
    if (!plan)
        return;

    context.check_equal(plan->actions().size(), std::size_t{3}, "configure, build and test actions");
    const auto build_action = std::ranges::find_if(
        plan->actions(),
        [&](const kaixa::Action& action) {
            return action.package == graph->roots().front()
                && action.stage == kaixa::ActionStage::build;
        }
    );
    context.check(build_action != plan->actions().end(), "root build action is identifiable");
    if (build_action != plan->actions().end()) {
        context.check(
            std::ranges::find(build_action->argv, "test_generated") != build_action->argv.end(),
            "selected test target restricts the build"
        );
    }

    const auto test_action = std::ranges::find_if(
        plan->actions(),
        [](const kaixa::Action& action) {
            return action.stage == kaixa::ActionStage::test;
        }
    );
    context.check(test_action != plan->actions().end(), "CTest action exists");
    if (test_action != plan->actions().end()) {
        context.check(
            std::ranges::find(test_action->argv, "gener") != test_action->argv.end(),
            "test name filter reaches CTest"
        );
        context.check(
            std::ranges::find(test_action->argv, "^kaixa\\.target:test_generated$")
                != test_action->argv.end(),
            "target label restricts CTest"
        );
    }

    kaixa::TestRequest list_request = request;
    list_request.mode = kaixa::TestMode::list;
    const auto list_plan = kaixa::plan_tests(
        *graph,
        registry,
        environment,
        list_request
    );
    context.check(list_plan.has_value(), "generated project test list plans");
    if (list_plan) {
        const auto list_action = std::ranges::find_if(
            list_plan->actions(),
            [](const kaixa::Action& action) {
                return action.stage == kaixa::ActionStage::test;
            }
        );
        context.check(list_action != list_plan->actions().end(), "CTest list action exists");
        if (list_action != list_plan->actions().end()) {
            context.check(
                std::ranges::find(list_action->argv, "--show-only")
                    != list_action->argv.end(),
                "CTest receives list mode"
            );
            context.check(
                std::ranges::find(list_action->argv, "--output-on-failure")
                    == list_action->argv.end(),
                "list mode does not receive execution-only output options"
            );
        }
    }

    const auto generated = std::ranges::find_if(
        plan->generated_files(),
        [](const kaixa::GeneratedFile& candidate) {
            return candidate.path.filename() == "CMakeLists.txt"
                && candidate.path.parent_path().filename() == "project"
                && candidate.path.parent_path().parent_path().filename() == "test_generated";
        }
    );
    context.check(
        generated != plan->generated_files().end(),
        "CMakeLists.txt is generated in Kaixa state"
    );
    context.check(
        !std::filesystem::exists(workspace.path() / "CMakeLists.txt"),
        "source root remains clean"
    );
    context.check(
        !std::filesystem::exists(workspace.path() / "packages/math/CMakeLists.txt"),
        "dependency source remains clean"
    );

    const auto metadata = std::ranges::find_if(
        plan->generated_files(),
        [](const kaixa::GeneratedFile& candidate) {
            return candidate.path.filename() == "variant.toml";
        }
    );
    context.check(metadata != plan->generated_files().end(), "build variant has metadata");
    if (metadata != plan->generated_files().end()) {
        context.check(
            metadata->path.parent_path().filename() == "debug",
            "variant directory uses the readable label"
        );
        context.check_contains(metadata->content, "label = \"debug\"", "variant label");
        context.check_contains(metadata->content, "fingerprint = ", "variant fingerprint");
        context.check_contains(metadata->content, "profile = \"debug\"", "variant profile");
        context.check(
            std::ranges::any_of(plan->actions(), [&](const kaixa::Action& action) {
                return action.description == "configure test_generated"
                    && std::ranges::find(action.inputs, metadata->path) != action.inputs.end();
            }),
            "variant metadata changes require CMake configuration"
        );
    }

    context.check_equal(plan->outputs().size(), std::size_t{1}, "one public output root");
    if (!plan->outputs().empty()) {
        context.check_equal(
            plan->outputs().front().path,
            environment.state_root / "build/debug",
            "public output has a short stable path"
        );
    }

    if (generated != plan->generated_files().end()) {
        context.check_contains(
            generated->content,
            "add_library(test_generated_support STATIC",
            "library target command"
        );
        context.check_contains(
            generated->content,
            "add_executable(test_generated",
            "executable target command"
        );
        context.check_contains(
            generated->content,
            "SYSTEM PUBLIC",
            "system include scope"
        );
        context.check_contains(
            generated->content,
            "target_compile_definitions",
            "compile definitions"
        );
        context.check_contains(generated->content, "cxx_std_20", "target overrides default");
        context.check_contains(
            generated->content,
            "CMAKE_RUNTIME_OUTPUT_DIRECTORY",
            "project output policy"
        );
        context.check_contains(generated->content, "add_test", "CTest registration");
        context.check_contains(generated->content, "TEST_INCLUDE_FILES", "CTest discovery include");
        context.check_contains(generated->content, "--kaixa-test-list", "test listing protocol");
        context.check_contains(generated->content, "--kaixa-test-run", "single test protocol");
        context.check_contains(generated->content, "kaixa.target:test_generated", "CTest target label");
        context.check_contains(generated->content, "cxx_std_23", "C++ standard");
    }

    const auto synchronization = kaixa::generate(*plan);
    context.check(synchronization.has_value(), "generated project synchronizes");
    if (!synchronization) {
        context.fail(kaixa::format_diagnostic(synchronization.error()));
        return;
    }
    context.check_equal(
        synchronization->synchronized,
        std::size_t{1},
        "CMake configuration synchronizes"
    );

    const auto run_targets = kaixa::discover_run_targets(*graph, registry, environment);
    context.check(run_targets.has_value(), "configured project exposes run targets");
    if (run_targets) {
        context.check_equal(run_targets->size(), std::size_t{1}, "one executable target");
        if (!run_targets->empty()) {
            context.check_equal(
                run_targets->front().name,
                std::string("test_generated"),
                "CMake executable target name"
            );
            context.check(
                !run_targets->front().process.argv.empty()
                    && std::filesystem::path(run_targets->front().process.argv.front())
                        .filename().stem() == "test_generated",
                "CMake reports the executable artifact"
            );
        }
    }

    const auto run_plan = kaixa::plan_run(
        *graph,
        registry,
        environment,
        "test_generated"
    );
    context.check(run_plan.has_value(), "run target plans");
    if (run_plan) {
        const auto build = std::ranges::find_if(
            run_plan->actions(),
            [](const kaixa::Action& action) {
                return action.description == "build selected targets for test_generated";
            }
        );
        context.check(build != run_plan->actions().end(), "run plan has a build action");
        if (build != run_plan->actions().end()) {
            context.check(
                std::ranges::find(build->argv, "--target") != build->argv.end()
                    && std::ranges::find(build->argv, "test_generated") != build->argv.end(),
                "run plan builds only the selected target"
            );
        }
    }

    const auto clean_plan = kaixa::plan_clean(*graph, registry, environment);
    context.check(clean_plan.has_value(), "CMake clean paths plan");
    if (clean_plan) {
        context.check(
            std::ranges::find(
                clean_plan->paths(),
                environment.state_root / "build/debug"
            ) != clean_plan->paths().end(),
            "clean plan owns public artifacts"
        );
        context.check(
            std::ranges::find(
                clean_plan->paths(),
                environment.state_root / "build/cmake/debug"
            ) != clean_plan->paths().end(),
            "clean plan owns private CMake state"
        );
        context.check(
            std::ranges::find(
                clean_plan->paths(),
                environment.state_root / "generated/cmake"
            ) == clean_plan->paths().end(),
            "regular clean preserves generated state"
        );
    }

    const auto generated_clean_plan = kaixa::plan_clean(
        *graph,
        registry,
        environment,
        kaixa::CleanRequest{true}
    );
    context.check(generated_clean_plan.has_value(), "CMake generated clean paths plan");
    if (generated_clean_plan) {
        context.check(
            std::ranges::find(
                generated_clean_plan->paths(),
                environment.state_root / "generated/cmake"
            ) == generated_clean_plan->paths().end(),
            "generated files option does not select state generation"
        );
        context.check(
            generated_clean_plan->generated_files().empty(),
            "state generation has no source files to clean"
        );
    }

    const auto synchronized_plan = kaixa::plan_build(*graph, registry, environment);
    context.check(synchronized_plan.has_value(), "synchronized project plans again");
    if (synchronized_plan) {
        const auto state = kaixa::check(*synchronized_plan);
        context.check(state.has_value(), "synchronized project can be checked");
        if (state) {
            context.check(
                !state->requires_synchronization(),
                "generate leaves no required synchronization"
            );
        }
    }

    kaixa::EffectiveBuildConfiguration changed_configuration;
    changed_configuration.profile = "debug";
    changed_configuration.resolvers.push_back({
        "cmake",
        kaixa::Value::table({{"generator", "Ninja"}}),
        {},
        {}
    });
    const kaixa::BuildEnvironment changed_environment{
        workspace.path(),
        workspace.path() / ".kaixa",
        std::move(changed_configuration)
    };
    const auto changed_variant = kaixa::plan_build(*graph, registry, changed_environment);
    context.check(changed_variant.has_value(), "changed CMake variant plans");
    if (changed_variant) {
        context.check_equal(
            changed_variant->outputs().front().path,
            plan->outputs().front().path,
            "same named config keeps the public output path"
        );
        context.check(
            std::ranges::any_of(
                changed_variant->actions(),
                [](const kaixa::Action& action) {
                    return action.description == "reset test_generated";
                }
            ),
            "incompatible CMake state plans a private build reset"
        );
    }

    const auto report = kaixa::test(*plan);
    context.check(report.has_value(), "generated project configures, builds and tests");
    if (!report)
        context.fail(kaixa::format_diagnostic(report.error()));
    else {
        context.check_equal(report->executed, std::size_t{3}, "configure, build and test execute");

        const auto configured_plan = kaixa::plan_build(*graph, registry, environment);
        context.check(configured_plan.has_value(), "configured project plans again");
        if (configured_plan) {
            const auto state = kaixa::check(*configured_plan);
            context.check(state.has_value(), "configured project can be checked");
            if (state) {
                context.check(
                    state->actions.front().state == kaixa::ActionState::current,
                    "CMake File API reports configuration as current"
                );
            }
        }

        std::filesystem::last_write_time(
            generated->path,
            std::filesystem::file_time_type::clock::now() + std::chrono::seconds(2)
        );
        const auto stale_plan = kaixa::plan_build(*graph, registry, environment);
        context.check(stale_plan.has_value(), "changed CMake input plans again");
        if (stale_plan) {
            const auto state = kaixa::check(*stale_plan);
            context.check(state.has_value(), "changed CMake input can be checked");
            if (state) {
                context.check(
                    state->actions.front().state == kaixa::ActionState::required,
                    "CMake File API detects configuration input changes"
                );
            }
        }
    }
}

KAIXA_TEST(generated_project_refuses_to_replace_a_manual_cmakelists) {
    const kaixa::testing::TempDirectory workspace("manual-cmakelists");
    kaixa::Manifest manifest{"manual", "cmake"};
    manifest.resolver_options = kaixa::Value::table({
        {"target", kaixa::Value::table({
            {"type", "executable"},
            {"sources", kaixa::Value::array({"main.cpp"})}
        })}
    });
    workspace.write_manifest("Kaixa.toml", manifest);
    workspace.write("main.cpp", "int main() { return 0; }\n");
    workspace.write("CMakeLists.txt", "# This file belongs to the user.\n");

    const auto graph = kaixa::load_workspace(workspace.path());
    if (!graph) {
        context.fail(kaixa::format_diagnostic(graph.error()));
        return;
    }

    const kaixa::ExtensionRegistry registry = kaixa::plugin::default_registry();
    const kaixa::BuildEnvironment environment{
        workspace.path(),
        workspace.path() / ".kaixa",
        "debug"
    };
    const auto plan = kaixa::plan_build(*graph, registry, environment);
    context.check(!plan.has_value(), "manual CMakeLists.txt is protected");
    if (!plan) {
        context.check_contains(
            kaixa::format_diagnostic(plan.error()),
            "was not generated by Kaixa",
            "diagnostic explains the ownership rule"
        );
    }
}

KAIXA_TEST(source_generated_project_declares_its_cmakelists_for_cleaning) {
    const kaixa::testing::TempDirectory workspace("source-generated-clean");
    kaixa::Manifest manifest{"source_generated", "cmake"};
    manifest.resolver_options = kaixa::Value::table({
        {"generation", "source"},
        {"target", kaixa::Value::table({
            {"type", "executable"},
            {"sources", kaixa::Value::array({"main.cpp"})}
        })}
    });
    workspace.write_manifest("Kaixa.toml", manifest);
    workspace.write("main.cpp", "int main() { return 0; }\n");

    const auto graph = kaixa::load_workspace(workspace.path());
    context.check(graph.has_value(), "source generated workspace loads");
    if (!graph)
        return;

    const kaixa::ExtensionRegistry registry = kaixa::plugin::default_registry();
    const kaixa::BuildEnvironment environment{
        workspace.path(),
        workspace.path() / ".kaixa",
        "debug"
    };
    const auto plan = kaixa::plan_clean(
        *graph,
        registry,
        environment,
        kaixa::CleanRequest{true}
    );
    context.check(plan.has_value(), "source generation clean plans");
    if (plan) {
        const std::filesystem::path cmakelists = (*graph)[graph->roots().front()].directory / "CMakeLists.txt";
        context.check(
            std::ranges::any_of(
                plan->generated_files(),
                [&](const kaixa::GeneratedCleanFile& generated) {
                    return generated.path == cmakelists;
                }
            ),
            "source CMakeLists is explicitly generated"
        );
    }
}

KAIXA_TEST(package_targets_can_be_inline_or_split_into_manifests) {
    const kaixa::testing::TempDirectory workspace("package-targets");
    workspace.write(
        "Kaixa.toml",
        "[package]\n"
        "name = \"app\"\n"
        "resolver = \"cmake\"\n"
        "tests = [\"tests\"]\n"
        "examples = [\"examples/*\"]\n"
        "benchmarks = [\"benchmarks\"]\n"
        "\n"
        "[example]\n"
        "sources = [\"inline.cpp\"]\n"
    );
    workspace.write("inline.cpp", "int main() { return 0; }\n");
    workspace.write(
        "tests/Kaixa.toml",
        "[test]\n"
        "sources = [\"*.cpp\"]\n"
        "\n"
        "[test.dependencies]\n"
        "test_support = { path = \"../test_support\" }\n"
        "\n"
        "[test.cmake]\n"
        "link-libraries = [\"test_support\"]\n"
    );
    workspace.write("tests/main.cpp", "int main() { return 0; }\n");
    workspace.write(
        "test_support/Kaixa.toml",
        "[package]\n"
        "name = \"test_support\"\n"
        "resolver = \"cmake\"\n"
        "\n"
        "[cmake]\n"
        "type = \"static-library\"\n"
        "sources = [\"support.cpp\"]\n"
    );
    workspace.write("test_support/support.cpp", "int test_support() { return 1; }\n");
    workspace.write(
        "examples/rendering/Kaixa.toml",
        "[examples]\n"
        "sources = [\"*.cpp\"]\n"
        "category = \"Rendering\"\n"
        "\n"
        "[examples.dependencies]\n"
        "test_support = { path = \"../../test_support\" }\n"
        "\n"
        "[examples.cmake]\n"
        "link-libraries = [\"test_support\"]\n"
    );
    workspace.write("examples/rendering/first.cpp", "int main() { return 0; }\n");
    workspace.write("examples/rendering/second.cpp", "int main() { return 0; }\n");
    workspace.write(
        "benchmarks/Kaixa.toml",
        "[benchmark]\n"
        "sources = [\"*.cpp\"]\n"
        "\n"
        "[benchmark.dependencies]\n"
        "test_support = { path = \"../test_support\" }\n"
        "\n"
        "[benchmark.cmake]\n"
        "link-libraries = [\"test_support\"]\n"
    );
    workspace.write("benchmarks/main.cpp", "int main() { return 0; }\n");

    const auto graph = kaixa::load_workspace(workspace.path());
    context.check(graph.has_value(), "workspace with package targets loads");
    if (!graph) {
        context.fail(kaixa::format_diagnostic(graph.error()));
        return;
    }

    const kaixa::Manifest& manifest = *(*graph)[graph->roots().front()].manifest;
    context.check_equal(manifest.targets.size(), std::size_t{1}, "inline declaration is preserved");
    context.check_equal(manifest.resolved_targets.size(), std::size_t{5}, "targets are normalized");
    context.check(
        std::ranges::all_of(manifest.resolved_targets, [](const kaixa::PackageTarget& target) {
            return target.name.has_value() && !target.each_source && !target.sources.files.empty();
        }),
        "resolver receives concrete named targets"
    );
    const kaixa::PackageNode& root = (*graph)[graph->roots().front()];
    context.check_equal(root.target_dependencies.size(), std::size_t{4}, "target dependency group count");
    context.check_equal(
        (*graph)[root.target_dependencies.front().packages.front()].name,
        std::string("test_support"),
        "test dependency resolves independently from package dependencies"
    );

    const kaixa::ExtensionRegistry registry = kaixa::plugin::default_registry();
    const kaixa::BuildEnvironment environment{
        workspace.path(),
        workspace.path() / ".kaixa",
        "debug"
    };
    const auto plan = kaixa::plan_build(*graph, registry, environment);
    context.check(plan.has_value(), "package targets plan through CMake");
    if (!plan) {
        context.fail(kaixa::format_diagnostic(plan.error()));
        return;
    }

    const auto generated = std::ranges::find_if(
        plan->generated_files(),
        [&](const kaixa::GeneratedFile& file) {
            return file.path == workspace.path() / "CMakeLists.txt";
        }
    );
    context.check(generated != plan->generated_files().end(), "source CMakeLists is generated");
    if (generated == plan->generated_files().end())
        return;

    context.check_contains(generated->content, "add_executable(app_example", "inline example target");
    context.check_contains(generated->content, "add_executable(app_tests", "external test target");
    context.check_contains(
        generated->content,
        "add_executable(app_example_first",
        "first discovered example target"
    );
    context.check_contains(
        generated->content,
        "add_executable(app_example_second",
        "second discovered example target"
    );
    context.check_contains(generated->content, "add_executable(app_benchmarks", "benchmark target");
    context.check_contains(generated->content, "add_test(NAME [[app_tests]]", "test registration");
    context.check_contains(
        generated->content,
        "set_target_properties(app_tests PROPERTIES EXCLUDE_FROM_ALL TRUE)",
        "package targets stay outside the default build"
    );
    const auto dependencies = std::ranges::find_if(
        plan->generated_files(),
        [](const kaixa::GeneratedFile& file) {
            return file.path.filename() == "dependencies.cmake";
        }
    );
    context.check(dependencies != plan->generated_files().end(), "CMake dependency integration is generated");
    if (dependencies != plan->generated_files().end()) {
        context.check_contains(dependencies->content, "test_support", "test dependency is integrated");
        context.check_contains(
            dependencies->content,
            "EXCLUDE_FROM_ALL",
            "test dependency stays outside the default build"
        );
    }

    const auto synchronization = kaixa::generate(*plan);
    context.check(synchronization.has_value(), "package targets configure");
    if (!synchronization) {
        context.fail(kaixa::format_diagnostic(synchronization.error()));
        return;
    }

    const auto run_targets = kaixa::discover_run_targets(*graph, registry, environment);
    context.check(run_targets.has_value(), "official examples are discovered for running");
    if (run_targets) {
        context.check_equal(run_targets->size(), std::size_t{3}, "only examples are runnable");
        context.check(
            std::ranges::all_of(*run_targets, [](const kaixa::RunTarget& target) {
                return target.purpose == kaixa::ProductPurpose::example;
            }),
            "runnable examples keep their semantic purpose"
        );
        context.check(
            std::ranges::none_of(*run_targets, [](const kaixa::RunTarget& target) {
                return target.name == "app_tests" || target.name == "app_benchmarks";
            }),
            "tests and benchmarks stay out of run targets"
        );
    }

    const auto test_plan = kaixa::plan_tests(*graph, registry, environment, {});
    context.check(test_plan.has_value(), "official tests plan");
    if (test_plan) {
        const auto build = std::ranges::find_if(
            test_plan->actions(),
            [](const kaixa::Action& action) {
                return action.stage == kaixa::ActionStage::build;
            }
        );
        context.check(build != test_plan->actions().end(), "test build action exists");
        if (build != test_plan->actions().end()) {
            context.check(
                std::ranges::find(build->argv, "app_tests") != build->argv.end(),
                "test command builds the excluded test target"
            );
        }

        const auto tested = kaixa::test(*test_plan);
        context.check(tested.has_value(), "test target builds with its specific dependency");
        if (!tested)
            context.fail(kaixa::format_diagnostic(tested.error()));
    }
}
