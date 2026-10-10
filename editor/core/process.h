/**
 * @file process.h
 * @brief Child processes for the editor and the hub: a cancellable background Task whose output
 *        streams in line by line (builds, tools/toyhub), plus small helpers (shell quoting,
 *        detached launches, this executable's path).
 */

#ifndef TOYEDITOR_CORE_PROCESS_H
#define TOYEDITOR_CORE_PROCESS_H

#include <algorithm>
#include <atomic>
#include <cctype>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <toyengine/core/runtime_paths.h>

extern char** environ;

namespace toy::editor {

/** @brief Single-quotes `s` for /bin/sh. */
std::string shell_quote(const std::string& s);

/**
 * @brief `export PATH=...;` putting Homebrew's bin directories first: an app started from the
 *        Dock / Finder (or by the hub) gets a bare PATH without cmake / glslc / git.
 */
std::string tool_path_prefix();

/** @brief The running executable's absolute path (empty if it can't be found). */
inline std::filesystem::path current_executable() { return toy::core::executable_path(); }

/**
 * @class Task
 * @brief A shell command run in the background, its combined stdout/stderr collected line by
 *        line. One at a time per Task. The command runs in its own process group, so cancel()
 *        stops everything it started (cmake, make, the compilers).
 */
class Task {
public:
    ~Task();

    /** @brief Starts `command` (via /bin/sh -c); false if one is still running. */
    bool start(std::string title, const std::string& command);

    /**
     * @brief A C++ job's view of its Task: where its log lines go, whether cancel() was asked,
     *        and the slot a child process it spawns registers in (own process group) so cancel()
     *        can stop it -- see CommandRunner (editor/build/deps.h).
     */
    struct JobContext {
        std::function<void(const std::string&)> log;
        const std::atomic<bool>* cancelled = nullptr;
        std::atomic<pid_t>* child = nullptr;
    };

    /**
     * @brief Starts `body` on the task's thread (a multi-step job such as Build): its lines
     *        collect like a command's, its return value is the exit code. False if busy.
     */
    bool start(std::string title, std::function<int(JobContext&)> body);

    /** @brief Stops the running command and everything it started (SIGTERM to its group). */
    void cancel();

    /** @brief Blocks until the current command finishes (tests, shutdown). */
    void wait() { if (thread_.joinable()) thread_.join(); }

    bool running() const { return running_; }
    bool cancelled() const { return cancelled_; }
    /** @brief The last command's exit status (-1 while running or before the first). */
    int exit_code() const { return exit_code_; }
    bool succeeded() const { return !running_ && exit_code_ == 0; }
    /** @brief Increments per start(): lets a caller act once when "its" run finishes. */
    int generation() const { return generation_; }
    std::string title() const;
    std::vector<std::string> lines() const;
    size_t line_count() const;
    void clear();

private:
    void append_(std::string line);

    mutable std::mutex mutex_;
    std::string title_;
    std::vector<std::string> lines_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> cancelled_{false};
    std::atomic<int> exit_code_{-1};
    std::atomic<int> generation_{0};
    std::atomic<pid_t> pid_{0};
};

/** @brief Runs `command` detached (nohup, backgrounded): it outlives this process. */
void launch_detached(const std::string& command, const std::filesystem::path& log);

/** @brief Opens a file or folder with the platform's default application / file browser. */
void open_with_system(const std::filesystem::path& p);

/** @brief First line of a command's output (synchronous; for quick `git` / `which` queries). */
std::string command_output(const std::string& command);

/** @brief A `path:line[:col]: error|warning: message` line from a compiler or linker. */
struct Diagnostic {
    std::string file;
    int line = 0, column = 0;
    bool error = true;
    std::string message;
    std::string text;   ///< The whole log line
};

/** @brief Parses clang / gcc diagnostics (`a.cpp:12:5: error: ...`); nullopt for other lines. */
std::optional<Diagnostic> parse_diagnostic(const std::string& l);

/**
 * @brief Opens `file` at `line` in the user's code editor: VS Code (`code -g`) when it's on the
 *        PATH, else the system default application for the file.
 */
void open_in_code_editor(const std::string& file, int line, int column);

} // namespace toy::editor

#endif // TOYEDITOR_CORE_PROCESS_H
