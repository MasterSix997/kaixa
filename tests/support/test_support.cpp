#include <test_support.hpp>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <ranges>
#include <stdexcept>
#include <system_error>

namespace kaixa::testing {
    namespace {
        std::string unique_name(const std::string_view label) {
            static std::atomic<unsigned> counter{0};
            const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();

            return "kaixa-test-" + std::string(label) + '-'
                + std::to_string(static_cast<unsigned long long>(ticks)) + '-'
                + std::to_string(counter.fetch_add(1));
        }
    }

    TestContext::TestContext(const std::string_view name, std::ostream& out)
        : m_name(name), m_out(&out) {
    }

    void TestContext::check(const bool condition, const std::string_view what) {
        if (!condition)
            fail(what);
    }

    void TestContext::check_contains(
        const std::string_view haystack,
        const std::string_view needle,
        const std::string_view what
    ) {
        if (haystack.contains(needle))
            return;

        std::ostringstream detail;
        detail << what << " (missing `" << needle << "` in: " << haystack << ')';
        fail(detail.str());
    }

    void TestContext::fail(const std::string_view what) {
        ++m_failures;
        *m_out << "FAIL [" << m_name << "] " << what << '\n';
    }

    TestRegistry& TestRegistry::instance() {
        static TestRegistry registry;
        return registry;
    }

    bool TestRegistry::add(const std::string_view name, const TestFunction function) {
        m_entries.push_back({std::string(name), function});
        return true;
    }

    void TestRegistry::list(std::ostream& out) const {
        for (const Entry& entry: m_entries)
            out << entry.name << '\n';
    }

    int TestRegistry::run(const std::string_view name, std::ostream& out) const {
        const auto entry = std::ranges::find(m_entries, name, &Entry::name);
        if (entry == m_entries.end()) {
            out << "unknown test: " << name << '\n';
            return 2;
        }

        TestContext context(entry->name, out);
        try {
            entry->function(context);
        } catch (const std::exception& exception) {
            context.fail(std::string("unexpected exception: ") + exception.what());
        } catch (...) {
            context.fail("unexpected non-standard exception");
        }

        return context.failures() == 0 ? 0 : 1;
    }

    int TestRegistry::run_all(std::ostream& out) {
        std::size_t failed_cases = 0;
        for (const Entry& entry: m_entries) {
            TestContext context(entry.name, out);
            try {
                entry.function(context);
            } catch (const std::exception& exception) {
                context.fail(std::string("unexpected exception: ") + exception.what());
            } catch (...) {
                context.fail("unexpected non-standard exception");
            }

            if (context.failures() != 0)
                ++failed_cases;
        }

        if (failed_cases == 0) {
            out << m_entries.size() << " tests passed\n";
            return 0;
        }

        out << failed_cases << " of " << m_entries.size() << " tests failed\n";
        return static_cast<int>(failed_cases);
    }

    int run_echo_mode(const int argc, char** argv) {
        const int exit_code = argc >= 3 ? std::atoi(argv[2]) : 0;
        for (int index = 3; index < argc; ++index)
            std::cout << argv[index] << '\n';
        return exit_code;
    }

    TempDirectory::TempDirectory(const std::string_view label) {
        std::error_code failure;
        const std::filesystem::path temporary = std::filesystem::temp_directory_path(failure);
        if (failure)
            throw std::runtime_error("cannot locate the temporary directory: " + failure.message());

        m_path = temporary / unique_name(label);
        std::filesystem::create_directories(m_path, failure);
        if (failure)
            throw std::runtime_error("cannot create test directory: " + failure.message());
    }

    TempDirectory::~TempDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(m_path, ignored);
    }

    void TempDirectory::copy_from(const std::filesystem::path& source) const {
        std::error_code failure;
        for (std::filesystem::recursive_directory_iterator iterator(source, failure), end;
             iterator != end;
             iterator.increment(failure)) {
            if (failure)
                throw std::runtime_error("cannot copy test fixture: " + failure.message());

            const std::filesystem::path relative = std::filesystem::relative(
                iterator->path(),
                source,
                failure
            );
            if (failure)
                throw std::runtime_error("cannot resolve test fixture path: " + failure.message());
            const std::filesystem::path destination = m_path / relative;

            if (iterator->is_directory(failure)) {
                std::filesystem::create_directories(destination, failure);
            } else if (iterator->is_regular_file(failure)) {
                std::filesystem::create_directories(destination.parent_path(), failure);
                if (!failure) {
                    std::filesystem::copy_file(
                        iterator->path(),
                        destination,
                        std::filesystem::copy_options::overwrite_existing,
                        failure
                    );
                }
            }
            if (failure)
                throw std::runtime_error("cannot copy test fixture: " + failure.message());
        }
        if (failure)
            throw std::runtime_error("cannot inspect test fixture: " + failure.message());
    }

    void TempDirectory::write_manifest(
        const std::string_view relative,
        const Manifest& manifest
    ) const {
        const auto written = write_manifest_file(m_path / relative, manifest);
        if (!written)
            throw std::runtime_error(format_diagnostic(written.error()));
    }

    void TempDirectory::write(
        const std::string_view relative,
        const std::string_view content
    ) const {
        const std::filesystem::path target = m_path / relative;
        std::error_code failure;
        std::filesystem::create_directories(target.parent_path(), failure);
        if (failure)
            throw std::runtime_error("cannot create fixture directory: " + failure.message());

        std::ofstream file(target, std::ios::binary | std::ios::trunc);
        if (!file)
            throw std::runtime_error("cannot open fixture `" + target.string() + "`");
        file.write(content.data(), static_cast<std::streamsize>(content.size()));
        if (!file)
            throw std::runtime_error("cannot write fixture `" + target.string() + "`");
    }
}
