#include <kaixa/package/publication_backend.hpp>

#include <kaixa/foundation/filesystem.hpp>
#include <kaixa/foundation/process.hpp>

#include <utility>
#include <vector>

namespace kaixa {
    namespace {
        class CommandPublicationBackend final : public PublicationBackend {
        public:
            [[nodiscard]] Result<void> create_archive(
                const std::filesystem::path& contents,
                const std::filesystem::path& destination
            ) const override {
                auto process = run_process(
                    {{"cmake", "-E", "tar", "czf", destination.string(), "--format=gnutar", "."}, contents, {}, true}
                );
                if (!process)
                    return std::unexpected(process.error());

                if (!process->succeeded())
                    return std::unexpected(error("cannot create package archive").add_note(process->output));

                return {};
            }

            [[nodiscard]] Result<void> upload(const PackageUpload& request) const override {
                std::vector<std::string> arguments{
                    "curl",
                    "--fail",
                    "--silent",
                    "--show-error",
                    "--request",
                    "POST",
                    "--form",
                    "metadata=@" + request.metadata.string() + ";type=application/toml",
                    "--form",
                    "package=@" + request.archive.string() + ";type=application/gzip",
                };
                std::optional<std::filesystem::path> credential_file;
                if (request.token_environment) {
                    const auto token = environment_variable(*request.token_environment);
                    if (!token) {
                        return std::unexpected(error("credential environment variable `" + *request.token_environment + "` is not set"));
                    }
                    if (token->contains('\n') || token->contains('\r'))
                        return std::unexpected(error("credential environment variable contains a newline"));

                    std::string escaped;
                    for (const char character: *token) {
                        if (character == '\\' || character == '"')
                            escaped += '\\';

                        escaped += character;
                    }
                    credential_file = request.metadata.parent_path() / ".curl-config";
                    auto written = write_file(*credential_file, "header = \"Authorization: Bearer " + escaped + "\"\n");
                    if (!written)
                        return std::unexpected(written.error());

#ifndef _WIN32
                    std::error_code permission_failure;
                    std::filesystem::permissions(
                        *credential_file,
                        std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
                        std::filesystem::perm_options::replace,
                        permission_failure
                    );
                    if (permission_failure) {
                        return std::unexpected(error("cannot protect the temporary credential file: " + permission_failure.message()));
                    }
#endif

                    arguments.emplace_back("--config");
                    arguments.emplace_back(credential_file->string());
                }
                arguments.push_back(request.endpoint);
                auto uploaded = run_process({std::move(arguments), request.working_directory, {}, true});
                if (credential_file) {
                    std::error_code ignored;
                    std::filesystem::remove(*credential_file, ignored);
                }
                if (!uploaded)
                    return std::unexpected(uploaded.error());

                if (!uploaded->succeeded()) {
                    return std::unexpected(
                        error("registry rejected package publication with exit code " + std::to_string(uploaded->exit_code))
                            .add_note(uploaded->output)
                    );
                }
                return {};
            }
        };
    }

    const PublicationBackend& command_publication_backend() {
        static const CommandPublicationBackend backend;
        return backend;
    }
}
