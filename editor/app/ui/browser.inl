// editor/app/ui/browser.inl -- included inside EditorApp's class body.
//
// The Console (Unity's log, with severity icons and filters), shown under the viewer. The
// asset browser itself is the Asset panel (ui/assets.inl).

    void draw_console_(imm::Context& ctx, const imm::Box& hb, const imm::Box& body) {
        using I = imm::Icon;
        // Header: severity filters + clear (Unity's console toolbar).
        float x = hb.right() - 30;
        const float s = hb.h - 8;
        if (ctx.icon_button("clear", I::Trash, "Clear\nEmpty the console", false, s, imm::Context::kAll, imm::Box{x, hb.y + 4, s, s})) log_.clear();
        x -= s + 10;
        const I sev_icons[3] = {I::Info, I::Warning, I::Error};
        const char* sev_tips[3] = {"Info\nShow messages", "Warnings\nShow warnings", "Errors\nShow errors"};
        for (int i = 2; i >= 0; --i) {
            ctx.push_id(i);
            if (ctx.icon_button("sev", sev_icons[i], sev_tips[i], console_show_[i], s, imm::Context::kAll, imm::Box{x, hb.y + 4, s, s})) {
                console_show_[i] = !console_show_[i];
            }
            ctx.pop_id();
            x -= s + 2;
        }
        ctx.begin_region("console", body, true);
        for (auto it = log_.rbegin(); it != log_.rend(); ++it) {
            if (!console_show_[std::clamp(it->first, 0, 2)]) continue;
            imm::Box row = ctx.next_box(ctx.style.row_height);
            ctx.icon(sev_icons[std::clamp(it->first, 0, 2)], {row.x + 2, row.y + 3, row.h - 6, row.h - 6},
                     it->first == 0 ? ctx.style.text_dim : glm::vec4(1));
            const glm::vec4 c = it->first == 2 ? ctx.style.error : it->first == 1 ? ctx.style.warning : ctx.style.text;
            ctx.text_in({row.x + row.h + 2, row.y, row.w - row.h, row.h}, it->second, c, 0.0f);
        }
        if (log_.empty()) ctx.label_dim("No messages.");
        ctx.end_region();
    }
