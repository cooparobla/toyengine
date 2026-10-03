/**
 * @file file_dialog.h
 * @brief An in-editor file/folder picker drawn with the immediate-mode layer (a modal).
 */

#ifndef TOYEDITOR_APP_FILE_DIALOG_H
#define TOYEDITOR_APP_FILE_DIALOG_H

#include <uicoopa/immediate/imm.h>

#include <algorithm>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

namespace toy::editor {

namespace imm = coopa::ui::imm;

class FileDialog {
public:
    enum class Mode { OpenFile, SaveFile, PickFolder };

    /**
     * @param ext Accepted extensions (".yaml"); empty accepts anything. Ignored for folders.
     * @param on_done Called with the chosen path.
     */
    void open(imm::Context& ctx, Mode mode, std::string title, const std::filesystem::path& start,
              std::vector<std::string> ext, std::function<void(const std::filesystem::path&)> on_done,
              std::string default_name = {}) {
        mode_ = mode;
        title_ = std::move(title);
        ext_ = std::move(ext);
        on_done_ = std::move(on_done);
        std::error_code ec;
        dir_ = std::filesystem::is_directory(start, ec) ? start : start.parent_path();
        if (dir_.empty() || !std::filesystem::is_directory(dir_, ec)) dir_ = std::filesystem::current_path(ec);
        name_ = default_name;
        selected_.clear();
        refresh_();
        ctx.open_modal(title_ + "##filedialog");
        open_ = true;
    }

    bool is_open() const { return open_; }

    /** @brief Draws the dialog if open. Call every frame. */
    void draw(imm::Context& ctx) {
        if (!open_) return;
        const glm::vec2 cs = ctx.canvas_size();
        const glm::vec2 size(std::min(720.0f, cs.x - 40), std::min(520.0f, cs.y - 40));
        if (!ctx.begin_modal(title_ + "##filedialog", size)) { open_ = false; return; }
        // Path bar.
        std::string path_text = dir_.string();
        imm::Box bar = ctx.next_box(ctx.style.row_height);
        imm::Box up_btn{bar.x, bar.y, 32, bar.h};
        if (ctx.invisible_button("up", up_btn)) navigate_(dir_.parent_path());
        ctx.fill(up_btn, ctx.last_hovered() ? ctx.style.button_hover : ctx.style.button);
        ctx.text_in(up_btn, "..", ctx.style.text, 0, true);
        if (ctx.input_text_box("path", {bar.x + 36, bar.y, bar.w - 36, bar.h}, &path_text)) navigate_(path_text);

        // Listing.
        imm::Box list = ctx.next_box(size.y - ctx.style.row_height * 5.5f);
        static const glm::vec4 list_bg{0.09f, 0.10f, 0.12f, 1.0f};
        ctx.begin_region("listing", list, true, &list_bg);
        for (const auto& e : entries_) {
            const bool is_dir = e.dir;
            const std::string label = (is_dir ? "[ ] " : "    ") + e.name;
            ctx.push_id(e.name);
            const bool sel = selected_ == e.name;
            if (ctx.selectable(label, sel)) {
                if (is_dir) {
                    if (sel || mode_ == Mode::PickFolder) { navigate_(dir_ / e.name); ctx.pop_id(); break; }
                    selected_ = e.name;
                } else {
                    selected_ = e.name;
                    name_ = e.name;
                    if (last_click_ == e.name && ctx.time() - last_click_time_ < 0.4) { finish_(dir_ / e.name); ctx.pop_id(); break; }
                    last_click_ = e.name;
                    last_click_time_ = ctx.time();
                }
            }
            ctx.pop_id();
        }
        ctx.end_region();

        if (mode_ != Mode::PickFolder) ctx.input_text("File name", &name_);
        const char* ok_label = mode_ == Mode::SaveFile ? "Save" : mode_ == Mode::PickFolder ? "Choose This Folder" : "Open";
        if (ctx.button(ok_label, 150)) {
            if (mode_ == Mode::PickFolder) finish_(selected_.empty() ? dir_ : dir_ / selected_);
            else if (!name_.empty()) {
                std::filesystem::path p = dir_ / name_;
                if (mode_ == Mode::SaveFile && !ext_.empty() && p.extension().empty()) p += ext_.front();
                finish_(p);
            }
        }
        ctx.same_line();
        if (ctx.button("Cancel", 100)) { open_ = false; ctx.close_modal(); }
        if (ctx.shortcut(coopa::input::Key::Escape)) { open_ = false; ctx.close_modal(); }
        if (!open_) ctx.close_modal();
        ctx.end_modal();
    }

private:
    struct Entry { std::string name; bool dir; };

    void navigate_(const std::filesystem::path& p) {
        std::error_code ec;
        if (std::filesystem::is_directory(p, ec)) { dir_ = std::filesystem::absolute(p, ec).lexically_normal(); selected_.clear(); refresh_(); }
    }

    void refresh_() {
        entries_.clear();
        std::error_code ec;
        for (auto it = std::filesystem::directory_iterator(dir_, ec); !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
            const std::string name = it->path().filename().string();
            if (name.empty() || name[0] == '.') continue;
            const bool dir = it->is_directory(ec);
            if (!dir) {
                if (mode_ == Mode::PickFolder) continue;
                if (!ext_.empty() && std::find(ext_.begin(), ext_.end(), it->path().extension().string()) == ext_.end()) continue;
            }
            entries_.push_back({name, dir});
        }
        std::sort(entries_.begin(), entries_.end(), [](const Entry& a, const Entry& b) {
            if (a.dir != b.dir) return a.dir;
            return a.name < b.name;
        });
    }

    void finish_(const std::filesystem::path& p) {
        open_ = false;
        if (on_done_) on_done_(p);
    }

    Mode mode_ = Mode::OpenFile;
    std::string title_;
    std::vector<std::string> ext_;
    std::function<void(const std::filesystem::path&)> on_done_;
    std::filesystem::path dir_;
    std::string name_;
    std::string selected_;
    std::vector<Entry> entries_;
    std::string last_click_;
    double last_click_time_ = 0.0;
    bool open_ = false;
};

} // namespace toy::editor

#endif // TOYEDITOR_APP_FILE_DIALOG_H
