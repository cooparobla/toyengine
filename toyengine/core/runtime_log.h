/**
 * @file runtime_log.h
 * @brief A packaged game's log file and crash reports.
 *
 * A game launched from Finder or a desktop launcher has no terminal: whatever the engine prints
 * to std::cout / std::cerr is lost, and a crash leaves nothing behind. install_runtime_log()
 * mirrors both streams into <user_log_dir>/<name>.log (keeping the previous run's as
 * <name>.prev.log); in a shipping build the console copy is dropped and only the file is
 * written. install_crash_handlers() catches fatal signals and std::terminate and writes
 * crash-<pid>.txt beside the log -- the signal, the in-flight exception (if any) and a
 * backtrace -- before letting the process die as it would have.
 */

#ifndef TOYENGINE_CORE_RUNTIME_LOG_H
#define TOYENGINE_CORE_RUNTIME_LOG_H

#include <toyengine/core/runtime_paths.h>

#include <csignal>
#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <streambuf>
#include <string>
#include <system_error>

#include <execinfo.h>
#include <fcntl.h>
#include <unistd.h>

namespace toy::core {

namespace detail {

/** @brief A streambuf writing to a file and (optionally) the stream's original buffer. */
class TeeBuf : public std::streambuf {
public:
    TeeBuf(std::streambuf* console, std::streambuf* file) : console_(console), file_(file) {}

protected:
    int overflow(int c) override {
        if (c == traits_type::eof()) return traits_type::not_eof(c);
        if (console_ && console_->sputc(static_cast<char>(c)) == traits_type::eof()) console_ = nullptr;
        if (file_) file_->sputc(static_cast<char>(c));
        return c;
    }
    std::streamsize xsputn(const char* s, std::streamsize n) override {
        if (console_) console_->sputn(s, n);
        if (file_) file_->sputn(s, n);
        return n;
    }
    int sync() override {
        if (console_) console_->pubsync();
        if (file_) file_->pubsync();
        return 0;
    }

private:
    std::streambuf* console_;
    std::streambuf* file_;
};

struct LogState {
    std::ofstream file;
    std::unique_ptr<TeeBuf> out_tee, err_tee;
    std::streambuf* orig_out = nullptr;
    std::streambuf* orig_err = nullptr;
    std::filesystem::path path;
};

inline LogState& log_state() {
    static LogState s;
    return s;
}

/// Crash file path, preformatted so the signal handler only open()s it (async-signal-safe).
inline char g_crash_path[1024] = {0};

inline void write_str_(int fd, const char* s) {
    if (fd >= 0 && s) { ssize_t r = ::write(fd, s, std::strlen(s)); (void)r; }
}

inline const char* signal_name_(int sig) {
    switch (sig) {
        case SIGSEGV: return "SIGSEGV (segmentation fault)";
        case SIGBUS:  return "SIGBUS (bus error)";
        case SIGABRT: return "SIGABRT (abort)";
        case SIGFPE:  return "SIGFPE (arithmetic error)";
        case SIGILL:  return "SIGILL (illegal instruction)";
        default:      return "fatal signal";
    }
}

inline int open_crash_file_() {
    if (!g_crash_path[0]) return -1;
    return ::open(g_crash_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
}

inline void write_backtrace_(int fd) {
    void* frames[64];
    const int n = ::backtrace(frames, 64);
    write_str_(fd, "\nBacktrace:\n");
    ::backtrace_symbols_fd(frames, n, fd);
    write_str_(STDERR_FILENO, "\nBacktrace:\n");
    ::backtrace_symbols_fd(frames, n, STDERR_FILENO);
}

inline void on_fatal_signal_(int sig) {
    const int fd = open_crash_file_();
    write_str_(fd, "Fatal: ");
    write_str_(fd, signal_name_(sig));
    write_str_(fd, "\n");
    write_str_(STDERR_FILENO, "[toyengine] Fatal: ");
    write_str_(STDERR_FILENO, signal_name_(sig));
    write_str_(STDERR_FILENO, "\n");
    write_backtrace_(fd);
    if (fd >= 0) ::close(fd);
    // Die the way we would have: default disposition, re-raised.
    std::signal(sig, SIG_DFL);
    std::raise(sig);
}

inline void on_terminate_() {
    const int fd = open_crash_file_();
    write_str_(fd, "Fatal: std::terminate");
    if (std::exception_ptr ep = std::current_exception()) {
        try { std::rethrow_exception(ep); }
        catch (const std::exception& e) { write_str_(fd, " -- uncaught exception: "); write_str_(fd, e.what()); }
        catch (...) { write_str_(fd, " -- uncaught non-std exception"); }
    }
    write_str_(fd, "\n");
    write_backtrace_(fd);
    if (fd >= 0) ::close(fd);
    std::signal(SIGABRT, SIG_DFL);   // the abort below must not write a second report
    std::abort();
}

} // namespace detail

/**
 * @brief Mirrors std::cout / std::cerr into `<dir>/<name>.log` (rotating the previous run's to
 *        `<name>.prev.log`). `console` false (shipping) writes the file only.
 * @return The log file's path, or empty if it couldn't be opened (logging stays on the console).
 */
inline std::filesystem::path install_runtime_log(const std::string& name,
                                                 const std::filesystem::path& dir = user_log_dir(),
                                                 bool console = !k_shipping) {
    namespace fs = std::filesystem;
    detail::LogState& st = detail::log_state();
    if (st.file.is_open()) return st.path;
    std::error_code ec;
    fs::create_directories(dir, ec);
    const fs::path path = dir / (name + ".log");
    if (fs::exists(path, ec)) fs::rename(path, dir / (name + ".prev.log"), ec);
    st.file.open(path, std::ios::out | std::ios::trunc);
    if (!st.file) return {};
    st.path = path;
    st.orig_out = std::cout.rdbuf();
    st.orig_err = std::cerr.rdbuf();
    st.out_tee = std::make_unique<detail::TeeBuf>(console ? st.orig_out : nullptr, st.file.rdbuf());
    st.err_tee = std::make_unique<detail::TeeBuf>(console ? st.orig_err : nullptr, st.file.rdbuf());
    std::cout.rdbuf(st.out_tee.get());
    std::cerr.rdbuf(st.err_tee.get());
    std::cerr << std::unitbuf;   // stderr lines reach the file even if the process dies next
    return path;
}

/** @brief Restores std::cout / std::cerr and closes the log (also safe if never installed). */
inline void shutdown_runtime_log() {
    detail::LogState& st = detail::log_state();
    if (!st.file.is_open()) return;
    std::cout.flush();
    std::cerr.flush();
    std::cout.rdbuf(st.orig_out);
    std::cerr.rdbuf(st.orig_err);
    st.out_tee.reset();
    st.err_tee.reset();
    st.file.close();
}

/**
 * @brief Writes `<dir>/crash-<pid>.txt` on SIGSEGV/SIGBUS/SIGABRT/SIGFPE/SIGILL or
 *        std::terminate, then lets the process die as it otherwise would.
 */
inline void install_crash_handlers(const std::filesystem::path& dir = user_log_dir()) {
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const std::string path = (dir / ("crash-" + std::to_string(::getpid()) + ".txt")).string();
    std::snprintf(detail::g_crash_path, sizeof(detail::g_crash_path), "%s", path.c_str());
    for (int sig : {SIGSEGV, SIGBUS, SIGABRT, SIGFPE, SIGILL}) std::signal(sig, detail::on_fatal_signal_);
    std::set_terminate(detail::on_terminate_);
}

/**
 * @brief Appends a fatal error message to the crash file (for a caught top-level exception,
 *        which is not a crash the signal handlers see).
 */
inline void write_crash_report(const std::string& message) {
    const int fd = detail::open_crash_file_();
    if (fd < 0) return;
    detail::write_str_(fd, "Fatal: ");
    detail::write_str_(fd, message.c_str());
    detail::write_str_(fd, "\n");
    ::close(fd);
}

} // namespace toy::core

#endif // TOYENGINE_CORE_RUNTIME_LOG_H
