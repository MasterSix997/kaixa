#include <kaixa/foundation/process.hpp>

#include <algorithm>
#include <bit>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <iostream>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <io.h>
#include <windows.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace kaixa {
    namespace {
        std::FILE* create_capture() {
#ifdef _WIN32
            std::FILE* capture = nullptr;
            return tmpfile_s(&capture) == 0 ? capture : nullptr;
#else
            return std::tmpfile();
#endif
        }

        std::string read_capture(std::FILE* capture) {
            if (!capture)
                return {};

            std::fflush(capture);
            if (std::fseek(capture, 0, SEEK_SET) != 0)
                return {};

            std::string output;
            char buffer[4096];
            while (const std::size_t count = std::fread(buffer, 1, sizeof(buffer), capture))
                output.append(buffer, count);
            return output;
        }

        bool needs_quotes(const std::string_view argument) {
            if (argument.empty())
                return true;

            for (const char character: argument) {
                if (std::isspace(static_cast<unsigned char>(character)) != 0 || character == '"')
                    return true;
            }
            return false;
        }

        std::string quote(const std::string_view argument) {
            if (!needs_quotes(argument))
                return std::string(argument);

            std::string result = "\"";
            std::size_t slashes = 0;
            for (const char character: argument) {
                if (character == '\\') {
                    ++slashes;
                    continue;
                }

                if (character == '"') {
                    result.append(slashes * 2 + 1, '\\');
                    result += '"';
                } else {
                    result.append(slashes, '\\');
                    result += character;
                }
                slashes = 0;
            }
            result.append(slashes * 2, '\\');
            result += '"';
            return result;
        }

        void forward_progress_line(std::string_view line) {
            if (!line.empty() && line.back() == '\r')
                line.remove_suffix(1);
            const bool windows_command = line.contains("cmd.exe /C") || line.contains("cmd.exe /c");
            const bool windows_tool_invocation = line.size() > 2 && line[1] == ':' && (line.contains(".EXE") || line.contains(".exe"));
            if (line.empty() || windows_command || windows_tool_invocation)
                return;

            std::cout << "  " << line << '\n';
            std::cout.flush();
        }

        void forward_capture(std::FILE* capture, std::size_t& offset, std::string& pending, const bool final) {
            if (!capture)
                return;

            std::fflush(capture);
            if (std::fseek(capture, static_cast<long>(offset), SEEK_SET) != 0)
                return;

            char buffer[4096];
            while (const std::size_t count = std::fread(buffer, 1, sizeof(buffer), capture)) {
                offset += count;
                pending.append(buffer, count);
            }

            std::size_t start = 0;
            for (;;) {
                const std::size_t end = pending.find('\n', start);
                if (end == std::string::npos)
                    break;
                forward_progress_line(std::string_view(pending).substr(start, end - start));
                start = end + 1;
            }
            if (start != 0)
                pending.erase(0, start);
            if (final && !pending.empty()) {
                forward_progress_line(pending);
                pending.clear();
            }
        }

#ifdef _WIN32
        Result<std::wstring> widen(const std::string_view text) {
            if (text.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
                return std::unexpected(error("text is too long for the Windows process API"));

            if (text.empty())
                return std::wstring{};

            const int source_size = static_cast<int>(text.size());
            const int required = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), source_size, nullptr, 0);
            if (required == 0)
                return std::unexpected(error("process argument is not valid UTF-8"));

            std::wstring result(static_cast<std::size_t>(required), L'\0');
            MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), source_size, result.data(), required);
            return result;
        }

        std::string windows_error(const DWORD code) {
            return std::system_category().message(static_cast<int>(code));
        }

        std::wstring_view environment_name(const std::wstring_view entry) {
            const std::size_t start = entry.starts_with(L'=') ? 1 : 0;
            const std::size_t separator = entry.find(L'=', start);
            return separator == std::wstring_view::npos ? entry : entry.substr(0, separator);
        }

        bool equal_environment_name(const std::wstring_view left, const std::wstring_view right) {
            return CompareStringOrdinal(left.data(), static_cast<int>(left.size()), right.data(), static_cast<int>(right.size()), TRUE)
                == CSTR_EQUAL;
        }

        Result<std::vector<wchar_t>> windows_environment(const std::span<const EnvironmentVariable> overrides) {
            if (overrides.empty())
                return std::vector<wchar_t>{};

            LPWCH inherited = GetEnvironmentStringsW();
            if (!inherited)
                return std::unexpected(error("cannot read the process environment: " + windows_error(GetLastError())));

            std::vector<std::wstring> entries;
            for (const wchar_t* entry = inherited; *entry != L'\0'; entry += std::wcslen(entry) + 1)
                entries.emplace_back(entry);

            FreeEnvironmentStringsW(inherited);
            for (const EnvironmentVariable& override_value: overrides) {
                if (override_value.name.empty() || override_value.name.contains('=')) {
                    return std::unexpected(error("invalid environment variable name `" + override_value.name + "`"));
                }

                auto name = widen(override_value.name);
                if (!name)
                    return std::unexpected(name.error());

                auto value = widen(override_value.value);
                if (!value)
                    return std::unexpected(value.error());

                const auto existing = std::ranges::find_if(entries, [&](const std::wstring& entry) {
                    return equal_environment_name(environment_name(entry), *name);
                });
                std::wstring combined = *name + L'=' + *value;
                if (existing == entries.end())
                    entries.push_back(std::move(combined));
                else
                    *existing = std::move(combined);
            }

            std::ranges::sort(entries, [](const std::wstring& left, const std::wstring& right) {
                return CompareStringOrdinal(left.c_str(), -1, right.c_str(), -1, TRUE) == CSTR_LESS_THAN;
            });

            std::vector<wchar_t> block;
            for (const std::wstring& entry: entries) {
                block.insert(block.end(), entry.begin(), entry.end());
                block.push_back(L'\0');
            }
            block.push_back(L'\0');
            return block;
        }
