// editor/app/ui/sculpt.inl -- included inside EditorApp's class body.
//
// Blender's Sculpt Mode on the active mesh object: brushes (Draw, Smooth, Inflate, Grab,
// Flatten) applied along the stroke, F / Shift+F to resize the brush radius / strength,
// X / Y / Z symmetry (local or global axes), and a surface-hugging brush circle. Strokes reshape the
// mesh document directly (one undo step each); while sculpting, the GPU mesh is the
// SculptPreview's dynamic buffer instead of a re-export per frame.

    // =================================================================================
    // Lifecycle
    // =================================================================================

    void sculpt_begin_();

    void sculpt_end_();

    /** @brief Shows the preview mesh on every live object using the edited mesh file. */
    void assign_sculpt_preview_();

    /** @brief Per frame (pre-render): follow undo / subdivide, upload moved positions. */
    void sculpt_frame_();

    // =================================================================================
    // Strokes
    // =================================================================================

    struct Stroke {
        bool active = false;
        SculptBrush brush = SculptBrush::Draw;
        bool invert = false;
        float radius = 0.1f;          ///< mesh-local
        glm::vec2 last_px{0.0f};
        // Grab
        std::vector<SculptGrab> grabs;
        glm::vec3 plane_point{0.0f}, plane_normal{0, 0, 1};   ///< world
        glm::vec3 start_world{0.0f};
    };

    /** @brief Mesh-local surface hit under `px` (normal facing the camera). */
    bool sculpt_hit_(const ViewProj& vp, glm::vec2 px, glm::vec3& hit, glm::vec3& normal);

    /** @brief The brush's screen radius as a mesh-local distance at `local_hit`. */
    float sculpt_local_radius_(const ViewProj& vp, const glm::vec3& local_hit) {
        return brush_local_radius_(vp, local_hit, sculpt_.radius_px);
    }

    void sculpt_dab_at_(const ViewProj& vp, glm::vec2 px, std::vector<uint32_t>& moved);

    void begin_stroke_(const ViewProj& vp, glm::vec2 px, bool shift, bool ctrl);

    void continue_stroke_(const ViewProj& vp, glm::vec2 px);

    void finish_dabs_(const std::vector<uint32_t>& moved);

    void end_stroke_();

    // =================================================================================
    // Input
    // =================================================================================

    struct SculptResize { bool active = false; bool strength = false; glm::vec2 start{0.0f}; float start_value = 0.0f; };

    /** @brief Sculpt Mode's mouse and keys (navigation is handled before this). */
    void handle_sculpt_input_(imm::Context& ctx, const ViewProj& vp, bool hovered);

    /** @brief The brush circle (on the surface when over the mesh) and the F / Shift+F gauge. */
    void draw_sculpt_overlay_(imm::Context& ctx, const ViewProj& vp);

    /**
     * @brief The brush circle every brush mode draws (Sculpt, Vertex / Weight Paint): on the
     *        surface when over the mesh with an inner strength ring, a screen circle otherwise,
     *        and the F / Shift+F gauge while `resize` is active.
     */
    void draw_brush_overlay_(imm::Context& ctx, const ViewProj& vp, float radius_px, float strength, const SculptResize& resize,
                             bool stroking, bool inverted);

    // =================================================================================
    // UI
    // =================================================================================

    /** @brief Header widgets in Sculpt Mode: radius, strength. Returns the width used. */
    float draw_sculpt_header_(imm::Context& ctx, float x, const imm::Box& hb);

    /** @brief "Mirror", or "Mirror XZ" / "Mirror X (Global)" when axes are on. */
    static std::string symmetry_label_(const MirrorSettings& sym);

    /** @brief The width draw_symmetry_buttons_() takes. */
    float symmetry_buttons_width_(imm::Context& ctx, const imm::Box& hb, const MirrorSettings& sym) const {
        return ctx.text_width(symmetry_label_(sym)) + (hb.h - 6) + 6;
    }

    /**
     * @brief The symmetry control shared by the Edit and Sculpt headers: one button naming the
     *        active axes (highlighted when any are on) that opens a popover with the X / Y / Z
     *        toggles and the Local / Global choice. Returns the new x.
     */
    float draw_symmetry_buttons_(imm::Context& ctx, float x, const imm::Box& hb, MirrorSettings& sym, const char* id,
                                 const char* what);

    /** @brief The same toggles as panel rows (Properties > Tool). */
    void draw_symmetry_panel_(imm::Context& ctx, MirrorSettings& sym);

    /** @brief The brush column of the toolbar (Sculpt Mode). */
    void draw_sculpt_toolbar_(imm::Context& ctx, const imm::Box& r);

    /** @brief Properties > Tool in Sculpt Mode: brush settings and density. */
    void draw_sculpt_tool_panel_(imm::Context& ctx);

public:
    SculptSettings& sculpt_settings() { return sculpt_; }
    MirrorSettings& edit_symmetry() { return edit_symmetry_; }
    bool sculpt_stroking() const { return stroke_.active; }

private:
