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
inline std::string shell_quote(const std::string& s) {
    std::string q = "'";
    for (char c : s) q += c == '\'' ? std::string("'\\''") : std::string(1, c);
    return q + "'";
}

/**
 * @brief `export PATH=...;` putting Homebrew's bin directories first: an app started from the
 *        Dock / Finder (or by the hub) gets a bare PATH without cmake / glslc / git.
 */
inline std::string tool_path_prefix() {
    return "export PATH=\"/opt/homebrew/bin:/usr/local/bin:$PATH\"; ";
}

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
    ~Task() {
        cancel();
        if (thread_.joinable()) thread_.join();
    }

    /** @brief Starts `command` (via /bin/sh -c); false if one is still running. */
    bool start(std::string title, const std::string& command) {
        if (running()) return false;
        if (thread_.joinable()) thread_.join();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            title_ = std::move(title);
            lines_.clear();
            lines_.push_back("$ " + command);
        }
        exit_code_ = -1;
        cancelled_ = false;
        int fds[2];
        if (::pipe(fds) != 0) { append_("failed to start: pipe"); exit_code_ = 127; return true; }
        posix_spawn_file_actions_t fa;
        posix_spawn_file_actions_init(&fa);
        posix_spawn_file_actions_adddup2(&fa, fds[1], STDOUT_FILENO);
        posix_spawn_file_actions_adddup2(&fa, fds[1], STDERR_FILENO);
        posix_spawn_file_actions_addclose(&fa, fds[0]);
        posix_spawn_file_actions_addclose(&fa, fds[1]);
        posix_spawn_file_actions_addopen(&fa, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
        posix_spawnattr_t attr;
        posix_spawnattr_init(&attr);
        posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP);
        posix_spawnattr_setpgroup(&attr, 0);   // its own group: cancel() signals all of it
        std::string cmd = command;
        char* argv[] = {const_cast<char*>("/bin/sh"), const_cast<char*>("-c"), cmd.data(), nullptr};
        pid_t pid = 0;
        const int rc = ::posix_spawn(&pid, "/bin/sh", &fa, &attr, argv, environ);
        posix_spawn_file_actions_destroy(&fa);
        posix_spawnattr_destroy(&attr);
        ::close(fds[1]);
        if (rc != 0) {
            ::close(fds[0]);
            append_("failed to start: " + command);
            exit_code_ = 127;
            return true;
        }
        pid_ = pid;
        running_ = true;
        ++generation_;
        thread_ = std::thread([this, fd = fds[0], pid] {
            std::string partial;
            char buf[1024];
            ssize_t n;
            while ((n = ::read(fd, buf, sizeof(buf))) > 0) {
                partial.append(buf, static_cast<size_t>(n));
                size_t nl;
                while ((nl = partial.find('\n')) != std::string::npos) {
                    append_(partial.substr(0, nl));
                    partial.erase(0, nl + 1);
                }
            }
            if (!partial.empty()) append_(partial);
            ::close(fd);
            int status = 0;
            ::waitpid(pid, &status, 0);
            exit_code_ = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
            pid_ = 0;
            running_ = false;
        });
        return true;
    }

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
    bool start(std::string title, std::function<int(JobContext&)> body) {
        if (running()) return false;
        if (thread_.joinable()) thread_.join();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            title_ = std::move(title);
            lines_.clear();
        }
        exit_code_ = -1;
        cancelled_ = false;
        pid_ = 0;
        running_ = true;
        ++generation_;
        thread_ = std::thread([this, body = std::move(body)] {
            JobContext ctx;
            ctx.log = [this](const std::string& l) { append_(l); };
            ctx.cancelled = &cancelled_;
            ctx.child = &pid_;
            int code = 1;
            try {
                code = body(ctx);
            } catch (const std::exception& e) {
                append_(std::string("error: ") + e.what());
            }
            exit_code_ = cancelled_ ? 130 : code;
            pid_ = 0;
            running_ = false;
        });
        return true;
    }

    /** @brief Stops the running command and everything it started (SIGTERM to its group). */
    void cancel() {
        if (!running_) return;
        cancelled_ = true;
        const pid_t pid = pid_;
        if (pid > 0) ::kill(-pid, SIGTERM);
    }

    /** @brief Blocks until the current command finishes (tests, shutdown). */
    void wait() { if (thread_.joinable()) thread_.join(); }

    bool running() const { return running_; }
    bool cancelled() const { return cancelled_; }
    /** @brief The last command's exit status (-1 while running or before the first). */
    int exit_code() const { return exit_code_; }
    bool succeeded() const { return !running_ && exit_code_ == 0; }
    /** @brief Increments per start(): lets a caller act once when "its" run finishes. */
    int generation() const { return generation_; }
    std::string title() const { std::lock_guard<std::mutex> lock(mutex_); return title_; }
    std::vector<std::string> lines() const { std::lock_guard<std::mutex> lock(mutex_); return lines_; }
    size_t line_count() const { std::lock_guard<std::mutex> lock(mutex_); return lines_.size(); }
    void clear() { if (!running()) { std::lock_guard<std::mutex> lock(mutex_); lines_.clear(); title_.clear(); exit_code_ = -1; } }