#endif

#ifdef _WIN32
        Result<ProcessResult> run_process_platform(const ProcessRequest& request, std::FILE* capture) {
            const auto wide_command_result = widen(format_command(request.argv));
            if (!wide_command_result)
                return std::unexpected(wide_command_result.error());

            std::wstring command = *wide_command_result;
            auto environment = windows_environment(request.environment);
            if (!environment)
                return std::unexpected(environment.error());

            std::wstring working_directory;
            if (!request.working_directory.empty()) {
                const auto directory_result = widen(request.working_directory.string());
                if (!directory_result)
                    return std::unexpected(directory_result.error());
                working_directory = *directory_result;
            }

            STARTUPINFOW startup{};
            startup.cb = sizeof(startup);
            PROCESS_INFORMATION process{};
            if (capture) {
                const HANDLE output = std::bit_cast<HANDLE>(_get_osfhandle(_fileno(capture)));
                if (output == INVALID_HANDLE_VALUE || !SetHandleInformation(output, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT))
                    return std::unexpected(error("cannot prepare process output capture"));
                startup.dwFlags |= STARTF_USESTDHANDLES;
                startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
                startup.hStdOutput = output;
                startup.hStdError = output;
            }

            const BOOL created = CreateProcessW(
                nullptr,
                command.data(),
                nullptr,
                nullptr,
                TRUE,
                environment->empty() ? 0 : CREATE_UNICODE_ENVIRONMENT,
                environment->empty() ? nullptr : environment->data(),
                working_directory.empty() ? nullptr : working_directory.c_str(),
                &startup,
                &process
            );
            if (!created)
                return std::unexpected(error("cannot start `" + request.argv.front() + "`: " + windows_error(GetLastError())));

            std::size_t offset = 0;
            std::string pending;
            const bool streaming = request.output == ProcessOutputMode::stream;
            DWORD wait_result = WAIT_TIMEOUT;
            while (wait_result == WAIT_TIMEOUT) {
                wait_result = WaitForSingleObject(process.hProcess, streaming ? 100 : INFINITE);
                if (streaming)
                    forward_capture(capture, offset, pending, wait_result != WAIT_TIMEOUT);
            }
            DWORD exit_code = 0;
            const BOOL read_exit_code = GetExitCodeProcess(process.hProcess, &exit_code);
            CloseHandle(process.hThread);
            CloseHandle(process.hProcess);
            if (wait_result == WAIT_FAILED)
                return std::unexpected(error("cannot wait for child process"));
            if (!read_exit_code)
                return std::unexpected(error("cannot read child process exit code"));

            return ProcessResult{static_cast<int>(exit_code), read_capture(capture)};
        }
