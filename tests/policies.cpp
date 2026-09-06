#include <test_support.hpp>

#include <kaixa/kaixa.hpp>
#include <kaixa/plugin/bundle.hpp>

#include <algorithm>
#include <string>
#include <vector>

namespace {
    const kaixa::PolicySchema& native_schema() {
        static const kaixa::ExtensionRegistry registry = kaixa::plugin::default_registry();
        return registry.policy_schema();
    }

    kaixa::Value policy(std::initializer_list<kaixa::TableEntry> entries) {
        return kaixa::Value::table(std::vector<kaixa::TableEntry>(entries));
    }

    kaixa::PackageId add_managed(kaixa::Graph& graph, std::string name, kaixa::Value package_policy) {
        kaixa::PackageNode node;
        node.name = std::move(name);
        node.directory = node.name;
        node.resolver = "cmake";
        node.semantics = kaixa::ManagedPackage{kaixa::Manifest{node.name, node.resolver}};
        node.policy_layers.push_back(std::move(package_policy));
        return graph.add(std::move(node));
    }

    const kaixa::ConfiguredPackageInstance* find_context(
        const std::vector<kaixa::ConfiguredPackageInstance>& instances,
        const kaixa::PackageId package,
        const std::string_view context
    ) {
        const auto found = std::ranges::find_if(instances, [&](const kaixa::ConfiguredPackageInstance& instance) {
            return instance.package == package && std::ranges::find(instance.contexts, context) != instance.contexts.end();
        });
        return found == instances.end() ? nullptr : &*found;
    }
}

KAIXA_TEST(policy_conditions_use_the_build_context_and_keep_classes) {
    const kaixa::Value conditional = policy(
        {{"warnings", "strict"},
            {"cxx", 20},
            {"when",
                kaixa::Value::array(
                    {policy(
                        {{"if", policy({{"profile", "release"}, {"target-os", "windows"}})},
                            {"warnings", "pedantic"},
                            {"defines", policy({{"NOMINMAX", true}})}}
                    )}
                )}}
    );

    const std::vector<kaixa::Value> layers{conditional};
    const auto effective = kaixa::resolve_policy_layers(layers, {}, {"release", "windows"}, native_schema());
    context.check(effective.has_value(), "conditional policy resolves");
    if (!effective) {
        context.fail(kaixa::format_diagnostic(effective.error()));
        return;
    }

    context.check_equal(*effective->find("warnings")->value.as_string(), std::string("pedantic"), "matching branch replaces local policy");
    context.check(effective->find("warnings")->classification == kaixa::PolicyClass::local, "warnings remain local");
    context.check(effective->find("cxx")->classification == kaixa::PolicyClass::floor, "C++ is a floor policy");
    context.check(effective->find("profile")->classification == kaixa::PolicyClass::abi, "profile is ABI policy");
    context.check(effective->find("defines") != nullptr, "conditional definitions are effective");
}

KAIXA_TEST(abi_policy_conflicts_are_reported_per_final_artifact) {
    kaixa::Graph graph;
    const kaixa::PackageId root = add_managed(graph, "root", policy({}));
    const kaixa::PackageId left = add_managed(graph, "left", policy({{"exceptions", true}}));
    const kaixa::PackageId right = add_managed(graph, "right", policy({{"exceptions", false}}));
    graph[root].dependencies = {left, right};
    graph.add_root(root);

    const auto instances = kaixa::configure_package_instances(graph, {}, native_schema());
    context.check(!instances.has_value(), "incompatible ABI requirements fail");
    if (instances)
        return;

    const std::string diagnostic = kaixa::format_diagnostic(instances.error());
    context.check_contains(diagnostic, "ABI policy conflict", "diagnostic identifies the policy class conflict");
    context.check_contains(diagnostic, "exceptions", "diagnostic identifies the policy key");
    context.check_contains(diagnostic, "root:default", "diagnostic identifies the final artifact context");
}

