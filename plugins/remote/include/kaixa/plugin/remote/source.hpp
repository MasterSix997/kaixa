#pragma once

#include <kaixa/extension/source.hpp>

#include <memory>

namespace kaixa::plugin::remote {
    [[nodiscard]] std::unique_ptr<SourceDriver> make_git_source_driver();
    [[nodiscard]] std::unique_ptr<SourceDriver> make_url_source_driver();
    [[nodiscard]] std::unique_ptr<SourceDriver> make_archive_source_driver();
}
