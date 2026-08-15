#include <kaixa/plugin/bundle.hpp>

#include <kaixa/plugin/cmake/resolver.hpp>
#include <kaixa/plugin/path/source.hpp>

namespace kaixa::plugin {
    ExtensionRegistry default_registry() {
        ExtensionRegistry registry;
        registry.add(cmake::make_resolver());
        registry.add(path::make_source_driver());
        return registry;
    }
}
