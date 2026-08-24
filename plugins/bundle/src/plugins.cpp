#include <kaixa/plugin/bundle.hpp>

#include <kaixa/plugin/cmake/resolver.hpp>
#include <kaixa/plugin/path/provider.hpp>
#include <kaixa/plugin/path/source.hpp>
#include <kaixa/plugin/registry/provider.hpp>
#include <kaixa/plugin/remote/source.hpp>

namespace kaixa::plugin {
    ExtensionRegistry default_registry() {
        ExtensionRegistry registry;
        registry.add(cmake::make_resolver());
        registry.add(path::make_source_driver());
        registry.add(remote::make_git_source_driver());
        registry.add(remote::make_url_source_driver());
        registry.add(remote::make_archive_source_driver());
        registry.add(path::make_provider_driver());
        registry.add(path::make_package_map_provider_driver());
        registry.add(path::make_system_packages_provider_driver());
        registry.add(kaixa::plugin::registry::make_provider_driver());
        add_standard_test_adapters(registry);
        return registry;
    }
}