KAIXA_TEST(floor_policy_is_raised_across_an_artifact_closure) {
    kaixa::Graph graph;
    const kaixa::PackageId root = add_managed(graph, "root", policy({{"cxx", 20}}));
    const kaixa::PackageId dependency = add_managed(graph, "dependency", policy({{"cxx", 23}}));
    graph[root].dependencies = {dependency};
    kaixa::DependencyBinding binding;
    binding.request.package = "dependency";
    binding.visibility = kaixa::DependencyVisibility::public_dependency;
    graph[root].manifest()->dependencies.push_back(std::move(binding));
    graph.add_root(root);

    const auto instances = kaixa::configure_package_instances(graph, {}, native_schema());
    context.check(instances.has_value(), "compatible floor policies configure");
    if (!instances) {
        context.fail(kaixa::format_diagnostic(instances.error()));
        return;
    }

    const kaixa::ConfiguredPackageInstance* configured = find_context(*instances, root, "root:default");
    context.check(configured != nullptr, "root configured instance exists");
    if (configured)
        context.check_equal(*configured->policy.find("cxx")->value.as_integer(), std::int64_t{23}, "dependency raises the language floor");
}

KAIXA_TEST(target_abi_policy_creates_dependency_variants_and_stable_identities) {
    kaixa::Graph graph;
    const kaixa::PackageId root = add_managed(graph, "root", policy({}));
    const kaixa::PackageId dependency = add_managed(graph, "dependency", policy({{"exceptions", true}}));
    graph[root].dependencies = {dependency};
    kaixa::PackageTarget target;
    target.name = "root.tests.noexcept";
    target.policy = policy({{"exceptions", false}});
    graph[root].targets.push_back(std::move(target));
    graph.add_root(root);

    const auto first = kaixa::configure_package_instances(graph, {}, native_schema());
    const auto second = kaixa::configure_package_instances(graph, {}, native_schema());
    context.check(first.has_value() && second.has_value(), "target variants configure repeatedly");
    if (!first || !second)
        return;

    std::vector<const kaixa::ConfiguredPackageInstance*> dependency_instances;
    for (const kaixa::ConfiguredPackageInstance& instance: *first) {
        if (instance.package == dependency)
            dependency_instances.push_back(&instance);
    }
    context.check_equal(dependency_instances.size(), std::size_t{2}, "dependency has default and target ABI variants");
    if (dependency_instances.size() == 2) {
        context.check(
            dependency_instances[0]->artifact != dependency_instances[1]->artifact,
            "dependency ABI variants have distinct artifacts"
        );
        context.check(
            std::ranges::any_of(
                dependency_instances,
                [](const kaixa::ConfiguredPackageInstance* instance) {
                    return std::ranges::find(instance->contexts, "root.tests.noexcept") != instance->contexts.end();
                }
            ),
            "target context is retained throughout the configured closure"
        );
    }

    context.check_equal(first->size(), second->size(), "repeat configuration has the same instance count");
    if (first->size() == second->size()) {
        for (std::size_t index = 0; index < first->size(); ++index) {
            context.check_equal((*first)[index].artifact, (*second)[index].artifact, "configured artifact identity is stable");
        }
    }
}

KAIXA_TEST(prebuilt_descriptor_abi_is_validated_against_the_artifact) {
    kaixa::Graph graph;
    const kaixa::PackageId root = add_managed(graph, "root", policy({{"exceptions", false}}));
    kaixa::PackageNode prebuilt;
    prebuilt.name = "prebuilt";
    prebuilt.semantics = kaixa::OpaquePackage{policy({{"abi", policy({{"exceptions", true}})}})};
    const kaixa::PackageId dependency = graph.add(std::move(prebuilt));
    graph[root].dependencies.push_back(dependency);
    graph.add_root(root);

    const auto instances = kaixa::configure_package_instances(graph, {}, native_schema());
    context.check(!instances.has_value(), "prebuilt ABI mismatch fails");
    if (instances)
        return;

    const std::string diagnostic = kaixa::format_diagnostic(instances.error());
    context.check_contains(diagnostic, "package `prebuilt` declares true", "diagnostic identifies the prebuilt declaration");
    context.check_contains(diagnostic, "requested false", "diagnostic identifies the requested ABI value");
}

