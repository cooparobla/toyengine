/**
 * @file imm_file_dialog.h
 * @brief A file / folder picker drawn with the immediate-mode layer (a modal), laid out
 *        like the system's Open / Save panels: back / forward / up, a clickable path bar and a
 *        live search field on top; Favorites, Recent folders and Locations in a sidebar; a
 *        sortable Name / Date Modified / Size / Kind list; a Format menu and Cancel / Open at
 *        the bottom. Files the format filter doesn't accept are listed greyed out, as macOS does.
 *
 *        Keys: Up / Down select, Enter or Cmd+Down opens, Cmd+Up goes to the enclosing folder,
 *        Cmd+[ / Cmd+] back / forward, typing selects by name, "/" or "~" or Shift+Cmd+G types a
 *        path, Shift+Cmd+. shows hidden files, Shift+Cmd+H / D jump Home / to the Desktop.
 *        Shift+Cmd+N (or New Folder in the list's right-click menu) makes an "untitled folder"
 *        and starts renaming it in place, as Finder does.
 *
 *        Usage: keep one FileDialog, call draw(ctx) every frame (after the rest of the UI, so
 *        the modal is on top), and open() it from anywhere -- a button, a menu item:
 *        @code
 *        dialog.open(ctx, FileDialog::Mode::OpenFile, "Open Scene", start_dir, {".yaml", ".caml"},
 *                    [&](const std::filesystem::path& p) { load(p); });
 *        @endcode
 *        An app names its own document types with `kind_name` / `format_group` (the Kind column
 *        and the Format menu), and adds sidebar Favorites with `favorites` / `launch_dir`.
 */

#ifndef UICOOPA_IMMEDIATE_IMM_FILE_DIALOG_H
#define UICOOPA_IMMEDIATE_IMM_FILE_DIALOG_H

#include <uicoopa/immediate/imm.h>

#include <coopa/input/keys.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <functional>
#include <string>
#include <system_error>
#include <vector>

namespace coopa::ui::imm {

namespace file_dialog_detail {

inline std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

/** @brief Finder-style "Kind" for a file (FileDialog::kind_name can name an app's own types). */
inline std::string kind_of(const std::string& ext_lower) {
    if (ext_lower == ".yaml" || ext_lower == ".yml") return "YAML Document";
    if (ext_lower == ".caml") return "caml Document";
    if (ext_lower == ".ai") return "Adobe Illustrator Document";
    if (ext_lower == ".pdf") return "PDF Document";
    if (ext_lower == ".svg") return "SVG Image";
    if (ext_lower == ".png") return "PNG Image";
    if (ext_lower == ".jpg" || ext_lower == ".jpeg") return "JPEG Image";
    if (ext_lower == ".gif") return "GIF Image";
    if (ext_lower == ".bmp") return "BMP Image";
    if (ext_lower == ".tga") return "TGA Image";
    if (ext_lower == ".psd") return "Photoshop Document";
    if (ext_lower == ".txt") return "Plain Text";
    if (ext_lower.size() > 1) {
        std::string e = ext_lower.substr(1);
        for (char& c : e) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        return e + " File";
    }
    return "Document";
}

inline bool is_image_ext(const std::string& e) {
    return e == ".png" || e == ".jpg" || e == ".jpeg" || e == ".gif" || e == ".bmp" || e == ".tga" || e == ".psd" || e == ".svg";
}

/** @brief The Format menu groups extensions the way the system panels do. */
inline std::string group_of(const std::string& e) {
    if (e == ".yaml" || e == ".yml" || e == ".caml") return "YAML Documents";
    if (e == ".ai") return "Adobe Illustrator";
    if (e == ".pdf") return "PDF";
    if (e == ".svg") return "SVG";
    if (is_image_ext(e)) return "Images";
    return kind_of(e);
}

/** @brief "4.2 MB", decimal units like Finder. */
inline std::string format_size(uintmax_t n) {
    char buf[32];
    if (n < 1000) std::snprintf(buf, sizeof buf, "%ju bytes", static_cast<uintmax_t>(n));
    else {
        static const char* units[] = {"KB", "MB", "GB", "TB"};
        double v = static_cast<double>(n) / 1000.0;
        int u = 0;
        while (v >= 1000.0 && u < 3) { v /= 1000.0; ++u; }
        std::snprintf(buf, sizeof buf, v < 10.0 ? "%.1f %s" : "%.0f %s", v, units[u]);
    }
    return buf;
}

/** @brief "Today at 14:03", "Yesterday at 09:12", "4 Oct 2026 at 18:08". */
inline std::string format_time(std::time_t t) {
    if (t <= 0) return "--";
    std::tm tm{}, now{};
    const std::time_t n = std::time(nullptr);
#ifdef _WIN32
    localtime_s(&tm, &t);
    localtime_s(&now, &n);
#else
    localtime_r(&t, &tm);
    localtime_r(&n, &now);
#endif
    static const char* months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    char buf[64];
    std::tm yest = now;
    yest.tm_mday -= 1;
    std::mktime(&yest);
    if (tm.tm_year == now.tm_year && tm.tm_yday == now.tm_yday) std::snprintf(buf, sizeof buf, "Today at %02d:%02d", tm.tm_hour, tm.tm_min);
    else if (tm.tm_year == yest.tm_year && tm.tm_yday == yest.tm_yday) std::snprintf(buf, sizeof buf, "Yesterday at %02d:%02d", tm.tm_hour, tm.tm_min);
    else std::snprintf(buf, sizeof buf, "%d %s %d at %02d:%02d", tm.tm_mday, months[tm.tm_mon], tm.tm_year + 1900, tm.tm_hour, tm.tm_min);
    return buf;
}

inline std::time_t to_time_t(std::filesystem::file_time_type ft) {
    using namespace std::chrono;
    const auto sys = time_point_cast<system_clock::duration>(ft - std::filesystem::file_time_type::clock::now() + system_clock::now());
    return system_clock::to_time_t(sys);
}

inline std::filesystem::path home_dir() {
    const char* h = std::getenv("HOME");
#ifdef _WIN32
    if (!h || !*h) h = std::getenv("USERPROFILE");
#endif
    return h && *h ? std::filesystem::path(h) : std::filesystem::path();
}

inline bool is_dir(const std::filesystem::path& p) {
    std::error_code ec;
    return !p.empty() && std::filesystem::is_directory(p, ec);
}

/** @brief Display name of a folder ("/" is the startup disk). */
inline std::string folder_label(const std::filesystem::path& p) {
    const std::string n = p.filename().string();
    if (!n.empty()) return n;
#ifdef __APPLE__
    if (p == p.root_path()) return "Macintosh HD";
#endif
    return p.string();
}

} // namespace file_dialog_detail

class FileDialog {
public:
    enum class Mode { OpenFile, SaveFile, PickFolder };

    /** @brief A sidebar entry. */
    struct Place {
        std::string label;
        std::filesystem::path path;
        Icon icon = Icon::Folder;
        std::string tip;                   ///< Tooltip line under the label (empty: the path)
    };

    /** @brief Shown first under Favorites: the folder the app was launched from (empty: none). */
    std::filesystem::path launch_dir;
    /** @brief App-specific Favorites (a project folder, ...), listed after launch_dir and before
     *         Home / Desktop / Documents / Downloads / Pictures. Missing folders are skipped. */
    std::vector<Place> favorites;
    /** @brief Sidebar "Recent" folders, most recent first. */
    std::vector<std::filesystem::path> recent_dirs;
    /** @brief Called when the dialog finishes, with the folder it finished in and its mode. */
    std::function<void(const std::filesystem::path&, Mode)> on_folder_used;
    /** @brief The Kind column for a lower-case extension (".yaml"); empty result or unset: the
     *         built-in names ("PNG Image", "YAML Document", ...). */
    std::function<std::string(const std::string&)> kind_name;
    /** @brief The Format-menu group for a lower-case extension (extensions in one group share an
     *         entry); empty result or unset: the built-in grouping. */
    std::function<std::string(const std::string&)> format_group;

    /**
     * @param ext Accepted extensions (".yaml"); empty accepts anything. Ignored for folders.
     * @param on_done Called with the chosen path.
     */
    void open(Context& ctx, Mode mode, std::string title, const std::filesystem::path& start,
              std::vector<std::string> ext, std::function<void(const std::filesystem::path&)> on_done,
              std::string default_name = {}) {
        using namespace file_dialog_detail;
        mode_ = mode;
        title_ = std::move(title);
        ext_.clear();
        for (const auto& e : ext) ext_.push_back(lower(e));
        on_done_ = std::move(on_done);
        build_filters_();
        std::error_code ec;
        std::filesystem::path d = is_dir(start) ? start : start.parent_path();
        if (!is_dir(d)) d = home_dir();
        if (!is_dir(d)) d = std::filesystem::current_path(ec);
        dir_ = std::filesystem::absolute(d, ec).lexically_normal();
        if (dir_.has_filename() == false && dir_ != dir_.root_path()) dir_ = dir_.parent_path();
        name_ = default_name;
        if (mode_ == Mode::SaveFile && !name_.empty()) {
            // Start on the format of the suggested name.
            const std::string e = lower(std::filesystem::path(name_).extension().string());
            for (size_t i = 0; i < filters_.size(); ++i)
                if (std::find(filters_[i].ext.begin(), filters_[i].ext.end(), e) != filters_[i].ext.end()) { filter_ = static_cast<int>(i); break; }
        }
        back_.clear();
        fwd_.clear();
        selected_.clear();
        search_.clear();
        search_focus_ = false;
        typeahead_.clear();   // type-to-select starts fresh in every dialog
        editing_path_ = false;
        replace_armed_.clear();
        renaming_.clear();
        notice_.clear();
        scroll_ = 0.0f;
        refresh_();
        // Opened from draw() on the next frame: called from inside a menu, an immediate
        // open_modal() would be dismissed along with the menu popup.
        (void)ctx;
        pending_open_ = true;
        focus_name_ = mode_ == Mode::SaveFile;
        open_ = true;
    }

    bool is_open() const { return open_; }

    /** @brief Draws the dialog if open. Call every frame. */
    void draw(Context& ctx) {
        if (!open_) return;
        if (pending_open_) {
            ctx.open_modal(title_ + "##filedialog");
            pending_open_ = false;
        }
        const glm::vec2 cs = ctx.canvas_size();
        const glm::vec2 size(std::min(960.0f, cs.x - 60), std::min(620.0f, cs.y - 60));
        if (!ctx.begin_modal(title_ + "##filedialog", size)) { open_ = false; return; }
        const Style& st = ctx.style;
        const Box R = ctx.content_region();
        const float bar_h = st.row_height + 3;
        const float gap = 6.0f;

        handle_keys_(ctx);

        Box toolbar{R.x, R.y, R.w, bar_h};
        Box bottom{R.x, R.bottom() - bar_h, R.w, bar_h};
        float body_bottom = bottom.y - gap;
        Box name_row{};
        if (mode_ == Mode::SaveFile) {
            name_row = {R.x, bottom.y - gap - bar_h, R.w, bar_h};
            body_bottom = name_row.y - gap;
        }
        const Box body{R.x, toolbar.bottom() + gap, R.w, body_bottom - (toolbar.bottom() + gap)};
        const float side_w = std::min(180.0f, body.w * 0.25f);
        const Box side{body.x, body.y, side_w, body.h};
        const Box list{side.right() + gap, body.y, body.w - side_w - gap, body.h};

        draw_toolbar_(ctx, toolbar);
        draw_sidebar_(ctx, side);
        draw_list_(ctx, list);
        if (mode_ == Mode::SaveFile) draw_name_row_(ctx, name_row);
        draw_bottom_(ctx, bottom);

        if (!open_) ctx.close_modal();
        ctx.end_modal();
    }

private:
    struct Entry {
        std::string name, lname, ext;   ///< lname: lower-case name (sorting, search)
        bool dir = false;
        bool ok = true;                 ///< Openable under the current filter (always, for folders)
        uintmax_t size = 0;
        std::time_t mtime = 0;
        std::string kind;
    };
    struct Filter { std::string label; std::vector<std::string> ext; };   ///< ext empty: all files

    // =========================================================================================
    // Model
    // =========================================================================================

    void build_filters_() {
        using namespace file_dialog_detail;
        filters_.clear();
        filter_ = 0;
        if (mode_ == Mode::PickFolder) return;
        if (ext_.empty()) { filters_.push_back({"All Files", {}}); return; }
        auto list_of = [](const std::vector<std::string>& v) {
            std::string s;
            for (const auto& e : v) s += (s.empty() ? "" : ", ") + e;
            return s;
        };
        if (mode_ == Mode::SaveFile) {
            // One format per extension: the choice decides what's written.
            for (const auto& e : ext_) filters_.push_back({kind_(e) + " (" + e + ")", {e}});
            return;
        }
        if (ext_.size() > 1) filters_.push_back({"All Readable Documents", ext_});
        std::vector<Filter> groups;
        for (const auto& e : ext_) {
            const std::string g = group_(e);
            auto it = std::find_if(groups.begin(), groups.end(), [&](const Filter& f) { return f.label == g; });
            if (it == groups.end()) groups.push_back({g, {e}});
            else it->ext.push_back(e);
        }
        for (auto& g : groups) {
            g.label += " (" + list_of(g.ext) + ")";
            filters_.push_back(g);
        }
        filters_.push_back({"All Files", {}});
    }

    std::string kind_(const std::string& e) const {
        if (kind_name) { std::string k = kind_name(e); if (!k.empty()) return k; }
        return file_dialog_detail::kind_of(e);
    }
    std::string group_(const std::string& e) const {
        if (format_group) { std::string g = format_group(e); if (!g.empty()) return g; }
        // An app-named kind groups by that name, so its extensions share one Format entry.
        if (kind_name) { std::string k = kind_name(e); if (!k.empty()) return k; }
        return file_dialog_detail::group_of(e);
    }

    bool accepts_(const std::string& ext_lower) const {
        if (mode_ == Mode::PickFolder) return false;
        if (filters_.empty()) return true;
        const auto& f = filters_[static_cast<size_t>(std::clamp(filter_, 0, static_cast<int>(filters_.size()) - 1))].ext;
        return f.empty() || std::find(f.begin(), f.end(), ext_lower) != f.end();
    }

    void refresh_() {
        using namespace file_dialog_detail;
        all_.clear();
        error_.clear();
        std::error_code ec;
        std::filesystem::directory_iterator it(dir_, ec);
        if (ec) { error_ = "The folder can't be opened: " + ec.message() + "."; apply_view_(); return; }
        for (; !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
            Entry e;
            e.name = it->path().filename().string();
            if (e.name.empty() || (!show_hidden_ && e.name[0] == '.')) continue;
            std::error_code ec2;
            e.dir = it->is_directory(ec2);
            e.lname = lower(e.name);
            if (!e.dir) {
                e.ext = lower(it->path().extension().string());
                e.size = it->file_size(ec2);
                if (ec2) e.size = 0;
            }
            const auto ft = it->last_write_time(ec2);
            if (!ec2) e.mtime = to_time_t(ft);
            e.kind = e.dir ? "Folder" : kind_(e.ext);
            all_.push_back(std::move(e));
        }
        apply_view_();
    }

    /** @brief Filter (format, search) and sort `all_` into `view_`. */
    void apply_view_() {
        using namespace file_dialog_detail;
        const std::string sel = selected_;
        view_.clear();
        const std::string q = lower(search_);
        for (Entry e : all_) {
            if (!q.empty() && e.lname.find(q) == std::string::npos) continue;
            e.ok = e.dir || accepts_(e.ext);
            view_.push_back(std::move(e));
        }
        const int col = sort_col_;
        const bool asc = sort_asc_;
        std::stable_sort(view_.begin(), view_.end(), [&](const Entry& a, const Entry& b) {
            if (a.dir != b.dir) return a.dir;   // folders first
            int c = 0;
            switch (col) {
                case 1: c = a.mtime < b.mtime ? -1 : a.mtime > b.mtime ? 1 : 0; break;
                case 2: c = a.size < b.size ? -1 : a.size > b.size ? 1 : 0; break;
                case 3: c = a.kind.compare(b.kind); break;
                default: break;
            }
            if (c == 0) c = a.lname.compare(b.lname);
            return asc ? c < 0 : c > 0;
        });
        selected_index_ = -1;
        for (size_t i = 0; i < view_.size(); ++i) if (view_[i].name == sel) selected_index_ = static_cast<int>(i);
        if (selected_index_ < 0) selected_.clear();
    }

    void select_(int i) {
        if (i < 0 || i >= static_cast<int>(view_.size())) return;
        selected_index_ = i;
        selected_ = view_[static_cast<size_t>(i)].name;
        replace_armed_.clear();
        if (mode_ == Mode::SaveFile && !view_[static_cast<size_t>(i)].dir) name_ = selected_;
        scroll_to_ = i;
    }

    void navigate_(const std::filesystem::path& p, bool record = true) {
        using namespace file_dialog_detail;
        std::error_code ec;
        std::filesystem::path q = std::filesystem::absolute(p, ec).lexically_normal();
        if (q.has_filename() == false && q != q.root_path()) q = q.parent_path();
        if (!is_dir(q) || q == dir_) return;
        if (record) { back_.push_back(dir_); fwd_.clear(); }
        const std::filesystem::path from = dir_;
        dir_ = q;
        search_.clear();
        selected_.clear();
        replace_armed_.clear();
        renaming_.clear();
        notice_.clear();
        scroll_ = 0.0f;
        refresh_();
        // Going up selects the folder we came from, as Finder does.
        if (from.parent_path() == dir_)
            for (size_t i = 0; i < view_.size(); ++i)
                if (view_[i].name == from.filename().string()) { select_(static_cast<int>(i)); break; }
    }

    void go_back_() {
        if (back_.empty()) return;
        fwd_.push_back(dir_);
        const auto p = back_.back();
        back_.pop_back();
        navigate_(p, false);
    }
    void go_forward_() {
        if (fwd_.empty()) return;
        back_.push_back(dir_);
        const auto p = fwd_.back();
        fwd_.pop_back();
        navigate_(p, false);
    }
    void go_up_() { if (dir_ != dir_.root_path()) navigate_(dir_.parent_path()); }

    /** @brief Open / Save / Choose (button, Enter, double-click). */
    void accept_() {
        const Entry* sel = selected_index_ >= 0 ? &view_[static_cast<size_t>(selected_index_)] : nullptr;
        if (mode_ == Mode::PickFolder) {
            finish_(sel && sel->dir ? dir_ / sel->name : dir_);
            return;
        }
        if (mode_ == Mode::OpenFile) {
            if (!sel) return;
            if (sel->dir) navigate_(dir_ / sel->name);
            else if (sel->ok) finish_(dir_ / sel->name);
            return;
        }
        // Save.
        if (sel && sel->dir && (name_.empty() || name_ == sel->name)) { navigate_(dir_ / sel->name); return; }
        if (name_.empty()) return;
        std::filesystem::path p = dir_ / name_;
        if (!filters_.empty()) {
            const auto& f = filters_[static_cast<size_t>(filter_)].ext;
            if (!f.empty() && std::find(f.begin(), f.end(), file_dialog_detail::lower(p.extension().string())) == f.end()) p += f.front();
        }
        std::error_code ec;
        if (std::filesystem::exists(p, ec) && replace_armed_ != p.string()) {
            replace_armed_ = p.string();   // the second press replaces
            return;
        }
        finish_(p);
    }

    bool can_accept_() const {
        const Entry* sel = selected_index_ >= 0 ? &view_[static_cast<size_t>(selected_index_)] : nullptr;
        switch (mode_) {
            case Mode::PickFolder: return true;
            case Mode::OpenFile: return sel && (sel->dir || sel->ok);
            case Mode::SaveFile: return !name_.empty() || (sel && sel->dir);
        }
        return false;
    }

    void finish_(const std::filesystem::path& p) {
        open_ = false;
        if (on_folder_used) on_folder_used(mode_ == Mode::PickFolder ? p : p.parent_path(), mode_);
        if (on_done_) on_done_(p);
    }

    void cancel_() { open_ = false; }

