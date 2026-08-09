#pragma once

#include <kaixa/model/manifest.hpp>

#include <cstddef>
#include <filesystem>
#include <iosfwd>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace kaixa::testing {
    class TestContext {
    public:
        TestContext(std::string_view name, std::ostream& out);

        void check(bool condition, std::string_view what);

        template<typename Actual, typename Expected>
        void check_equal(const Actual& actual, const Expected& expected, std::string_view what) {
            if (actual == expected)
                return;

            std::ostringstream detail;
            detail << what << " (actual: " << actual << ", expected: " << expected << ')';
            fail(detail.str());
        }

        void check_contains(
            std::string_view haystack,
            std::string_view needle,
            std::string_view what
        );
        void fail(std::string_view what);

        [[nodiscard]] std::size_t failures() const noexcept { return m_failures; }

    private:
        std::string m_name;
        std::ostream* m_out;
        std::size_t m_failures = 0;
    };

    using TestFunction = void (*)(TestContext&);

    class TestRegistry {
    public:
        static TestRegistry& instance();

        bool add(std::string_view name, TestFunction function);
        [[nodiscard]] int run_all(std::ostream& out);

    private:
        struct Entry {
            std::string name;
            TestFunction function;
        };

        std::vector<Entry> m_entries;
    };

    inline constexpr std::string_view echo_flag = "--kaixa-test-echo";
    [[nodiscard]] int run_echo_mode(int argc, char** argv);

    class TempDirectory {
    public:
        explicit TempDirectory(std::string_view label);

        TempDirectory(const TempDirectory&) = delete;
        TempDirectory& operator=(const TempDirectory&) = delete;
        TempDirectory(TempDirectory&&) = delete;
        TempDirectory& operator=(TempDirectory&&) = delete;

        ~TempDirectory();

        [[nodiscard]] const std::filesystem::path& path() const noexcept { return m_path; }
        void copy_from(const std::filesystem::path& source) const;
        void write_manifest(std::string_view relative, const Manifest& manifest) const;
        void write(std::string_view relative, std::string_view content) const;

    private:
        std::filesystem::path m_path;
    };
}

#define KAIXA_TEST(name)                                           \
    static void name(kaixa::testing::TestContext& context);        \
    static const bool kaixa_registered_##name =                    \
        kaixa::testing::TestRegistry::instance().add(#name, name); \
    static void name(kaixa::testing::TestContext& context)