#else
        Result<ProcessResult> run_process_platform(const ProcessRequest& request, std::FILE* capture) {
            const pid_t child = fork();
            if (child < 0)
                return std::unexpected(error(std::string("cannot fork: ") + std::strerror(errno)));

            if (child == 0) {
                if (!request.working_directory.empty() && chdir(request.working_directory.c_str()) != 0)
                    _exit(126);
                if (capture) {
                    const int descriptor = fileno(capture);
                    if (dup2(descriptor, STDOUT_FILENO) < 0 || dup2(descriptor, STDERR_FILENO) < 0)
                        _exit(126);
                }
                for (const EnvironmentVariable& variable: request.environment) {
                    if (variable.name.empty()
                        || variable.name.contains('=')
                        || setenv(variable.name.c_str(), variable.value.c_str(), 1) != 0) {
                        _exit(126);
                    }
                }

                std::vector<char*> arguments;
                arguments.reserve(request.argv.size() + 1);
                for (const std::string& argument: request.argv)
                    arguments.push_back(const_cast<char*>(argument.c_str()));
                arguments.push_back(nullptr);
                execvp(arguments.front(), arguments.data());
                _exit(127);
            }

            int status = 0;
            if (request.output == ProcessOutputMode::stream) {
                std::size_t offset = 0;
                std::string pending;
                for (;;) {
                    const pid_t waited = waitpid(child, &status, WNOHANG);
                    if (waited == child)
                        break;
                    if (waited < 0 && errno != EINTR)
                        return std::unexpected(error(std::string("cannot wait for child process: ") + std::strerror(errno)));

                    forward_capture(capture, offset, pending, false);
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                }
                forward_capture(capture, offset, pending, true);
            } else {
                while (waitpid(child, &status, 0) < 0) {
                    if (errno != EINTR)
                        return std::unexpected(error(std::string("cannot wait for child process: ") + std::strerror(errno)));
                }
            }

            std::string output = read_capture(capture);
            if (WIFEXITED(status))
                return ProcessResult{WEXITSTATUS(status), std::move(output)};
            if (WIFSIGNALED(status))
                return ProcessResult{128 + WTERMSIG(status), std::move(output)};
            return std::unexpected(error("child process ended without an exit status"));
        }
#endif
    }

    std::string format_command(const std::span<const std::string> argv) {
        std::string command;
        for (const std::string& argument: argv) {
            if (!command.empty())
                command += ' ';

            command += quote(argument);
        }
        return command;
    }

    std::optional<std::string> environment_variable(const std::string_view name) {
#ifdef _WIN32
        char* value = nullptr;
        std::size_t size = 0;
        const std::string owned_name(name);
        if (_dupenv_s(&value, &size, owned_name.c_str()) != 0 || !value)
            return std::nullopt;

        std::string result(value);
        std::free(value);
        return result;
#else
        const std::string owned_name(name);
        const char* value = std::getenv(owned_name.c_str());
        return value ? std::optional<std::string>(value) : std::nullopt;
#endif
    }

    Result<ProcessResult> run_process(const ProcessRequest& request) {
        if (request.argv.empty())
            return std::unexpected(error("cannot run an empty command"));

        const bool retains_output = request.output != ProcessOutputMode::inherit;
        std::FILE* capture = retains_output ? create_capture() : nullptr;
        if (retains_output && !capture)
            return std::unexpected(error("cannot create a temporary process output file"));
        auto result = run_process_platform(request, capture);
        if (capture)
            std::fclose(capture);
        return result;
    }
}