    /** @brief Makes "untitled folder" (or "untitled folder 2", ...) here, selects it and starts
     *         renaming it, as Finder's New Folder does. */
    void new_folder_() {
        std::error_code ec;
        std::string name = "untitled folder";
        for (int n = 2; std::filesystem::exists(dir_ / name, ec); ++n) name = "untitled folder " + std::to_string(n);
        if (!std::filesystem::create_directory(dir_ / name, ec) || ec) {
            notice_ = "Couldn't create a folder here: " + (ec ? ec.message() : std::string("it already exists")) + ".";
            return;
        }
        search_.clear();   // the new folder must be listed to be renamed
        refresh_();
        for (size_t i = 0; i < view_.size(); ++i)
            if (view_[i].name == name) { select_(static_cast<int>(i)); break; }
        start_rename_(name);
    }

    void start_rename_(const std::string& name) {
        renaming_ = name;
        rename_buf_ = name;
        rename_focus_ = true;
        notice_.clear();
    }

    /** @brief Renames `renaming_` to `rename_buf_`; an unusable name keeps the old one. */
    void finish_rename_() {
        const std::string from = renaming_;
        renaming_.clear();
        std::string to = rename_buf_;
        while (!to.empty() && std::isspace(static_cast<unsigned char>(to.back()))) to.pop_back();
        while (!to.empty() && std::isspace(static_cast<unsigned char>(to.front()))) to.erase(to.begin());
        if (to.empty() || to == from) return;
        if (to == "." || to == ".." || to.find_first_of("/\\:") != std::string::npos) {
            notice_ = "\"" + to + "\" can't be used as a name.";
            return;
        }
        std::error_code ec;
        if (std::filesystem::exists(dir_ / to, ec)) {
            notice_ = "\"" + to + "\" already exists.";
            return;
        }
        std::filesystem::rename(dir_ / from, dir_ / to, ec);
        if (ec) { notice_ = "Couldn't rename \"" + from + "\": " + ec.message() + "."; return; }
        refresh_();
        for (size_t i = 0; i < view_.size(); ++i)
            if (view_[i].name == to) { select_(static_cast<int>(i)); break; }
    }

    // =========================================================================================
    // Keyboard
    // =========================================================================================

    void handle_keys_(Context& ctx) {
        using coopa::input::Key;
        using coopa::input::Mods;
        if (ctx.wants_keyboard()) return;   // a text field (path, file name) has it
        const Mods cmd = Context::command_mod();
        auto pressed = [&](Key k, Mods m = Mods::None) {
            if (ctx.key_pressed(k, m, true)) return true;
            // Control works in place of Command (Linux / Windows keyboards, and habit on macOS).
            if (has(m, Mods::Super)) {
                const Mods alt = static_cast<Mods>((static_cast<uint8_t>(m) & ~static_cast<uint8_t>(Mods::Super)) | static_cast<uint8_t>(Mods::Control));
                return ctx.key_pressed(k, alt, true);
            }
            return false;
        };
        if (search_focus_) {
            // The search field is ours (live filtering needs every keystroke).
            bool changed = false;
            const bool command_held = has(ctx.input().mods, cmd) || has(ctx.input().mods, Mods::Control);
            if (!command_held)
                for (uint32_t c : ctx.input().chars)
                    if (c >= 32 && c <= 126) { search_.push_back(static_cast<char>(c)); changed = true; }
            if (pressed(Key::Backspace) && !search_.empty()) { search_.pop_back(); changed = true; }
            if (pressed(Key::Escape)) {
                if (search_.empty()) search_focus_ = false;
                else { search_.clear(); changed = true; }
            }
            if (changed) { apply_view_(); if (selected_index_ < 0 && !view_.empty()) select_(0); scroll_ = 0.0f; }
        } else {
            if (pressed(Key::Escape)) { cancel_(); return; }
            // Type to select (Finder), or start typing a path.
            const bool command_held = has(ctx.input().mods, cmd) || has(ctx.input().mods, Mods::Control);
            if (!command_held) {
                for (uint32_t c : ctx.input().chars) {
                    if (c == '/' || c == '~') {
                        editing_path_ = true;
                        path_buf_ = std::string(1, static_cast<char>(c));
                        path_focus_ = true;
                        break;
                    }
                    if (c < 32 || c > 126) continue;
                    if (ctx.time() - typeahead_time_ > 1.0) typeahead_.clear();
                    typeahead_.push_back(static_cast<char>(std::tolower(static_cast<int>(c))));
                    typeahead_time_ = ctx.time();
                    for (size_t i = 0; i < view_.size(); ++i)
                        if (view_[i].lname.compare(0, typeahead_.size(), typeahead_) == 0) { select_(static_cast<int>(i)); break; }
                }
            }
        }
        if (pressed(Key::Down) && !view_.empty()) select_(std::min(static_cast<int>(view_.size()) - 1, selected_index_ + 1));
        if (pressed(Key::Up) && !view_.empty()) select_(std::max(0, selected_index_ < 0 ? 0 : selected_index_ - 1));
        if (pressed(Key::Home) && !view_.empty()) select_(0);
        if (pressed(Key::End) && !view_.empty()) select_(static_cast<int>(view_.size()) - 1);
        if (pressed(Key::Enter) || pressed(Key::KpEnter) || pressed(Key::Down, cmd)) { if (can_accept_()) accept_(); }
        if (pressed(Key::Up, cmd)) go_up_();
        if (pressed(Key::LeftBracket, cmd)) go_back_();
        if (pressed(Key::RightBracket, cmd)) go_forward_();
        if (pressed(Key::G, cmd | Mods::Shift)) { editing_path_ = true; path_buf_ = dir_.string(); path_focus_ = true; }
        if (pressed(Key::H, cmd | Mods::Shift)) navigate_(file_dialog_detail::home_dir());
        if (pressed(Key::D, cmd | Mods::Shift)) navigate_(file_dialog_detail::home_dir() / "Desktop");
        if (pressed(Key::Period, cmd | Mods::Shift)) { show_hidden_ = !show_hidden_; refresh_(); }
        if (pressed(Key::N, cmd | Mods::Shift)) new_folder_();
        if (pressed(Key::F, cmd)) search_focus_ = true;
    }

    // =========================================================================================
    // Drawing
    // =========================================================================================

    /** @brief A chevron ("<", ">", "^", "v") centred in `b`. dir: 0 left, 1 right, 2 up, 3 down. */
    static void chevron_(Context& ctx, const Box& b, int dir, const glm::vec4& c, float s = 4.0f) {
        const glm::vec2 m{b.x + b.w * 0.5f, b.y + b.h * 0.5f};
        const float t = 1.5f;
        switch (dir) {
            case 0: ctx.line({m.x + s * 0.5f, m.y - s}, {m.x - s * 0.5f, m.y}, c, t); ctx.line({m.x - s * 0.5f, m.y}, {m.x + s * 0.5f, m.y + s}, c, t); break;
            case 1: ctx.line({m.x - s * 0.5f, m.y - s}, {m.x + s * 0.5f, m.y}, c, t); ctx.line({m.x + s * 0.5f, m.y}, {m.x - s * 0.5f, m.y + s}, c, t); break;
            case 2: ctx.line({m.x - s, m.y + s * 0.5f}, {m.x, m.y - s * 0.5f}, c, t); ctx.line({m.x, m.y - s * 0.5f}, {m.x + s, m.y + s * 0.5f}, c, t); break;
            default: ctx.line({m.x - s, m.y - s * 0.5f}, {m.x, m.y + s * 0.5f}, c, t); ctx.line({m.x, m.y + s * 0.5f}, {m.x + s, m.y - s * 0.5f}, c, t); break;
        }
    }

