// editor/app/ui/build.inl -- included inside EditorApp's class body.
//
// Build > Refresh: rebuilds the project's game + editor -- the build this editor is running from
// -- so src/ edits made outside the editor (VS Code, ...) take effect. A blocking modal streams
// the build log; compiler errors are listed (click one to open it in VS Code / the default app)
// and also go to the Console. A build that changed the editor binary offers to relaunch it,
// since new C++ only loads in a new process.
//
// Build > Build (Development / Shipping): compiles the game, stages its assets and bundles a
// relocatable, signed package for this platform (editor/build/build_pipeline.h), in the same
// modal. Build Settings edits <project>/build_settings.yaml (name, version, bundle id, signing).

public:
    /**
     * @brief The build command: `cmake --build` on the directory this editor binary was built in,
     *        for its own target and its game's (a game project: build/<target>_editor + <target>;
     *        toyengine itself: toyengine_editor + toyengine, not the tests).
     */
    static std::string default_build_command();

    /** @brief Starts Build > Refresh (`command` overrides the build command: tests). */
    void build_refresh(const std::string& command = {});

    /**
     * @brief Starts Build > Build for `profile` (in the background, the Build modal streaming
     *        its log). `run_after`: launch the built game when it succeeds. `req` overrides the
     *        output / compile step (tests).
     */
    void build_game(BuildProfile profile, bool run_after = false, BuildRequest req = {});

    /** @brief The last Build > Build's outcome (empty until one finishes). */
    std::optional<BuildResult> last_build_result() const;

    const Task& build_task() const { return build_task_; }
    const std::vector<Diagnostic>& build_diagnostics() const { return build_diags_; }
    /** @brief True once the user chose to relaunch after a build (main() restarts the process). */
    bool relaunch_requested() const { return relaunch_; }
    /** @brief The relaunch prompt after a build that changed this editor's binary. */
    bool build_relaunch_offered() const { return build_relaunch_offered_; }
    /** @brief Relaunch now (asks about unsaved changes first, like Quit). */
    void relaunch_after_build();

private:
    void draw_build_menu_(imm::Context& ctx);

    /** @brief Once the build finishes: diagnostics to the Console, relaunch offer if needed. */
    void poll_build_();

    void draw_build_modal_(imm::Context& ctx);

    /** @brief A button at a fixed box (primary: accent-filled, the default action). */
    static bool build_button_(imm::Context& ctx, const char* label, const imm::Box& b, bool primary = false);

    /** @brief Launches a built game detached (Build and Run / Run). */
    void run_built_game_(const BuildResult& r);

    static void reveal_in_file_browser_(const fs::path& p);

    void open_build_settings_();

    void draw_build_settings_modal_(imm::Context& ctx);

    enum class BuildKind { Refresh, Game };
    BuildKind build_kind_ = BuildKind::Refresh;
    bool build_run_after_ = false;
    std::shared_ptr<BuildResult> build_game_result_;
    std::optional<BuildSettings> build_settings_snapshot_;
    BuildSettings build_settings_edit_;
    std::vector<std::string> sign_identities_;
    Task build_task_;
    int build_gen_ = 0;
    std::vector<Diagnostic> build_diags_;
    fs::file_time_type build_binary_stamp_{};
    bool build_relaunch_offered_ = false;
    bool relaunch_ = false;
    LogView build_log_;