KAIXA_TEST(cmake_translates_effective_policy_to_target_configuration) {
    const kaixa::testing::TempDirectory workspace("cmake-policy-translation");
    workspace.write(
        "Kaixa.toml",
        "[package]\n"
        "name = \"policy_app\"\n"
        "resolver = \"cmake\"\n"
        "\n"
        "[package-set]\n"
        "\n"
        "[package-set.policy]\n"
        "cxx = 23\n"
        "warnings = \"pedantic\"\n"
        "warnings-as-errors = true\n"
        "exceptions = false\n"
        "rtti = false\n"
        "sanitizers = [\"address\"]\n"
        "precompiled-headers = [\"pch.hpp\"]\n"
        "msvc-runtime = \"dynamic\"\n"
        "defines = { POLICY_ACTIVE = true }\n"
        "\n"
        "[lib]\n"
        "type = \"static\"\n"
        "sources = [\"app.cpp\"]\n"
    );
    workspace.write("app.cpp", "int answer() { return 42; }\n");
    workspace.write("pch.hpp", "#pragma once\n");

    kaixa::ExtensionRegistry extensions = kaixa::plugin::default_registry();
    const auto graph = kaixa::load_workspace(workspace.path(), &extensions);
    context.check(graph.has_value(), "policy workspace loads");
    if (!graph) {
        context.fail(kaixa::format_diagnostic(graph.error()));
        return;
    }

    const kaixa::ExtensionRegistry registry = kaixa::plugin::default_registry();
    const kaixa::BuildEnvironment environment{workspace.path(), workspace.path() / ".kaixa", "debug"};
    const auto plan = kaixa::plan_build(*graph, registry, environment);
    context.check(plan.has_value(), "policy workspace plans");
    if (!plan) {
        context.fail(kaixa::format_diagnostic(plan.error()));
        return;
    }

    const auto generated = std::ranges::find_if(plan->generated_files(), [](const kaixa::GeneratedFile& file) {
        return file.path.filename() == "CMakeLists.txt";
    });
    context.check(generated != plan->generated_files().end(), "CMake project is generated");
    if (generated == plan->generated_files().end())
        return;

    context.check_contains(generated->content, "cxx_std_23", "language floor reaches CMake");
    context.check_contains(generated->content, "-Wpedantic", "warning policy reaches CMake");
    context.check_contains(generated->content, "-Werror", "warnings-as-errors reaches CMake");
    context.check_contains(generated->content, "-fno-exceptions", "exception policy reaches CMake");
    context.check_contains(generated->content, "-fno-rtti", "RTTI policy reaches CMake");
    context.check_contains(generated->content, "-fsanitize=address", "sanitizer reaches compile and link options");
    context.check_contains(generated->content, "target_link_options", "sanitizer link option is emitted");
    context.check_contains(generated->content, "target_precompile_headers", "precompiled headers are emitted");
    context.check_contains(generated->content, "POLICY_ACTIVE", "policy definitions are emitted");
    context.check_contains(generated->content, "MSVC_RUNTIME_LIBRARY", "runtime policy is emitted");
}

