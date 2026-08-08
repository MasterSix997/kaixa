#include <kaixa/plugin/bundle.hpp>

#include <kaixa/plugin/cmake/resolver.hpp>

namespace kaixa::plugin {
    ResolverRegistry default_registry() {
        ResolverRegistry registry;
        registry.add(cmake::make_resolver());
        return registry;
    }
}
