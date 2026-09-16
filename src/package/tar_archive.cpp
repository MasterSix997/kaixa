#include <kaixa/package/archive_backend.hpp>

#include <miniz/miniz.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string_view>
#include <utility>
#include <vector>

namespace kaixa {
    namespace {
        constexpr std::size_t tar_block = 512;
        constexpr std::size_t io_chunk = 64 * 1024;
        constexpr std::size_t tar_name_field = 100;
        constexpr std::size_t tar_prefix_field = 155;

        using Block = std::array<char, tar_block>;

        std::ifstream open_input(const std::filesystem::path& path) {
            std::ifstream input(path, std::ios::binary);
            input.exceptions(std::ios::goodbit);
            return input;
        }

        std::ofstream open_output(const std::filesystem::path& path) {
            std::ofstream output(path, std::ios::binary | std::ios::trunc);
            output.exceptions(std::ios::goodbit);
            return output;
        }

        void write_octal(char* field, const std::size_t width, std::uint64_t value) {
            field[width - 1] = '\0';
            for (std::size_t index = width - 1; index-- > 0;) {
                field[index] = static_cast<char>('0' + (value & 7U));
                value >>= 3U;
            }
        }

        std::uint64_t read_octal(const char* field, const std::size_t width) {
            std::uint64_t value = 0;
            for (std::size_t index = 0; index < width; ++index) {
                const char character = field[index];
                if (character < '0' || character > '7')
                    break;

                value = (value << 3U) + static_cast<std::uint64_t>(character - '0');
            }
            return value;
        }

        void seal_header(Block& header) {
            std::memcpy(header.data() + 257, "ustar\00000", 8);
            std::memset(header.data() + 148, ' ', 8);
            std::uint32_t sum = 0;
            for (const char byte: header)
                sum += static_cast<unsigned char>(byte);

            write_octal(header.data() + 148, 7, sum);
            header[155] = ' ';
        }

        Result<Block> tar_header(const std::string& name, const char type, const std::uint64_t size) {
            Block header{};
            std::string_view remaining(name);
            std::string_view prefix;
            if (remaining.size() > tar_name_field) {
                const std::size_t split = remaining.rfind('/', tar_name_field);
                if (split == std::string_view::npos || remaining.size() - split - 1 > tar_name_field || split > tar_prefix_field) {
                    return std::unexpected(error("archive entry path `" + name + "` is too long for the tar format"));
                }
                prefix = remaining.substr(0, split);
                remaining = remaining.substr(split + 1);
            }
            std::memcpy(header.data(), remaining.data(), remaining.size());
            std::memcpy(header.data() + 345, prefix.data(), prefix.size());
            write_octal(header.data() + 100, 8, type == '5' ? 0755U : 0644U);
            write_octal(header.data() + 108, 8, 0);
            write_octal(header.data() + 116, 8, 0);
            write_octal(header.data() + 124, 12, size);
            write_octal(header.data() + 136, 12, 0);
            header[156] = type;
            seal_header(header);
            return header;
        }

        class GzipSink {
        public:
            explicit GzipSink(std::ofstream& output)
                : m_output(output) {}

            GzipSink(const GzipSink&) = delete;
            GzipSink& operator=(const GzipSink&) = delete;

            ~GzipSink() {
                if (m_started)
                    mz_deflateEnd(&m_stream);
            }

            Result<void> begin() {
                static constexpr std::array<char, 10> gzip_header{'\x1f', '\x8b', '\x08', '\0', '\0', '\0', '\0', '\0', '\0', '\xff'};
                m_output.write(gzip_header.data(), gzip_header.size());
                if (mz_deflateInit2(&m_stream, MZ_BEST_COMPRESSION, MZ_DEFLATED, -MZ_DEFAULT_WINDOW_BITS, 9, MZ_DEFAULT_STRATEGY)
                    != MZ_OK) {
                    return std::unexpected(error("cannot initialize the archive compressor"));
                }
                m_started = true;
                return {};
            }

            Result<void> write(const char* data, const std::size_t size) {
                m_crc = mz_crc32(m_crc, reinterpret_cast<const unsigned char*>(data), size);
                m_size += size;
                return pump(data, size, MZ_NO_FLUSH);
            }