    /** @brief A flat toolbar button holding a chevron. */
    bool nav_button_(Context& ctx, const char* id, const Box& b, int dir, bool enabled, const char* tip) {
        bool hov = false, held = false;
        const bool clicked = ctx.invisible_button(id, b, &hov, &held);
        const Style& st = ctx.style;
        ctx.fill_rounded(b, held && enabled ? st.button_hover : hov && enabled ? st.button : st.header);
        chevron_(ctx, b, dir, enabled ? st.text : st.text_disabled);
        if (hov) ctx.tooltip(tip);
        return clicked && enabled;
    }

    /** @brief The accent-coloured default button (Open / Save). */
    bool primary_button_(Context& ctx, const char* label, const Box& b, bool enabled) {
        bool hov = false, held = false;
        const bool clicked = ctx.invisible_button(label, b, &hov, &held);
        const Style& st = ctx.style;
        glm::vec4 c = st.accent;
        if (!enabled) c = with_alpha(st.accent, 0.35f);
        else if (held) c = c * 0.85f + glm::vec4(0, 0, 0, 0.15f);
        else if (hov) c = glm::mix(c, glm::vec4(1.0f), 0.12f);
        ctx.fill_rounded(b, c);
        ctx.text_in(b, label, enabled ? glm::vec4(1.0f) : st.text_disabled, 0, true);
        return clicked && enabled;
    }

    bool flat_button_(Context& ctx, const char* label, const Box& b) {
        bool hov = false, held = false;
        const bool clicked = ctx.invisible_button(label, b, &hov, &held);
        const Style& st = ctx.style;
        ctx.fill_rounded(b, held ? st.button_active : hov ? st.button_hover : st.button);
        ctx.text_in(b, label, st.text, 0, true);
        return clicked;
    }

    void draw_toolbar_(Context& ctx, const Box& bar) {
        const Style& st = ctx.style;
        const float bw = 28.0f;
        Box b{bar.x, bar.y, bw, bar.h};
        if (nav_button_(ctx, "##back", b, 0, !back_.empty(), "Back  (Cmd [)")) go_back_();
        b.x += bw + 2;
        if (nav_button_(ctx, "##fwd", b, 1, !fwd_.empty(), "Forward  (Cmd ])")) go_forward_();
        b.x += bw + 6;
        if (nav_button_(ctx, "##up", b, 2, dir_ != dir_.root_path(), "Enclosing Folder  (Cmd Up)")) go_up_();
        b.x += bw + 8;

        // Search field (right).
        const float sw = std::min(220.0f, bar.w * 0.28f);
        const Box sb{bar.right() - sw, bar.y, sw, bar.h};
        // Path bar (middle).
        const Box pb{b.x, bar.y, sb.x - 8 - b.x, bar.h};
        if (editing_path_) {
            if (path_focus_) { ctx.begin_text_edit("##pathedit", path_buf_); path_focus_ = false; path_started_ = true; }
            std::string buf = path_buf_;
            const bool committed = ctx.input_text_box("##pathedit", pb, &buf, "Go to folder: type a path");
            const bool editing = ctx.wants_keyboard();
            if (committed) {
                path_buf_ = buf;
                go_to_typed_path_();
                editing_path_ = false;
            } else if (!editing && !path_started_) {
                editing_path_ = false;   // Escape or a click elsewhere
            }
            path_started_ = false;
        } else {
            draw_breadcrumb_(ctx, pb);
        }

        bool hov = false;
        if (ctx.invisible_button("##search", sb, &hov)) search_focus_ = true;
        else if (ctx.mouse_pressed() && !hov && search_focus_ && search_.empty()) search_focus_ = false;
        ctx.fill_rounded(sb, hov && !search_focus_ ? st.field_hover : st.field);
        if (search_focus_) ctx.outline_rounded(sb, st.accent);
        const Box ic{sb.x + 5, sb.y + (sb.h - 14) * 0.5f, 14, 14};
        ctx.icon(Icon::Search, ic, st.text_dim);
        const Box tb{ic.right() + 2, sb.y, sb.w - (ic.right() + 2 - sb.x) - 22, sb.h};
        if (search_.empty() && !search_focus_) ctx.text_in(tb, "Search", st.text_disabled, 4);
        else {
            ctx.text_in(tb, search_, st.text, 4);
            if (search_focus_ && std::fmod(ctx.time(), 1.0) < 0.6) {
                const float cx = std::min(tb.right() - 1, tb.x + 4 + ctx.text_width(search_));
                ctx.fill({cx, sb.y + 4, 1, sb.h - 8}, st.text);
            }
        }
        if (!search_.empty()) {
            const Box xb{sb.right() - 20, sb.y + (sb.h - 16) * 0.5f, 16, 16};
            bool xh = false;
            if (ctx.invisible_button("##searchclear", xb, &xh)) { search_.clear(); apply_view_(); }
            ctx.circle({xb.x + 8, xb.y + 8}, 6.5f, xh ? st.text_dim : st.text_disabled);
            ctx.line({xb.x + 5.5f, xb.y + 5.5f}, {xb.x + 10.5f, xb.y + 10.5f}, st.field, 1.4f);
            ctx.line({xb.x + 10.5f, xb.y + 5.5f}, {xb.x + 5.5f, xb.y + 10.5f}, st.field, 1.4f);
        }
    }

    void go_to_typed_path_() {
        using namespace file_dialog_detail;
        std::string s = path_buf_;
        while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
        if (s.empty()) return;
        std::filesystem::path p;
        if (s[0] == '~') p = home_dir() / s.substr(s.size() > 1 && (s[1] == '/' || s[1] == '\\') ? 2 : 1);
        else if (std::filesystem::path(s).is_absolute()) p = s;
        else p = dir_ / s;
        std::error_code ec;
        if (is_dir(p)) { navigate_(p); return; }
        if (std::filesystem::exists(p, ec) && mode_ == Mode::OpenFile) {
            navigate_(p.parent_path());
            for (size_t i = 0; i < view_.size(); ++i) if (view_[i].name == p.filename().string()) select_(static_cast<int>(i));
            if (selected_index_ >= 0 && view_[static_cast<size_t>(selected_index_)].ok) finish_(dir_ / view_[static_cast<size_t>(selected_index_)].name);
            return;
        }
        if (mode_ == Mode::SaveFile && is_dir(p.parent_path())) { navigate_(p.parent_path()); name_ = p.filename().string(); }
    }

