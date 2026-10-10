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
    int overflow(int c) override;
    std::streamsize xsputn(const char* s, std::streamsize n) override;
    int sync() override;

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

LogState& log_state();

/// Crash file path, preformatted so the signal handler only open()s it (async-signal-safe).
inline char g_crash_path[1024] = {0};

void write_str_(int fd, const char* s);

const char* signal_name_(int sig);

int open_crash_file_();

void write_backtrace_(int fd);

void on_fatal_signal_(int sig);

void on_terminate_();

} // namespace detail

/**
 * @brief Mirrors std::cout / std::cerr into `<dir>/<name>.log` (rotating the previous run's to
 *        `<name>.prev.log`). `console` false (shipping) writes the file only.
 * @return The log file's path, or empty if it couldn't be opened (logging stays on the console).
 */
std::filesystem::path install_runtime_log(const std::string& name,
                                                 const std::filesystem::path& dir = user_log_dir(),
                                                 bool console = !k_shipping);

/** @brief Restores std::cout / std::cerr and closes the log (also safe if never installed). */
void shutdown_runtime_log();

/**
 * @brief Writes `<dir>/crash-<pid>.txt` on SIGSEGV/SIGBUS/SIGABRT/SIGFPE/SIGILL or
 *        std::terminate, then lets the process die as it otherwise would.
 */
void install_crash_handlers(const std::filesystem::path& dir = user_log_dir());

/**
 * @brief Appends a fatal error message to the crash file (for a caught top-level exception,
 *        which is not a crash the signal handlers see).
 */
void write_crash_report(const std::string& message);

} // namespace toy::core

#endif // TOYENGINE_CORE_RUNTIME_LOG_H
