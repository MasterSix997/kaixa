#include <test_support.hpp>

#include <kaixa/foundation/filesystem.hpp>
#include <kaixa/package/archive_backend.hpp>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

using kaixa::testing::TempDirectory;

namespace {
    const kaixa::ArchiveEntry* find_entry(const std::vector<kaixa::ArchiveEntry>& entries, const std::string_view path) {
        const auto found = std::ranges::find(entries, path, &kaixa::ArchiveEntry::path);
        return found == entries.end() ? nullptr : &*found;
    }
}

KAIXA_TEST(archives_round_trip_contents_without_external_tools) {
    const TempDirectory root("archive-round-trip");
    root.write("contents/Kaixa.toml", "[package]\nname = \"archived\"\n");
    root.write("contents/src/main.cpp", "int main() { return 0; }\n");
    root.write("contents/docs/nested/deep/notes.md", "# notes\n");

    const kaixa::ArchiveBackend& backend = kaixa::default_archive_backend();
    const std::filesystem::path archive = root.path() / "package.tar.gz";
    const auto created = backend.create_archive(root.path() / "contents", archive);
    context.check(created.has_value(), "archive is created");
    if (!created) {
        context.fail(kaixa::format_diagnostic(created.error()));
        return;
    }

    const auto entries = backend.list_archive(archive);
    context.check(entries.has_value(), "archive is listed");
    if (!entries) {
        context.fail(kaixa::format_diagnostic(entries.error()));
        return;
    }

    const kaixa::ArchiveEntry* manifest = find_entry(*entries, "Kaixa.toml");
    context.check(manifest != nullptr, "listing reports the manifest");
    if (manifest != nullptr) {
        context.check(manifest->kind == kaixa::ArchiveEntryKind::file, "manifest is listed as a file");
        context.check_equal(manifest->size, std::uintmax_t{28}, "manifest size");
    }
    const kaixa::ArchiveEntry* directory = find_entry(*entries, "src/");
    context.check(directory != nullptr && directory->kind == kaixa::ArchiveEntryKind::directory, "listing reports directories");
    context.check(
        std::ranges::none_of(*entries, [](const kaixa::ArchiveEntry& entry) { return entry.kind == kaixa::ArchiveEntryKind::link; }),
        "no links are produced"
    );

    const std::filesystem::path extracted = root.path() / "extracted";
    const auto opened = backend.extract_archive(archive, extracted);
    context.check(opened.has_value(), "archive is extracted");
    if (!opened) {
        context.fail(kaixa::format_diagnostic(opened.error()));
        return;
    }

    const auto restored = kaixa::read_file(extracted / "docs/nested/deep/notes.md");
    context.check(restored.has_value(), "nested entry is restored");
    if (restored)
        context.check_equal(*restored, std::string("# notes\n"), "nested entry contents survive the round trip");

    const auto source = kaixa::read_file(extracted / "src/main.cpp");
    context.check(source.has_value() && *source == "int main() { return 0; }\n", "source entry contents survive the round trip");
}

KAIXA_TEST(archives_are_deterministic_for_identical_contents) {
    const TempDirectory root("archive-determinism");
    root.write("first/data.txt", "same bytes\n");
    root.write("first/nested/other.txt", "more bytes\n");
    root.write("second/data.txt", "same bytes\n");
    root.write("second/nested/other.txt", "more bytes\n");

    const kaixa::ArchiveBackend& backend = kaixa::default_archive_backend();
    const auto first = backend.create_archive(root.path() / "first", root.path() / "first.tar.gz");
    const auto second = backend.create_archive(root.path() / "second", root.path() / "second.tar.gz");
    context.check(first.has_value() && second.has_value(), "both archives are created");
    if (!first || !second)
        return;

    const auto left = kaixa::read_file(root.path() / "first.tar.gz");
    const auto right = kaixa::read_file(root.path() / "second.tar.gz");
    context.check(left.has_value() && right.has_value(), "both archives are readable");
    if (left && right)
        context.check(*left == *right, "identical contents produce byte-identical archives");
}

KAIXA_TEST(archives_reject_unsupported_compression) {
    const TempDirectory root("archive-unsupported");
    const auto written = kaixa::write_file(root.path() / "source.tar.xz", std::string("\xfd\x37\x7a\x58\x5a\x00", 6));
    context.check(written.has_value(), "fixture is written");
    if (!written)
        return;

    const auto listed = kaixa::default_archive_backend().list_archive(root.path() / "source.tar.xz");
    context.check(!listed.has_value(), "xz archives are rejected");
    if (!listed)
        context.check_contains(listed.error().message, "xz archives are not supported", "unsupported compression diagnostic");
}