    /** @brief The folder path as clickable segments; a click on the empty part types a path. */
    void draw_breadcrumb_(Context& ctx, const Box& pb) {
        using namespace file_dialog_detail;
        const Style& st = ctx.style;
        ctx.fill_rounded(pb, st.panel_alt);
        std::vector<std::filesystem::path> segs;
        for (std::filesystem::path p = dir_;; p = p.parent_path()) {
            segs.push_back(p);
            if (p == p.root_path() || p.parent_path() == p || p.empty()) break;
        }
        std::reverse(segs.begin(), segs.end());
        const float sep_w = 14.0f, pad = 7.0f;
        std::vector<float> w(segs.size());
        float total = 0;
        for (size_t i = 0; i < segs.size(); ++i) { w[i] = ctx.text_width(folder_label(segs[i])) + pad * 2; total += w[i] + sep_w; }
        // Drop leading segments behind an ellipsis when it doesn't fit.
        size_t first = 0;
        const float ell_w = ctx.text_width("...") + pad * 2 + sep_w;
        while (first + 1 < segs.size() && total + (first > 0 ? ell_w : 0) > pb.w - 8) { total -= w[first] + sep_w; ++first; }
        ctx.push_clip(pb);
        float x = pb.x + 4;
        if (first > 0) {
            ctx.text_in({x, pb.y, ell_w - sep_w, pb.h}, "...", st.text_dim, 0, true);
            x += ell_w - sep_w;
            chevron_(ctx, {x, pb.y, sep_w, pb.h}, 1, st.text_disabled, 3.0f);
            x += sep_w;
        }
        for (size_t i = first; i < segs.size(); ++i) {
            const Box sb{x, pb.y + 2, w[i], pb.h - 4};
            ctx.push_id(static_cast<int64_t>(i));
            bool hov = false;
            if (ctx.invisible_button("##seg", sb, &hov) && segs[i] != dir_) navigate_(segs[i]);
            ctx.pop_id();
            if (hov) ctx.fill_rounded(sb, st.row_hover);
            ctx.text_in(sb, folder_label(segs[i]), i + 1 == segs.size() ? st.text : st.text_dim, pad);
            if (hov) ctx.tooltip(segs[i].string());
            x += w[i];
            if (i + 1 < segs.size()) {
                chevron_(ctx, {x, pb.y, sep_w, pb.h}, 1, st.text_disabled, 3.0f);
                x += sep_w;
            }
        }
        ctx.pop_clip();
        // Empty space: type a path.
        if (x < pb.right() - 4) {
            const Box rest{x, pb.y, pb.right() - x, pb.h};
            bool hov = false;
            if (ctx.invisible_button("##pathrest", rest, &hov)) { editing_path_ = true; path_buf_ = dir_.string(); path_focus_ = true; }
            if (hov) ctx.tooltip("Go to Folder  (Shift Cmd G)\nClick to type a path");
        }
    }

    void draw_sidebar_(Context& ctx, const Box& side) {
        using namespace file_dialog_detail;
        const Style& st = ctx.style;
        std::vector<std::pair<std::string, std::vector<Place>>> sections;
        std::vector<std::filesystem::path> shown;
        auto add = [&](std::vector<Place>& v, std::string label, const std::filesystem::path& p, Icon ic, std::string tip = {}) {
            if (!is_dir(p)) return;
            for (const auto& s : shown) if (s == p) return;
            shown.push_back(p);
            v.push_back({std::move(label), p, ic, std::move(tip)});
        };
        std::vector<Place> fav;
        const auto home = home_dir();
        if (!launch_dir.empty()) add(fav, folder_label(launch_dir), launch_dir, Icon::Console, "The folder the app was launched from");
        for (const Place& f : favorites) add(fav, f.label.empty() ? folder_label(f.path) : f.label, f.path, f.icon, f.tip);
        add(fav, folder_label(home), home, Icon::Folder);
        add(fav, "Desktop", home / "Desktop", Icon::Monitor);
        add(fav, "Documents", home / "Documents", Icon::File);
        add(fav, "Downloads", home / "Downloads", Icon::ArrowDown);
        add(fav, "Pictures", home / "Pictures", Icon::Image);
        sections.push_back({"Favorites", std::move(fav)});
        std::vector<Place> rec;
        for (const auto& r : recent_dirs) {
            if (rec.size() >= 6) break;
            add(rec, folder_label(r), r, Icon::Folder);
        }
        if (!rec.empty()) sections.push_back({"Recent", std::move(rec)});
        std::vector<Place> loc;
        const std::filesystem::path root = dir_.root_path().empty() ? std::filesystem::path("/") : dir_.root_path();
        add(loc, folder_label(root), root, Icon::Package);
#ifdef __APPLE__
        std::error_code ec;
        for (auto it = std::filesystem::directory_iterator("/Volumes", ec); !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
            std::error_code ec2;
            if (std::filesystem::is_symlink(it->symlink_status(ec2))) continue;   // the startup disk's alias
            if (it->path().filename().string().rfind('.', 0) == 0) continue;     // .timemachine etc.
            add(loc, it->path().filename().string(), it->path(), Icon::Package);
        }
#endif
        sections.push_back({"Locations", std::move(loc)});

        const glm::vec4 bg = st.panel_alt;
        ctx.begin_region("##places", side, true, &bg);
        int n = 0;
        for (const auto& [title, places] : sections) {
            const Box hb = ctx.next_box(st.row_height - 2);
            ctx.text_in(hb, title, st.text_disabled, 2);
            for (const Place& p : places) {
                const Box rb = ctx.next_box(st.row_height + 1);
                ctx.push_id(n++);
                bool hov = false;
                if (ctx.invisible_button("##place", rb, &hov)) navigate_(p.path);
                ctx.pop_id();
                const bool cur = p.path == dir_;
                if (cur) ctx.fill_rounded(rb, st.selection_dim);
                else if (hov) ctx.fill_rounded(rb, st.row_hover);
                ctx.icon(p.icon, {rb.x + 6, rb.y + (rb.h - 14) * 0.5f, 14, 14}, cur ? st.text : folder_tint_(st));
                ctx.text_in({rb.x + 26, rb.y, rb.w - 26, rb.h}, p.label, st.text, 0);
                if (hov) ctx.tooltip(!p.tip.empty() ? p.label + "\n" + p.tip + "\n" + p.path.string() : p.path.string());
            }
            ctx.spacing(4);
        }
        ctx.end_region();
    }

    static glm::vec4 folder_tint_(const Style& st) { return glm::mix(st.accent, glm::vec4(1.0f), 0.35f); }