            Result<void> finish() {
                auto flushed = pump(nullptr, 0, MZ_FINISH);
                if (!flushed)
                    return flushed;

                write_trailer(static_cast<std::uint32_t>(m_crc));
                write_trailer(static_cast<std::uint32_t>(m_size & 0xffffffffU));
                if (!m_output)
                    return std::unexpected(error("cannot write the compressed archive"));

                return {};
            }

        private:
            void write_trailer(const std::uint32_t value) {
                const std::array<char, 4> bytes{static_cast<char>(value & 0xffU),
                    static_cast<char>((value >> 8U) & 0xffU),
                    static_cast<char>((value >> 16U) & 0xffU),
                    static_cast<char>((value >> 24U) & 0xffU)};
                m_output.write(bytes.data(), bytes.size());
            }

            Result<void> pump(const char* data, const std::size_t size, const int flush) {
                m_stream.next_in = reinterpret_cast<const unsigned char*>(data);
                m_stream.avail_in = static_cast<unsigned int>(size);
                for (;;) {
                    m_stream.next_out = m_buffer.data();
                    m_stream.avail_out = static_cast<unsigned int>(m_buffer.size());
                    const int status = mz_deflate(&m_stream, flush);
                    if (status != MZ_OK && status != MZ_STREAM_END && status != MZ_BUF_ERROR)
                        return std::unexpected(error("cannot compress the archive contents"));

                    const std::size_t produced = m_buffer.size() - m_stream.avail_out;
                    if (produced != 0)
                        m_output.write(reinterpret_cast<const char*>(m_buffer.data()), static_cast<std::streamsize>(produced));

                    if (status == MZ_STREAM_END)
                        return {};

                    if (flush != MZ_FINISH && m_stream.avail_in == 0 && produced < m_buffer.size())
                        return {};

                    if (status == MZ_BUF_ERROR && produced == 0)
                        return std::unexpected(error("cannot compress the archive contents"));
                }
            }

            std::ofstream& m_output;
            mz_stream m_stream{};
            bool m_started = false;
            mz_ulong m_crc = MZ_CRC32_INIT;
            std::uint64_t m_size = 0;
            std::array<unsigned char, io_chunk> m_buffer{};
        };

        struct SourceEntry {
            std::string name;
            std::filesystem::path path;
            bool directory = false;
            std::uintmax_t size = 0;
        };

        Result<std::vector<SourceEntry>> collect_source_entries(const std::filesystem::path& root) {
            std::vector<SourceEntry> entries;
            std::error_code failure;
            for (std::filesystem::recursive_directory_iterator iterator(root, failure), end; iterator != end; iterator.increment(failure)) {
                if (failure)
                    return std::unexpected(error("cannot inspect archive contents: " + failure.message()));

                if (iterator->is_symlink(failure))
                    return std::unexpected(error("archive contents contain a symbolic link `" + iterator->path().string() + "`"));

                std::string name = iterator->path().lexically_relative(root).generic_string();
                if (name.empty() || name == ".")
                    continue;

                SourceEntry entry;
                entry.directory = iterator->is_directory(failure);
                if (failure)
                    return std::unexpected(error("cannot inspect archive entry: " + failure.message()));

                if (entry.directory) {
                    name += '/';
                } else {
                    entry.size = iterator->file_size(failure);
                    if (failure)
                        return std::unexpected(error("cannot inspect archive entry size: " + failure.message()));
                }
                entry.name = std::move(name);
                entry.path = iterator->path();
                entries.push_back(std::move(entry));
            }
            if (failure)
                return std::unexpected(error("cannot inspect archive contents: " + failure.message()));

            std::ranges::sort(entries, {}, &SourceEntry::name);
            return entries;
        }

