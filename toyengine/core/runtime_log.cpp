#include <toyengine/core/runtime_log.h>

namespace toy {
namespace core {
namespace detail {

int TeeBuf::overflow(int c) {
    if (c == traits_type::eof()) return traits_type::not_eof(c);
    if (console_ && console_->sputc(static_cast<char>(c)) == traits_type::eof()) console_ = nullptr;
    if (file_) file_->sputc(static_cast<char>(c));
    return c;
}

std::streamsize TeeBuf::xsputn(const char* s, std::streamsize n) {
    if (console_) console_->sputn(s, n);
    if (file_) file_->sputn(s, n);
    return n;
}

int TeeBuf::sync() {
    if (console_) console_->pubsync();
    if (file_) file_->pubsync();
    return 0;
}

LogState& log_state() {
    static LogState s;
    return s;
}

void write_str_(int fd, const char* s) {
    if (fd >= 0 && s) { ssize_t r = ::write(fd, s, std::strlen(s)); (void)r; }
}

const char* signal_name_(int sig) {
    switch (sig) {
        case SIGSEGV: return "SIGSEGV (segmentation fault)";
        case SIGBUS:  return "SIGBUS (bus error)";
        case SIGABRT: return "SIGABRT (abort)";
        case SIGFPE:  return "SIGFPE (arithmetic error)";
        case SIGILL:  return "SIGILL (illegal instruction)";
        default:      return "fatal signal";
    }
}

int open_crash_file_() {
    if (!g_crash_path[0]) return -1;
    return ::open(g_crash_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
}

void write_backtrace_(int fd) {
    void* frames[64];
    const int n = ::backtrace(frames, 64);
    write_str_(fd, "\nBacktrace:\n");
    ::backtrace_symbols_fd(frames, n, fd);
    write_str_(STDERR_FILENO, "\nBacktrace:\n");
    ::backtrace_symbols_fd(frames, n, STDERR_FILENO);
}

void on_fatal_signal_(int sig) {
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

void on_terminate_() {
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
} // namespace core
} // namespace toy

namespace toy {
namespace core {

std::filesystem::path install_runtime_log(const std::string& name,
                                                 const std::filesystem::path& dir,
                                                 bool console) {
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

void shutdown_runtime_log() {
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

void install_crash_handlers(const std::filesystem::path& dir) {
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const std::string path = (dir / ("crash-" + std::to_string(::getpid()) + ".txt")).string();
    std::snprintf(detail::g_crash_path, sizeof(detail::g_crash_path), "%s", path.c_str());
    for (int sig : {SIGSEGV, SIGBUS, SIGABRT, SIGFPE, SIGILL}) std::signal(sig, detail::on_fatal_signal_);
    std::set_terminate(detail::on_terminate_);
}

void write_crash_report(const std::string& message) {
    const int fd = detail::open_crash_file_();
    if (fd < 0) return;
    detail::write_str_(fd, "Fatal: ");
    detail::write_str_(fd, message.c_str());
    detail::write_str_(fd, "\n");
    ::close(fd);
}

} // namespace core
} // namespace toy