    void draw_list_(Context& ctx, const Box& list) {
        const Style& st = ctx.style;
        ctx.fill(list, st.field);
        const float head_h = st.row_height;
        const float row_h = st.row_height + 1;
        // Columns: Name | Date Modified | Size | Kind (Kind / Date drop out when narrow).
        const bool show_kind = list.w > 520, show_date = list.w > 360;
        const float kind_w = show_kind ? 170.0f : 0.0f, size_w = 80.0f, date_w = show_date ? 160.0f : 0.0f;
        const float sb_w = st.scrollbar;
        const float name_w = list.w - kind_w - size_w - date_w - sb_w;
        const float cx[5] = {list.x, list.x + name_w, list.x + name_w + date_w, list.x + name_w + date_w + size_w, list.x + name_w + date_w + size_w + kind_w};

        // Header.
        const Box head{list.x, list.y, list.w, head_h};
        ctx.fill(head, st.header);
        static const char* names[4] = {"Name", "Date Modified", "Size", "Kind"};
        for (int c = 0; c < 4; ++c) {
            const float w = cx[c + 1] - cx[c];
            if (w <= 0) continue;
            const Box hb{cx[c], head.y, w, head.h};
            ctx.push_id(c);
            bool hov = false;
            if (ctx.invisible_button("##col", hb, &hov)) {
                if (sort_col_ == c) sort_asc_ = !sort_asc_;
                else { sort_col_ = c; sort_asc_ = c == 0 || c == 3; }   // dates / sizes: newest / largest first
                apply_view_();
            }
            ctx.pop_id();
            if (hov) ctx.fill(hb, st.row_hover);
            ctx.text_in(hb, names[c], sort_col_ == c ? st.text : st.text_dim, 8);
            if (sort_col_ == c) chevron_(ctx, {hb.right() - 18, hb.y, 14, hb.h}, sort_asc_ ? 2 : 3, st.text_dim, 3.0f);
            if (c > 0) ctx.fill({hb.x, hb.y + 4, 1, hb.h - 8}, st.separator);
        }
        ctx.fill({list.x, head.bottom() - 1, list.w, 1}, st.border);

        // Rows (own scrolling: keyboard selection keeps the row in view).
        const Box rows{list.x, head.bottom(), list.w, list.h - head_h};
        const float content_h = static_cast<float>(view_.size()) * row_h;
        const float max_scroll = std::max(0.0f, content_h - rows.h);
        if (scroll_to_ >= 0) {
            const float top = scroll_to_ * row_h;
            if (top < scroll_) scroll_ = top;
            if (top + row_h > scroll_ + rows.h) scroll_ = top + row_h - rows.h;
            scroll_to_ = -1;
        }
        if (ctx.is_hovered(rows) && ctx.input().scroll.y != 0.0f) scroll_ -= ctx.input().scroll.y * row_h * 2.0f;
        scroll_ = std::clamp(scroll_, 0.0f, max_scroll);

        ctx.push_clip(rows);
        bool row_clicked = false;
        const int first = static_cast<int>(scroll_ / row_h);
        const int last = std::min(static_cast<int>(view_.size()), static_cast<int>((scroll_ + rows.h) / row_h) + 1);
        for (int i = first; i < last; ++i) {
            const Entry& e = view_[static_cast<size_t>(i)];
            const Box rb{rows.x, std::round(rows.y + i * row_h - scroll_), rows.w - sb_w, row_h};
            if (i % 2 == 1) ctx.fill(rb, glm::vec4(1, 1, 1, 0.025f));
            const bool renaming = !renaming_.empty() && e.name == renaming_;
            ctx.push_id(i);
            bool hov = false;
            // The row being renamed is the name field's: a row button would take its clicks.
            const bool clicked = !renaming && ctx.invisible_button("##row", rb, &hov);
            ctx.pop_id();
            const bool enabled = e.dir || e.ok;
            const bool sel = i == selected_index_;
            if (sel) ctx.fill(rb, enabled ? st.selection : st.selection_dim);
            else if (hov && enabled) ctx.fill(rb, st.row_hover);
            if (clicked) {
                row_clicked = true;
                const bool dbl = last_click_ == e.name && ctx.time() - last_click_time_ < 0.4;
                last_click_ = e.name;
                last_click_time_ = ctx.time();
                if (enabled || mode_ == Mode::SaveFile) select_(i);
                if (dbl && enabled) {
                    if (e.dir) { navigate_(dir_ / e.name); break; }
                    if (mode_ != Mode::PickFolder) { accept_(); break; }
                }
            }
            const glm::vec4 tc = enabled ? st.text : st.text_disabled;
            const glm::vec4 dc = enabled ? (sel ? st.text : st.text_dim) : st.text_disabled;
            const Icon ic = e.dir ? Icon::Folder : file_dialog_detail::is_image_ext(e.ext) ? Icon::Image : Icon::File;
            const glm::vec4 icc = !enabled ? st.text_disabled : e.dir ? (sel ? st.text : folder_tint_(st)) : (sel ? st.text : st.text_dim);
            ctx.icon(ic, {rb.x + 8, rb.y + (rb.h - 14) * 0.5f, 14, 14}, icc);
            if (renaming) draw_rename_field_(ctx, {rb.x + 24, rb.y + 1, cx[1] - rb.x - 30, rb.h - 2});
            else ctx.text_in({rb.x + 28, rb.y, cx[1] - rb.x - 34, rb.h}, e.name, tc, 0);
            if (show_date) ctx.text_in({cx[1], rb.y, date_w - 4, rb.h}, file_dialog_detail::format_time(e.mtime), dc, 8);
            {
                const std::string s = e.dir ? std::string("--") : file_dialog_detail::format_size(e.size);
                const float tw = ctx.text_width(s);
                ctx.text_in({cx[3] - tw - 12, rb.y, tw + 4, rb.h}, s, dc, 0);
            }
            if (show_kind) ctx.text_in({cx[3], rb.y, kind_w - 4, rb.h}, e.kind, dc, 8);
        }
        // A click on the empty space below the rows clears the selection. (Not a widget: one
        // declared over the rows would take their presses.)
        if (!row_clicked && ctx.mouse_released() && ctx.is_hovered(rows) &&
            ctx.mouse().y > rows.y + static_cast<float>(view_.size()) * row_h - scroll_) {
            selected_.clear();
            selected_index_ = -1;
        }
        // Scrolled out of view (or deleted behind our back): a field not declared this frame
        // has lost focus, so end the rename with what was last committed.
        if (!renaming_.empty() && !rename_drawn_) finish_rename_();
        rename_drawn_ = false;
        if (ctx.is_hovered(rows) && ctx.mouse_released(Mouse::Right)) ctx.open_popup("##listmenu");
        if (view_.empty()) {
            const char* msg = !error_.empty() ? error_.c_str() : !search_.empty() ? "No matches" : "This folder is empty";
            ctx.text_in({rows.x, rows.y + 30, rows.w, row_h}, msg, st.text_disabled, 0, true);
        }
        ctx.pop_clip();

        // Scrollbar.
        if (max_scroll > 0.0f) {
            const Box track{rows.right() - sb_w, rows.y, sb_w, rows.h};
            const float grab_h = std::max(24.0f, track.h * rows.h / content_h);
            bool hov = false, held = false;
            ctx.invisible_button("##vscroll", track, &hov, &held);
            if (held) scroll_ = std::clamp((ctx.mouse().y - track.y - grab_h * 0.5f) / (track.h - grab_h), 0.0f, 1.0f) * max_scroll;
            const Box grab{track.x + 1, track.y + (track.h - grab_h) * (scroll_ / max_scroll), track.w - 2, grab_h};
            ctx.fill_rounded(grab, held || hov ? st.text_dim : st.scroll_grab, grab.w * 0.5f);
        }
        ctx.outline(list, st.border);
        if (ctx.begin_popup("##listmenu", 180.0f)) {
            if (ctx.menu_item("New Folder", "Shift Cmd N", nullptr, error_.empty(), Icon::Folder)) new_folder_();
            ctx.end_popup();
        }
    }