private:
    void append_(std::string line) {
        // Strip ANSI colour codes (cmake / compilers) and carriage-return progress rewrites.
        std::string clean;
        for (size_t i = 0; i < line.size(); ++i) {
            if (line[i] == '\x1b') { while (i < line.size() && !std::isalpha(static_cast<unsigned char>(line[i]))) ++i; continue; }
            if (line[i] == '\r') { clean.clear(); continue; }
            clean += line[i];
        }
        std::lock_guard<std::mutex> lock(mutex_);
        lines_.push_back(std::move(clean));
        if (lines_.size() > 20000) lines_.erase(lines_.begin(), lines_.begin() + 5000);
    }

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
inline void launch_detached(const std::string& command, const std::filesystem::path& log) {
    std::error_code ec;
    std::filesystem::create_directories(log.parent_path(), ec);
    const std::string line = tool_path_prefix() + "nohup " + command + " > " + shell_quote(log.string()) + " 2>&1 < /dev/null &";
    [[maybe_unused]] const int rc = std::system(line.c_str());
}

/** @brief Opens a file or folder with the platform's default application / file browser. */
inline void open_with_system(const std::filesystem::path& p) {
#if defined(__APPLE__)
    const std::string cmd = "open " + shell_quote(p.string());
#else
    const std::string cmd = "xdg-open " + shell_quote(p.string()) + " >/dev/null 2>&1 &";
#endif
    [[maybe_unused]] const int rc = std::system(cmd.c_str());
}

/** @brief First line of a command's output (synchronous; for quick `git` / `which` queries). */
inline std::string command_output(const std::string& command) {
    std::string out;
    if (FILE* pipe = ::popen((tool_path_prefix() + command + " 2>/dev/null").c_str(), "r")) {
        char buf[512];
        if (std::fgets(buf, sizeof(buf), pipe)) out = buf;
        ::pclose(pipe);
    }
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    return out;
}

/** @brief A `path:line[:col]: error|warning: message` line from a compiler or linker. */
struct Diagnostic {
    std::string file;
    int line = 0, column = 0;
    bool error = true;
    std::string message;
    std::string text;   ///< The whole log line
};

/** @brief Parses clang / gcc diagnostics (`a.cpp:12:5: error: ...`); nullopt for other lines. */
inline std::optional<Diagnostic> parse_diagnostic(const std::string& l) {
    for (const char* kind : {": error: ", ": fatal error: ", ": warning: "}) {
        const size_t k = l.find(kind);
        if (k == std::string::npos) continue;
        Diagnostic d;
        d.error = std::string(kind) != ": warning: ";
        d.message = l.substr(k + std::char_traits<char>::length(kind));
        d.text = l;
        // "path:line:col" or "path:line" before the kind.
        std::string loc = l.substr(0, k);
        auto take_num = [&](int& out) {
            const size_t c = loc.rfind(':');
            if (c == std::string::npos) return false;
            const std::string n = loc.substr(c + 1);
            if (n.empty() || !std::all_of(n.begin(), n.end(), [](unsigned char ch) { return std::isdigit(ch); })) return false;
            out = std::stoi(n);
            loc.erase(c);
            return true;
        };
        int a = 0, b = 0;
        if (take_num(a)) {
            if (take_num(b)) { d.line = b; d.column = a; }
            else d.line = a;
        }
        d.file = loc;
        return d;
    }
    return std::nullopt;
}

/**
 * @brief Opens `file` at `line` in the user's code editor: VS Code (`code -g`) when it's on the
 *        PATH, else the system default application for the file.
 */
inline void open_in_code_editor(const std::string& file, int line, int column) {
    const std::string code = command_output("command -v code");
    if (!code.empty()) {
        const std::string loc = file + ":" + std::to_string(std::max(1, line)) + ":" + std::to_string(std::max(1, column));
        launch_detached(shell_quote(code) + " -g " + shell_quote(loc), std::filesystem::temp_directory_path() / "toyengine_code.log");
        return;
    }
    open_with_system(file);
}

} // namespace toy::editor

#endif // TOYEDITOR_CORE_PROCESS_H