        Result<void> append_entry_data(GzipSink& sink, const SourceEntry& entry) {
            std::ifstream input = open_input(entry.path);
            if (!input)
                return std::unexpected(error("cannot read archive entry `" + entry.path.string() + "`"));

            std::array<char, io_chunk> buffer{};
            std::uintmax_t remaining = entry.size;
            while (remaining != 0) {
                const std::size_t wanted = static_cast<std::size_t>(std::min<std::uintmax_t>(remaining, buffer.size()));
                input.read(buffer.data(), static_cast<std::streamsize>(wanted));
                if (static_cast<std::size_t>(input.gcount()) != wanted)
                    return std::unexpected(error("archive entry `" + entry.path.string() + "` changed while being archived"));

                auto written = sink.write(buffer.data(), wanted);
                if (!written)
                    return written;

                remaining -= wanted;
            }
            const std::size_t padding = (tar_block - static_cast<std::size_t>(entry.size % tar_block)) % tar_block;
            if (padding == 0)
                return {};

            const Block zeros{};
            return sink.write(zeros.data(), padding);
        }

        Result<void> append_entry(GzipSink& sink, const SourceEntry& entry) {
            auto header = tar_header(entry.name, entry.directory ? '5' : '0', entry.directory ? 0 : entry.size);
            if (!header)
                return std::unexpected(header.error());

            auto written = sink.write(header->data(), header->size());
            if (!written)
                return written;

            return entry.directory ? Result<void>{} : append_entry_data(sink, entry);
        }

        enum class ArchiveFormat {
            tar,
            tar_gzip,
            zip
        };

        Result<ArchiveFormat> detect_format(const std::filesystem::path& archive) {
            std::ifstream input = open_input(archive);
            if (!input)
                return std::unexpected(error("cannot open archive `" + archive.string() + "`"));

            std::array<char, 4> magic{};
            input.read(magic.data(), magic.size());
            if (input.gcount() < 2)
                return std::unexpected(error("archive `" + archive.string() + "` is too small to identify"));

            const auto byte = [&](const std::size_t index) { return static_cast<unsigned char>(magic[index]); };
            if (byte(0) == 0x1f && byte(1) == 0x8b)
                return ArchiveFormat::tar_gzip;

            if (input.gcount() == 4 && byte(0) == 'P' && byte(1) == 'K')
                return ArchiveFormat::zip;

            if (byte(0) == 'B' && byte(1) == 'Z')
                return std::unexpected(error("bzip2 archives are not supported; use tar, tar.gz or zip"));

            if (byte(0) == 0xfd && input.gcount() >= 3 && byte(1) == '7' && byte(2) == 'z')
                return std::unexpected(error("xz archives are not supported; use tar, tar.gz or zip"));

            return ArchiveFormat::tar;
        }

        Result<void> skip_gzip_header(std::ifstream& input) {
            std::array<char, 10> header{};
            input.read(header.data(), header.size());
            if (input.gcount() != static_cast<std::streamsize>(header.size()))
                return std::unexpected(error("truncated gzip header"));

            if (static_cast<unsigned char>(header[2]) != 8)
                return std::unexpected(error("unsupported gzip compression method"));

            const unsigned char flags = static_cast<unsigned char>(header[3]);
            if ((flags & 0x04U) != 0) {
                std::array<char, 2> extra{};
                input.read(extra.data(), extra.size());
                const std::size_t length = static_cast<unsigned char>(extra[0])
                    | (static_cast<std::size_t>(static_cast<unsigned char>(extra[1])) << 8U);
                input.seekg(static_cast<std::streamoff>(length), std::ios::cur);
            }
            for (const unsigned int mask: {0x08U, 0x10U}) {
                if ((flags & mask) == 0)
                    continue;

                char character = 0;
                do {
                    input.read(&character, 1);
                } while (input.gcount() == 1 && character != '\0');
            }
            if ((flags & 0x02U) != 0)
                input.seekg(2, std::ios::cur);

            return input ? Result<void>{} : std::unexpected(error("truncated gzip header"));
        }