KAIXA_TEST(cmake_plans_default_and_target_abi_instances_in_separate_projects) {
    const kaixa::testing::TempDirectory workspace("cmake-configured-routes");
    workspace.write(
        "Kaixa.toml",
        "[package-set]\n"
        "members = [\"app\", \"dependency\"]\n"
        "default = [\"app\"]\n"
        "\n"
        "[package-set.policy]\n"
        "cxx = 23\n"
        "exceptions = true\n"
    );
    workspace.write(
        "app/Kaixa.toml",
        "[package]\n"
        "name = \"app\"\n"
        "version = \"0.1.0\"\n"
        "resolver = \"cmake\"\n"
        "\n"
        "[dependencies]\n"
        "dependency = \"0.1.0\"\n"
        "\n"
        "[lib]\n"
        "type = \"static\"\n"
        "sources = [\"app.cpp\"]\n"
        "\n"
        "[[test]]\n"
        "name = \"app.tests.noexcept\"\n"
        "sources = [\"test.cpp\"]\n"
        "policy = { exceptions = false }\n"
        "\n"
        "[cmake]\n"
        "generation = \"state\"\n"
    );
    workspace.write("app/app.cpp", "int answer() { return 42; }\n");
    workspace.write("app/test.cpp", "int main() { return 0; }\n");
    workspace.write(
        "dependency/Kaixa.toml",
        "[package]\n"
        "name = \"dependency\"\n"
        "version = \"0.1.0\"\n"
        "resolver = \"cmake\"\n"
        "\n"
        "[lib]\n"
        "type = \"static\"\n"
        "sources = [\"dependency.cpp\"]\n"
        "\n"
        "[cmake]\n"
        "generation = \"state\"\n"
    );
    workspace.write("dependency/dependency.cpp", "int dependency() { return 1; }\n");

    kaixa::ExtensionRegistry extensions = kaixa::plugin::default_registry();
    const auto graph = kaixa::load_workspace(workspace.path(), &extensions);
    context.check(graph.has_value(), "configured-route workspace loads");
    if (!graph) {
        context.fail(kaixa::format_diagnostic(graph.error()));
        return;
    }

    const kaixa::PackageId root = graph->roots().front();
    kaixa::BuildRequest request;
    request.packages.push_back({root, {"app.tests.noexcept"}, true});
    const kaixa::ExtensionRegistry registry = kaixa::plugin::default_registry();
    const kaixa::BuildEnvironment environment{workspace.path(), workspace.path() / ".kaixa", "debug"};
    const auto plan = kaixa::plan_build(*graph, registry, environment, request);
    context.check(plan.has_value(), "default and ABI target routes plan together");
    if (!plan) {
        context.fail(kaixa::format_diagnostic(plan.error()));
        return;
    }

    std::vector<const kaixa::Action*> configure_actions;
    for (const kaixa::Action& action: plan->synchronization()) {
        if (action.package == root && action.description == "configure app")
            configure_actions.push_back(&action);
    }
    context.check_equal(configure_actions.size(), std::size_t{2}, "each root ABI instance has a configure action");
    if (configure_actions.size() == 2) {
        context.check(
            configure_actions[0]->configured_artifact != configure_actions[1]->configured_artifact,
            "configure actions retain distinct stable artifacts"
        );
        context.check(
            configure_actions[0]->outputs.front() != configure_actions[1]->outputs.front(),
            "configured instances use separate CMake build trees"
        );
    }

    std::vector<const kaixa::GeneratedFile*> dependency_projects;
    for (const kaixa::GeneratedFile& generated: plan->generated_files()) {
        if (generated.path.filename() == "CMakeLists.txt"
            && generated.path.parent_path().filename() == "project"
            && generated.path.parent_path().parent_path().filename() == "dependency") {
            dependency_projects.push_back(&generated);
        }
    }
    context.check_equal(dependency_projects.size(), std::size_t{2}, "dependency is generated once per ABI route");
    if (dependency_projects.size() == 2) {
        const bool first_noexcept = dependency_projects[0]->content.contains("-fno-exceptions");
        const bool second_noexcept = dependency_projects[1]->content.contains("-fno-exceptions");
        context.check(first_noexcept != second_noexcept, "only one dependency project receives the no-exceptions ABI policy");
    }

    const auto test_plan = kaixa::plan_tests(*graph, registry, environment, {});
    context.check(test_plan.has_value(), "test planning routes the policy-specific executable");
    if (!test_plan) {
        context.fail(kaixa::format_diagnostic(test_plan.error()));
        return;
    }

    const std::span<const kaixa::Action> test_actions = test_plan->tests();
    const auto test_action = test_actions.begin();
    context.check(!test_actions.empty(), "policy-specific CTest action exists");
    if (!test_actions.empty()) {
        context.check(test_action->configured_artifact.has_value(), "CTest action retains its configured artifact");
        context.check(
            std::ranges::find(test_action->argv, "^kaixa\\.target:app\\.tests\\.noexcept$") != test_action->argv.end(),
            "CTest action is restricted to the policy route"
        );
        context.check(test_action->argv[2].contains("instances"), "CTest action executes in the isolated configured build tree");
    }

    const auto executed = kaixa::test(*test_plan);
    context.check(executed.has_value(), "default and no-exceptions routes configure, build and test successfully");
    if (!executed)
        context.fail(kaixa::format_diagnostic(executed.error()));
}

