#include "editor/build/build_pipeline.h"

namespace toy {
namespace editor {

BuildEnvironment BuildEnvironment::current() {
    if (compiled()) return *compiled_slot_();
    BuildEnvironment e;
    e.build_dir = std::filesystem::path(ROOT_DIR) / "build";
    e.game_target = "toyengine";
    e.source_dir = ROOT_DIR;
    e.game_binary = e.build_dir / e.game_target;
    return e;
}

bool BuildEnvironment::builds(const Project& project) const {
    std::error_code ec;
    const bool generic = std::string(toy::core::build_project_root()) == std::string(ROOT_DIR);
    return generic || std::filesystem::equivalent(project.root(), std::filesystem::path(toy::core::build_project_root()), ec);
}

std::optional<BuildEnvironment>& BuildEnvironment::compiled_slot_() {
    static std::optional<BuildEnvironment> env;
    return env;
}

std::string artifact_launch_command(const std::filesystem::path& artifact, const std::string& exe_name) {
#if defined(__APPLE__)
    (void)exe_name;
    return "open " + shell_quote(artifact.string());
#else
    return shell_quote((artifact / (exe_name + ".sh")).string());
#endif
}

BuildResult run_build(const Project& project, const BuildSettings& settings, const BuildRequest& req,
                             const BuildEnvironment& env, CommandRunner& run) {
    namespace fs = std::filesystem;
    BuildResult res;
    std::error_code ec;
    const bool shipping = req.profile == BuildProfile::Shipping;
    auto fail = [&](const std::string& msg) { res.ok = false; res.error = msg; run.log("error: " + msg); return res; };
    run.log(std::string("Building ") + settings.product_name + " " + settings.version + " (" + profile_name(req.profile) +
            ", " + host_platform_name() + " " + host_arch_name() + ")");

    // --- 1. Compile ---
    fs::path binary = req.binary_override.empty() ? env.game_binary : req.binary_override;
    if (!req.skip_compile && req.binary_override.empty()) {
        if (!env.builds(project)) return fail("this editor's game binary is another project's; open the project's own editor to build it");
        if (shipping) {
            const fs::path ship_dir = project.root() / "build-ship";
            std::string configure = "cmake -S " + shell_quote(env.source_dir.string()) + " -B " + shell_quote(ship_dir.string()) +
                                    " -DCMAKE_BUILD_TYPE=Release -DTOY_SHIPPING=ON";
            configure += " -DTOY_CAML_KEY_BAKED=" + shell_quote(settings.caml_key);
            if (!run.run(configure).ok()) return fail("cmake configure failed");
            const CommandResult b = run.run("cmake --build " + shell_quote(ship_dir.string()) + " --target " +
                                            shell_quote(env.game_target) + " -j 8");
            if (!b.ok()) return fail(run.cancelled() ? "cancelled" : "compile failed");
            binary = ship_dir / env.game_target;
            if (!fs::exists(binary, ec)) {
                // A game project builds the engine as a subdirectory; its game lands at the top.
                return fail("built binary not found: " + binary.string());
            }
        } else {
            const CommandResult b = run.run("cmake --build " + shell_quote(env.build_dir.string()) + " --target " +
                                            shell_quote(env.game_target) + " -j 8");
            if (!b.ok()) return fail(run.cancelled() ? "cancelled" : "compile failed");
        }
    }
    if (!fs::exists(binary, ec)) return fail("game binary not found: " + binary.string() + " (build it first)");
    if (run.cancelled()) return fail("cancelled");

    // --- 2. Layout ---
    const fs::path out = req.out_dir.empty() ? settings.output_path(project) / profile_name(req.profile) : req.out_dir;
    fs::create_directories(out, ec);
    const std::string name = settings.file_name();
#if defined(__APPLE__)
    const MacAppLayout layout = MacAppLayout::for_(out, name);
    const fs::path artifact = layout.app;
    const fs::path resources = layout.resources;
#else
    const fs::path artifact = out / name;
    const fs::path resources = artifact;
#endif
    fs::remove_all(artifact, ec);
    fs::create_directories(resources, ec);
    if (ec) return fail("cannot create " + resources.string() + ": " + ec.message());

    // --- 3. Stage assets ---
    run.log("Staging assets...");
    PackageOptions opt;
    opt.out_dir = resources;
    opt.encode_yaml = shipping;
    opt.shipping = shipping;
    opt.passphrase = shipping ? settings.caml_key : std::string();
    opt.engine_assets = fs::path(ROOT_DIR) / "assets";
    opt.library_layers = default_library_layers(project.root());
    const PackageReport rep = package_project(project, opt);
    for (const std::string& w : rep.warnings) res.warnings.push_back(w);
    if (!rep.ok()) {
        for (const std::string& e : rep.errors) run.log("error: " + e);
        return fail("staging assets failed");
    }
    run.log("  " + std::to_string(rep.files) + " files" + (shipping ? " (" + std::to_string(rep.encoded) + " encoded)" : std::string()));

    // --- 4. Manifest ---
    core::PackageManifest manifest;
    manifest.name = name;
    manifest.version = settings.version;
    manifest.build = settings.build_number;
    manifest.profile = profile_name(req.profile);
    manifest.bundle_id = settings.bundle_id;
    manifest.engine_commit = command_output("git -C " + shell_quote(ROOT_DIR) + " rev-parse --short HEAD");
    manifest.save(resources / core::PackageManifest::k_file_name);

    // --- 5. Bundle ---
    BundleJob job;
    job.settings = settings;
    job.profile = req.profile;
    job.project_root = project.root();
    job.exe = binary;
#if defined(__APPLE__)
    const BundleResult b = finalize_macos_app(job, layout, run);
    res.executable = layout.macos / name;
#else
    const BundleResult b = finalize_linux_folder(job, artifact, run);
    res.executable = artifact / name;
#endif
    for (const std::string& w : b.warnings) res.warnings.push_back(w);
    for (const std::string& w : res.warnings) run.log("warning: " + w);
    if (!b.ok) return fail(b.error.empty() ? std::string("bundling failed") : b.error);
    res.ok = true;
    res.artifact = b.artifact;
    res.archive = b.archive;
    res.signed_with = b.signed_with;
    res.notarized = b.notarized;
    run.log("Built " + res.artifact.string());
    if (!res.archive.empty()) run.log("Archive " + res.archive.string());
    if (!res.signed_with.empty() && res.signed_with != "n/a") {
        run.log("Signed: " + res.signed_with + (res.notarized ? " (notarized, stapled)" : ""));
    }
    return res;
}

} // namespace editor
} // namespace toy