        Result<void> inflate_to_file(const std::filesystem::path& archive, const std::filesystem::path& destination) {
            std::ifstream input = open_input(archive);
            if (!input)
                return std::unexpected(error("cannot open archive `" + archive.string() + "`"));

            auto skipped = skip_gzip_header(input);
            if (!skipped)
                return skipped;

            std::ofstream output = open_output(destination);
            if (!output)
                return std::unexpected(error("cannot stage decompressed archive `" + destination.string() + "`"));

            mz_stream stream{};
            if (mz_inflateInit2(&stream, -MZ_DEFAULT_WINDOW_BITS) != MZ_OK)
                return std::unexpected(error("cannot initialize the archive decompressor"));

            std::array<unsigned char, io_chunk> source{};
            std::array<unsigned char, io_chunk> sink{};
            int status = MZ_OK;
            while (status != MZ_STREAM_END) {
                input.read(reinterpret_cast<char*>(source.data()), static_cast<std::streamsize>(source.size()));
                const std::size_t available = static_cast<std::size_t>(input.gcount());
                if (available == 0) {
                    mz_inflateEnd(&stream);
                    return std::unexpected(error("truncated compressed archive `" + archive.string() + "`"));
                }
                stream.next_in = source.data();
                stream.avail_in = static_cast<unsigned int>(available);
                for (;;) {
                    stream.next_out = sink.data();
                    stream.avail_out = static_cast<unsigned int>(sink.size());
                    status = mz_inflate(&stream, MZ_NO_FLUSH);
                    if (status != MZ_OK && status != MZ_STREAM_END) {
                        mz_inflateEnd(&stream);
                        return std::unexpected(error("cannot decompress archive `" + archive.string() + "`"));
                    }
                    const std::size_t produced = sink.size() - stream.avail_out;
                    if (produced != 0)
                        output.write(reinterpret_cast<const char*>(sink.data()), static_cast<std::streamsize>(produced));

                    if (status == MZ_STREAM_END || (stream.avail_in == 0 && produced < sink.size()))
                        break;
                }
            }
            mz_inflateEnd(&stream);
            output.flush();
            if (!output)
                return std::unexpected(error("cannot stage decompressed archive `" + destination.string() + "`"));

            return {};
        }

        bool safe_entry_path(const std::string& name) {
            if (name.empty() || name.front() == '/' || name.starts_with("//"))
                return false;

            const std::filesystem::path path{name};
            if (path.is_absolute() || path.has_root_name() || path.has_root_directory())
                return false;

            return std::ranges::find(path, "..") == path.end();
        }

        std::size_t read_decimal(const std::string_view text) {
            std::size_t value = 0;
            for (const char character: text) {
                if (character < '0' || character > '9')
                    return 0;

                value = value * 10 + static_cast<std::size_t>(character - '0');
            }
            return value;
        }

        std::string pax_path(const std::string& records) {
            std::size_t begin = 0;
            while (begin < records.size()) {
                const std::size_t space = records.find(' ', begin);
                if (space == std::string::npos)
                    break;

                const std::size_t record_length = read_decimal(std::string_view(records).substr(begin, space - begin));
                if (record_length == 0 || begin + record_length > records.size())
                    break;

                const std::string_view record(records.data() + space + 1, begin + record_length - space - 1);
                if (record.starts_with("path=")) {
                    std::string_view value = record.substr(5);
                    while (!value.empty() && (value.back() == '\n' || value.back() == '\0'))
                        value.remove_suffix(1);

                    return std::string(value);
                }
                begin += record_length;
            }
            return {};
        }

        struct TarEntry {
            std::string name;
            char type = '0';
            std::uint64_t size = 0;
        };

        Result<std::string> read_entry_data(std::ifstream& input, const std::uint64_t size) {
            std::string data(static_cast<std::size_t>(size), '\0');
            input.read(data.data(), static_cast<std::streamsize>(size));
            if (static_cast<std::uint64_t>(input.gcount()) != size)
                return std::unexpected(error("truncated archive entry"));

            const std::uint64_t padding = (tar_block - (size % tar_block)) % tar_block;
            input.seekg(static_cast<std::streamoff>(padding), std::ios::cur);
            return data;
        }

        ArchiveEntryKind entry_kind(const char type) {
            if (type == '5')
                return ArchiveEntryKind::directory;

            if (type == '1' || type == '2')
                return ArchiveEntryKind::link;

            return ArchiveEntryKind::file;
        }

        std::string header_name(const Block& header) {
            const std::string_view prefix(header.data() + 345, tar_prefix_field);
            const std::string_view name(header.data(), tar_name_field);
            std::string result(prefix.substr(0, prefix.find('\0')));
            if (!result.empty())
                result += '/';

            result += name.substr(0, name.find('\0'));
            return result;
        }