KAIXA_TEST(package_policy_reaches_adopted_dependency_projects) {
    const kaixa::testing::TempDirectory workspace("dependency-policy");
    workspace.write(
        "Kaixa.toml",
        "[package]\n"
        "name = \"policy_consumer\"\n"
        "resolver = \"cmake\"\n"
        "\n"
        "[package-set]\n"
        "\n"
        "[package-set.policy]\n"
        "cxx = 23\n"
        "msvc-runtime = \"static\"\n"
        "\n"
        "[lib]\n"
        "type = \"static\"\n"
        "sources = [\"app.cpp\"]\n"
        "\n"
        "[dependencies]\n"
        "vendor = { path = \"vendor\" }\n"
    );
    workspace.write("app.cpp", "int answer() { return 42; }\n");
    workspace.write(
        "vendor/Kaixa.toml",
        "[package]\n"
        "name = \"vendor\"\n"
        "version = \"1.0.0\"\n"
        "resolver = \"cmake\"\n"
    );
    workspace.write(
        "vendor/CMakeLists.txt",
        "cmake_minimum_required(VERSION 3.20)\n"
        "project(vendor LANGUAGES CXX)\n"
        "if(NOT CMAKE_CXX_STANDARD EQUAL 23)\n"
        "  message(FATAL_ERROR \"expected C++23 from the consuming package policy\")\n"
        "endif()\n"
        "if(MSVC AND NOT CMAKE_MSVC_RUNTIME_LIBRARY MATCHES \"^MultiThreaded\")\n"
        "  message(FATAL_ERROR \"expected the static MSVC runtime from the consuming package policy\")\n"
        "endif()\n"
        "add_library(vendor INTERFACE)\n"
        "install(TARGETS vendor EXPORT policy_consumerTargets)\n"
    );

    kaixa::ExtensionRegistry extensions = kaixa::plugin::default_registry();
    const auto graph = kaixa::load_workspace(workspace.path(), &extensions);
    context.check(graph.has_value(), "dependency policy workspace loads");
    if (!graph) {
        context.fail(kaixa::format_diagnostic(graph.error()));
        return;
    }

    const kaixa::ExtensionRegistry registry = kaixa::plugin::default_registry();
    const kaixa::BuildEnvironment environment{workspace.path(), workspace.path() / ".kaixa", "debug"};
    const auto plan = kaixa::plan_build(*graph, registry, environment);
    context.check(plan.has_value(), "dependency policy workspace plans");
    if (!plan) {
        context.fail(kaixa::format_diagnostic(plan.error()));
        return;
    }

    const auto integration = std::ranges::find_if(plan->generated_files(), [](const kaixa::GeneratedFile& file) {
        return file.path.filename() == "dependencies.cmake";
    });
    context.check(integration != plan->generated_files().end(), "dependency integration is generated");
    if (integration == plan->generated_files().end())
        return;

    const std::size_t runtime = integration->content.find("set(CMAKE_MSVC_RUNTIME_LIBRARY");
    const std::size_t standard = integration->content.find("set(CMAKE_CXX_STANDARD 23)");
    const std::size_t adoption = integration->content.find("add_subdirectory(");
    context.check(runtime != std::string::npos, "runtime policy reaches adopted dependencies");
    context.check(standard != std::string::npos, "language floor reaches adopted dependencies");
    context.check(runtime < adoption && standard < adoption, "policy is fixed before dependencies are adopted");

    const auto executed = kaixa::execute(*plan);
    context.check(executed.has_value(), "the adopted project observes the propagated policy while configuring");
    if (!executed)
        context.fail(kaixa::format_diagnostic(executed.error()));
}