    /** @brief The in-place name field of the row being renamed (a new folder). Enter, Tab or a
     *         click elsewhere renames; Escape keeps the name it had. */
    void draw_rename_field_(Context& ctx, const Box& b) {
        rename_drawn_ = true;
        if (rename_focus_) { ctx.begin_text_edit("##rename", rename_buf_); rename_focus_ = false; rename_started_ = true; }
        std::string buf = rename_buf_;
        const bool committed = ctx.input_text_box("##rename", b, &buf);
        if (committed) rename_buf_ = buf;
        if (committed || (!ctx.wants_keyboard() && !rename_started_)) finish_rename_();
        rename_started_ = false;
    }

    void draw_name_row_(Context& ctx, const Box& row) {
        const Style& st = ctx.style;
        const float lw = 70.0f;
        ctx.text_in({row.x, row.y, lw, row.h}, "Save As:", st.text_dim, 0);
        const Box fb{row.x + lw, row.y, std::min(420.0f, row.w - lw), row.h};
        if (focus_name_) { ctx.begin_text_edit("##savename", name_); focus_name_ = false; }
        std::string buf = name_;
        const bool was_editing = ctx.wants_keyboard();
        if (ctx.input_text_box("##savename", fb, &buf, "Untitled")) {
            replace_armed_.clear();   // an edited name needs its own confirmation
            name_ = buf;
        }
        // Enter in the field saves -- also when the name is unchanged, which input_text_box
        // doesn't report as a commit (a suggested name, or the second press confirming Replace).
        using coopa::input::Key;
        if (was_editing && !ctx.wants_keyboard() && (ctx.key_pressed(Key::Enter) || ctx.key_pressed(Key::KpEnter))) accept_();
        if (!replace_armed_.empty()) {
            const std::string msg = "\"" + std::filesystem::path(replace_armed_).filename().string() + "\" already exists. Save again to replace it.";
            ctx.text_in({fb.right() + 10, row.y, row.right() - fb.right() - 10, row.h}, msg, st.warning, 0);
        }
    }

    void draw_bottom_(Context& ctx, const Box& bar) {
        const Style& st = ctx.style;
        float x = bar.x;
        if (!filters_.empty()) {
            ctx.text_in({x, bar.y, 56, bar.h}, "Format:", st.text_dim, 0);
            x += 56;
            std::vector<std::string> labels;
            for (const auto& f : filters_) labels.push_back(f.label);
            const float cw = std::min(330.0f, bar.w * 0.4f);
            int f = filter_;
            if (ctx.combo_box("##format", {x, bar.y, cw, bar.h}, &f, labels) && f != filter_) {
                filter_ = f;
                if (mode_ == Mode::SaveFile && !name_.empty() && !filters_[static_cast<size_t>(f)].ext.empty()) {
                    name_ = std::filesystem::path(name_).replace_extension(filters_[static_cast<size_t>(f)].ext.front()).string();
                    replace_armed_.clear();
                }
                apply_view_();
            }
            x += cw + 12;
        }
        {
            size_t files = 0, dirs = 0;
            for (const auto& e : view_) (e.dir ? dirs : files) += 1;
            char buf[96];
            std::snprintf(buf, sizeof buf, "%zu item%s%s", view_.size(), view_.size() == 1 ? "" : "s", show_hidden_ ? "  (showing hidden)" : "");
            if (notice_.empty()) ctx.text_in({x, bar.y, 200, bar.h}, buf, st.text_disabled, 0);
            else ctx.text_in({x, bar.y, std::max(0.0f, bar.right() - 216 - x), bar.h}, notice_, st.warning, 0);
        }
        const float bw = 100.0f;
        const Box ok{bar.right() - bw, bar.y, bw, bar.h};
        const Box cancel{ok.x - bw - 8, bar.y, bw, bar.h};
        if (flat_button_(ctx, "Cancel", cancel)) cancel_();
        const char* ok_label = mode_ == Mode::SaveFile ? (replace_armed_.empty() ? "Save" : "Replace") : mode_ == Mode::PickFolder ? "Choose" : "Open";
        if (primary_button_(ctx, ok_label, ok, can_accept_())) accept_();
    }

    Mode mode_ = Mode::OpenFile;
    std::string title_;
    std::vector<std::string> ext_;   ///< lower case
    std::vector<Filter> filters_;
    int filter_ = 0;
    std::function<void(const std::filesystem::path&)> on_done_;
    std::filesystem::path dir_;
    std::vector<std::filesystem::path> back_, fwd_;
    std::string name_;                ///< Save: the file name field
    std::string selected_;
    int selected_index_ = -1;         ///< into view_
    std::vector<Entry> all_, view_;
    std::string error_;
    int sort_col_ = 0;                ///< 0 name, 1 date, 2 size, 3 kind
    bool sort_asc_ = true;
    bool show_hidden_ = false;
    float scroll_ = 0.0f;
    int scroll_to_ = -1;              ///< Row to bring into view on the next draw
    std::string search_;
    bool search_focus_ = false;
    std::string typeahead_;
    double typeahead_time_ = 0.0;
    bool editing_path_ = false, path_focus_ = false, path_started_ = false;
    std::string path_buf_;
    bool focus_name_ = false;
    std::string replace_armed_;       ///< Save: the existing file a second press replaces
    std::string renaming_;            ///< The entry being renamed in place (empty: none)
    std::string rename_buf_;
    bool rename_focus_ = false, rename_started_ = false, rename_drawn_ = false;
    std::string notice_;              ///< A failed New Folder / rename, shown in the bottom bar
    std::string last_click_;
    double last_click_time_ = 0.0;
    bool open_ = false;
    bool pending_open_ = false;       ///< open() was called; the modal opens in the next draw()
};

} // namespace coopa::ui::imm

#endif // UICOOPA_IMMEDIATE_IMM_FILE_DIALOG_H
