#pragma once

#include <kaixa/build/executor.hpp>
#include <kaixa/build/product.hpp>
#include <kaixa/clean/plan.hpp>
#include <kaixa/config/parser.hpp>
#include <kaixa/extension/provider.hpp>
#include <kaixa/extension/registry.hpp>
#include <kaixa/extension/source.hpp>
#include <kaixa/foundation/diagnostic.hpp>
#include <kaixa/foundation/hash.hpp>
#include <kaixa/model/effective_product.hpp>
#include <kaixa/model/file_set.hpp>
#include <kaixa/model/graph.hpp>
#include <kaixa/model/manifest.hpp>
#include <kaixa/model/policy.hpp>
#include <kaixa/package/manager.hpp>
#include <kaixa/services/build_service.hpp>
#include <kaixa/services/clean_service.hpp>
#include <kaixa/services/run_service.hpp>
#include <kaixa/services/task_service.hpp>
#include <kaixa/services/workflow_service.hpp>
#include <kaixa/source/cache.hpp>
#include <kaixa/test/adapter.hpp>
#include <kaixa/workspace/loader.hpp>
#include <kaixa/workspace/package_index.hpp>
#include <kaixa/workspace/resolution_lock.hpp>

#include <string_view>

namespace kaixa {
    [[nodiscard]] std::string_view version() noexcept;
}