        template <typename Visitor> Result<void> walk_tar(const std::filesystem::path& tar, Visitor&& visit) {
            std::ifstream input = open_input(tar);
            if (!input)
                return std::unexpected(error("cannot open archive `" + tar.string() + "`"));

            std::string pending_name;
            std::size_t visited = 0;
            for (;;) {
                Block header{};
                input.read(header.data(), header.size());
                if (input.gcount() == 0)
                    return {};

                if (static_cast<std::size_t>(input.gcount()) != header.size())
                    return std::unexpected(error("truncated archive `" + tar.string() + "`"));

                if (std::ranges::all_of(header, [](const char byte) { return byte == '\0'; }))
                    return {};

                TarEntry entry;
                entry.type = header[156];
                entry.size = read_octal(header.data() + 124, 12);
                entry.name = pending_name.empty() ? header_name(header) : std::exchange(pending_name, {});
                if (entry.type == 'L' || entry.type == 'x' || entry.type == 'g') {
                    auto data = read_entry_data(input, entry.size);
                    if (!data)
                        return std::unexpected(data.error());

                    if (entry.type == 'L')
                        pending_name = data->substr(0, data->find('\0'));
                    else if (entry.type == 'x')
                        pending_name = pax_path(*data);

                    continue;
                }
                if (++visited > 250'000)
                    return std::unexpected(error("archive contains more than 250000 entries"));

                auto visited_result = visit(entry, input);
                if (!visited_result)
                    return visited_result;
            }
        }

        Result<void> skip_tar_entry(std::ifstream& input, const std::uint64_t size) {
            const std::uint64_t blocks = (size + tar_block - 1) / tar_block;
            input.seekg(static_cast<std::streamoff>(blocks * tar_block), std::ios::cur);
            return input ? Result<void>{} : std::unexpected(error("truncated archive entry"));
        }

        class StagedTar {
        public:
            StagedTar() = default;
            StagedTar(const StagedTar&) = delete;
            StagedTar& operator=(const StagedTar&) = delete;

            ~StagedTar() {
                if (m_staged.empty())
                    return;

                std::error_code ignored;
                std::filesystem::remove(m_staged, ignored);
            }

            [[nodiscard]] Result<void> open(const std::filesystem::path& archive, const ArchiveFormat format) {
                if (format != ArchiveFormat::tar_gzip) {
                    m_path = archive;
                    return {};
                }
                m_staged = archive.parent_path() / (archive.filename().string() + ".kaixa-inflated");
                m_path = m_staged;
                return inflate_to_file(archive, m_staged);
            }

            [[nodiscard]] const std::filesystem::path& path() const noexcept { return m_path; }

        private:
            std::filesystem::path m_path;
            std::filesystem::path m_staged;
        };

        Result<std::vector<ArchiveEntry>> list_tar(const std::filesystem::path& tar) {
            std::vector<ArchiveEntry> entries;
            auto walked = walk_tar(tar, [&](const TarEntry& entry, std::ifstream& input) -> Result<void> {
                entries.push_back({entry.name, entry_kind(entry.type), entry.size});
                return skip_tar_entry(input, entry.size);
            });
            if (!walked)
                return std::unexpected(walked.error());

            return entries;
        }

        Result<void> write_extracted_file(std::ifstream& input, const std::filesystem::path& target, const std::uint64_t size) {
            std::error_code failure;
            std::filesystem::create_directories(target.parent_path(), failure);
            if (failure)
                return std::unexpected(error("cannot create archive entry directory: " + failure.message()));

            std::ofstream output = open_output(target);
            if (!output)
                return std::unexpected(error("cannot write archive entry `" + target.string() + "`"));

            std::array<char, io_chunk> buffer{};
            std::uint64_t remaining = size;
            while (remaining != 0) {
                const std::size_t wanted = static_cast<std::size_t>(std::min<std::uint64_t>(remaining, buffer.size()));
                input.read(buffer.data(), static_cast<std::streamsize>(wanted));
                if (static_cast<std::size_t>(input.gcount()) != wanted)
                    return std::unexpected(error("truncated archive entry `" + target.string() + "`"));

                output.write(buffer.data(), static_cast<std::streamsize>(wanted));
                remaining -= wanted;
            }
            const std::uint64_t padding = (tar_block - (size % tar_block)) % tar_block;
            input.seekg(static_cast<std::streamoff>(padding), std::ios::cur);
            output.flush();
            if (!output)
                return std::unexpected(error("cannot write archive entry `" + target.string() + "`"));

            return {};
        }

        Result<void> extract_tar(const std::filesystem::path& tar, const std::filesystem::path& destination) {
            return walk_tar(tar, [&](const TarEntry& entry, std::ifstream& input) -> Result<void> {
                if (!safe_entry_path(entry.name))
                    return std::unexpected(error("archive contains an unsafe path `" + entry.name + "`"));

                const ArchiveEntryKind kind = entry_kind(entry.type);
                if (kind == ArchiveEntryKind::link)
                    return std::unexpected(error("archive contains a symbolic or hard link, which is not allowed"));

                const std::filesystem::path target = destination / entry.name;
                if (kind == ArchiveEntryKind::directory) {
                    std::error_code failure;
                    std::filesystem::create_directories(target, failure);
                    if (failure)
                        return std::unexpected(error("cannot create archive directory: " + failure.message()));

                    return skip_tar_entry(input, entry.size);
                }
                return write_extracted_file(input, target, entry.size);
            });
        }

        class ZipReader {
        public:
            ZipReader() = default;
            ZipReader(const ZipReader&) = delete;
            ZipReader& operator=(const ZipReader&) = delete;

            ~ZipReader() {
                if (m_opened)
                    mz_zip_reader_end(&m_archive);

                if (m_file != nullptr)
                    std::fclose(m_file);
            }

            [[nodiscard]] Result<void> open(const std::filesystem::path& archive) {
                std::error_code failure;
                const std::uintmax_t size = std::filesystem::file_size(archive, failure);
                if (failure)
                    return std::unexpected(error("cannot inspect archive `" + archive.string() + "`: " + failure.message()));

#ifdef _WIN32
                m_file = _wfopen(archive.c_str(), L"rb");
#else
                m_file = std::fopen(archive.c_str(), "rb");
#endif
                if (m_file == nullptr)
                    return std::unexpected(error("cannot open archive `" + archive.string() + "`"));

                if (!mz_zip_reader_init_cfile(&m_archive, m_file, size, 0))
                    return std::unexpected(error("cannot read zip archive `" + archive.string() + "`"));

                m_opened = true;
                return {};
            }

            [[nodiscard]] mz_zip_archive& handle() noexcept { return m_archive; }

        private:
            mz_zip_archive m_archive{};
            std::FILE* m_file = nullptr;
            bool m_opened = false;
        };

        constexpr mz_uint max_archive_entries = 250000;

        Result<std::vector<ArchiveEntry>> list_zip(const std::filesystem::path& archive) {
            ZipReader reader;
            auto opened = reader.open(archive);
            if (!opened)
                return std::unexpected(opened.error());

            const mz_uint count = mz_zip_reader_get_num_files(&reader.handle());
            if (count > max_archive_entries)
                return std::unexpected(error("archive contains more than 250000 entries"));

            std::vector<ArchiveEntry> entries;
            for (mz_uint index = 0; index < count; ++index) {
                mz_zip_archive_file_stat stat{};
                if (!mz_zip_reader_file_stat(&reader.handle(), index, &stat))
                    return std::unexpected(error("cannot inspect zip archive entry"));

                entries.push_back(
                    {stat.m_filename,
                        stat.m_is_directory != MZ_FALSE ? ArchiveEntryKind::directory : ArchiveEntryKind::file,
                        stat.m_uncomp_size}
                );
            }
            return entries;
        }

        std::size_t append_zip_chunk(void* opaque, mz_uint64, const void* data, const std::size_t size) {
            auto* output = static_cast<std::ofstream*>(opaque);
            output->write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
            return *output ? size : 0;
        }

        Result<void> extract_zip_entry(mz_zip_archive& handle, const mz_uint index, const std::filesystem::path& target) {
            std::error_code failure;
            std::filesystem::create_directories(target.parent_path(), failure);
            if (failure)
                return std::unexpected(error("cannot create archive entry directory: " + failure.message()));

            std::ofstream output = open_output(target);
            if (!output)
                return std::unexpected(error("cannot write archive entry `" + target.string() + "`"));

            if (!mz_zip_reader_extract_to_callback(&handle, index, append_zip_chunk, &output, 0))
                return std::unexpected(error("cannot extract zip archive entry `" + target.string() + "`"));

            output.flush();
            if (!output)
                return std::unexpected(error("cannot write archive entry `" + target.string() + "`"));

            return {};
        }

        Result<void> extract_zip(const std::filesystem::path& archive, const std::filesystem::path& destination) {
            ZipReader reader;
            auto opened = reader.open(archive);
            if (!opened)
                return opened;

            const mz_uint count = mz_zip_reader_get_num_files(&reader.handle());
            if (count > max_archive_entries)
                return std::unexpected(error("archive contains more than 250000 entries"));

            for (mz_uint index = 0; index < count; ++index) {
                mz_zip_archive_file_stat stat{};
                if (!mz_zip_reader_file_stat(&reader.handle(), index, &stat))
                    return std::unexpected(error("cannot inspect zip archive entry"));

                const std::string name(stat.m_filename);
                if (!safe_entry_path(name))
                    return std::unexpected(error("archive contains an unsafe path `" + name + "`"));

                const std::filesystem::path target = destination / name;
                if (stat.m_is_directory != MZ_FALSE) {
                    std::error_code failure;
                    std::filesystem::create_directories(target, failure);
                    if (failure)
                        return std::unexpected(error("cannot create archive directory: " + failure.message()));

                    continue;
                }
                auto extracted = extract_zip_entry(reader.handle(), index, target);
                if (!extracted)
                    return extracted;
            }
            return {};
        }

        Result<void> write_tar_stream(GzipSink& sink, const std::vector<SourceEntry>& entries) {
            auto begun = sink.begin();
            if (!begun)
                return begun;

            for (const SourceEntry& entry: entries) {
                auto appended = append_entry(sink, entry);
                if (!appended)
                    return appended;
            }
            const Block zeros{};
            for (int index = 0; index < 2; ++index) {
                auto written = sink.write(zeros.data(), zeros.size());
                if (!written)
                    return written;
            }
            return sink.finish();
        }

        class MinizArchiveBackend final : public ArchiveBackend {
        public:
            [[nodiscard]] Result<void> create_archive(
                const std::filesystem::path& contents,
                const std::filesystem::path& destination
            ) const override {
                auto entries = collect_source_entries(contents);
                if (!entries)
                    return std::unexpected(entries.error());

                std::ofstream output = open_output(destination);
                if (!output)
                    return std::unexpected(error("cannot create package archive `" + destination.string() + "`"));

                GzipSink sink(output);
                auto written = write_tar_stream(sink, *entries);
                if (!written)
                    return written;

                output.flush();
                if (!output)
                    return std::unexpected(error("cannot create package archive `" + destination.string() + "`"));

                return {};
            }

            [[nodiscard]] Result<std::vector<ArchiveEntry>> list_archive(const std::filesystem::path& archive) const override {
                auto format = detect_format(archive);
                if (!format)
                    return std::unexpected(format.error());

                if (*format == ArchiveFormat::zip)
                    return list_zip(archive);

                StagedTar staged;
                auto opened = staged.open(archive, *format);
                if (!opened)
                    return std::unexpected(opened.error());

                return list_tar(staged.path());
            }

            [[nodiscard]] Result<void> extract_archive(
                const std::filesystem::path& archive,
                const std::filesystem::path& destination
            ) const override {
                auto format = detect_format(archive);
                if (!format)
                    return std::unexpected(format.error());

                std::error_code failure;
                std::filesystem::create_directories(destination, failure);
                if (failure)
                    return std::unexpected(error("cannot create archive extraction directory: " + failure.message()));

                if (*format == ArchiveFormat::zip)
                    return extract_zip(archive, destination);

                StagedTar staged;
                auto opened = staged.open(archive, *format);
                if (!opened)
                    return opened;

                return extract_tar(staged.path(), destination);
            }
        };
    }

    const ArchiveBackend& default_archive_backend() {
        static const MinizArchiveBackend backend;
        return backend;
    }
}
