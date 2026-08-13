#include <kaixa/foundation/process.hpp>

#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <system_error>

#ifdef _WIN32
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
#else
    #include <sys/wait.h>
    #include <unistd.h>
#endif

namespace kaixa {
    namespace {
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

#ifdef _WIN32
        Result<std::wstring> widen(const std::string_view text) {
            if (text.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
                return std::unexpected(error("text is too long for the Windows process API"));
            if (text.empty())
                return std::wstring{};

            const int source_size = static_cast<int>(text.size());
            const int required = MultiByteToWideChar(
                CP_UTF8,
                MB_ERR_INVALID_CHARS,
                text.data(),
                source_size,
                nullptr,
                0
            );
            if (required == 0)
                return std::unexpected(error("process argument is not valid UTF-8"));

            std::wstring result(static_cast<std::size_t>(required), L'\0');
            MultiByteToWideChar(
                CP_UTF8,
                MB_ERR_INVALID_CHARS,
                text.data(),
                source_size,
                result.data(),
                required
            );
            return result;
        }

        std::string windows_error(const DWORD code) {
            return std::system_category().message(static_cast<int>(code));
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

#ifdef _WIN32
        const auto wide_command_result = widen(format_command(request.argv));
        if (!wide_command_result)
            return std::unexpected(wide_command_result.error());
        std::wstring command = *wide_command_result;

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

        const BOOL created = CreateProcessW(
            nullptr,
            command.data(),
            nullptr,
            nullptr,
            TRUE,
            0,
            nullptr,
            working_directory.empty() ? nullptr : working_directory.c_str(),
            &startup,
            &process
        );
        if (!created)
            return std::unexpected(error(
                "cannot start `" + request.argv.front() + "`: " + windows_error(GetLastError())
            ));

        const DWORD wait_result = WaitForSingleObject(process.hProcess, INFINITE);
        DWORD exit_code = 0;
        const BOOL read_exit_code = GetExitCodeProcess(process.hProcess, &exit_code);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);

        if (wait_result == WAIT_FAILED)
            return std::unexpected(error("cannot wait for child process"));
        if (!read_exit_code)
            return std::unexpected(error("cannot read child process exit code"));
        return ProcessResult{static_cast<int>(exit_code)};
#else
        const pid_t child = fork();
        if (child < 0)
            return std::unexpected(error(std::string("cannot fork: ") + std::strerror(errno)));

        if (child == 0) {
            if (!request.working_directory.empty()
                && chdir(request.working_directory.c_str()) != 0)
                _exit(126);

            std::vector<char*> arguments;
            arguments.reserve(request.argv.size() + 1);
            for (const std::string& argument: request.argv)
                arguments.push_back(const_cast<char*>(argument.c_str()));
            arguments.push_back(nullptr);
            execvp(arguments.front(), arguments.data());
            _exit(127);
        }

        int status = 0;
        while (waitpid(child, &status, 0) < 0) {
            if (errno != EINTR)
                return std::unexpected(error(
                    std::string("cannot wait for child process: ") + std::strerror(errno)
                ));
        }

        if (WIFEXITED(status))
            return ProcessResult{WEXITSTATUS(status)};
        if (WIFSIGNALED(status))
            return ProcessResult{128 + WTERMSIG(status)};
        return std::unexpected(error("child process ended without an exit status"));
#endif
    }
}
