#include <toyengine/core/runtime_paths.h>

#include <coopa/yaml/document.h>
#include <coopa/yaml/writer.h>

#include <cstdlib>
#include <system_error>

#include <unistd.h>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

namespace toy::core {

namespace {
// Constant-initialized, so it holds ROOT_DIR before any dynamic initializer (including the
// generated set_build_project_root() call) can run.
const char* s_build_project_root = ROOT_DIR;
}

void set_build_project_root(const char* path) { s_build_project_root = path; }

const char* build_project_root() { return s_build_project_root; }

} // namespace toy::core

namespace toy {
namespace core {

const char* debug_env(const char* name) {
    if constexpr (k_shipping) { (void)name; return nullptr; }
    return std::getenv(name);
}

std::filesystem::path executable_path() {
    std::error_code ec;
#if defined(__APPLE__)
    char buf[4096];
    uint32_t size = sizeof(buf);
    if (_NSGetExecutablePath(buf, &size) == 0) return std::filesystem::weakly_canonical(buf, ec);
    return {};
#else
    return std::filesystem::read_symlink("/proc/self/exe", ec);
#endif
}

std::optional<PackageManifest> PackageManifest::load(const std::filesystem::path& path) {
    std::optional<fkyaml::node> doc;
    try { doc = coopa::yaml::try_load_document(path); } catch (...) { return std::nullopt; }
    if (!doc || !doc->is_mapping()) return std::nullopt;
    PackageManifest m;
    auto str = [&](const char* key, std::string& out) {
        if (doc->contains(key) && (*doc)[key].is_string()) out = (*doc)[key].get_value<std::string>();
    };
    str("name", m.name);
    str("version", m.version);
    str("build", m.build);
    str("profile", m.profile);
    str("bundle_id", m.bundle_id);
    str("engine_commit", m.engine_commit);
    return m;
}

void PackageManifest::save(const std::filesystem::path& path) const {
    fkyaml::node n = fkyaml::node::mapping();
    n["name"] = fkyaml::node(name);
    n["version"] = fkyaml::node(version);
    n["build"] = fkyaml::node(build);
    n["profile"] = fkyaml::node(profile);
    n["bundle_id"] = fkyaml::node(bundle_id);
    n["engine_commit"] = fkyaml::node(engine_commit);
    coopa::yaml::save_document(path, n);
}

RuntimeLayout RuntimeLayout::detect(const std::filesystem::path& exe) {
    RuntimeLayout l;
    l.exe = exe;
    std::error_code ec;
    if (!exe.empty()) {
        const std::filesystem::path dir = exe.parent_path();
        const std::filesystem::path candidates[] = {dir, dir.parent_path() / "Resources"};
        for (const auto& c : candidates) {
            if (std::filesystem::is_regular_file(c / PackageManifest::k_file_name, ec)) {
                l.mode = Mode::Packaged;
                l.resources_root = c;
                l.manifest = PackageManifest::load(c / PackageManifest::k_file_name);
                return l;
            }
        }
    }
    l.engine_assets = std::filesystem::path(ROOT_DIR) / "assets";
    return l;
}

const RuntimeLayout& RuntimeLayout::current() {
    static const RuntimeLayout layout = detect(executable_path());
    return layout;
}

std::filesystem::path RuntimeLayout::project_root() const {
    if (packaged()) return resources_root;
    if (const char* p = debug_env("TOY_PROJECT_DIR"); p && *p) return std::filesystem::path(p);
    return std::filesystem::path(build_project_root());
}

std::vector<std::string> RuntimeLayout::shader_roots(const std::filesystem::path& project_root) const {
    const std::filesystem::path project_shaders = project_root / "assets" / "shaders";
    if (packaged()) return {project_shaders.string()};
    const std::filesystem::path engine_shaders = engine_assets / "shaders";
    std::vector<std::string> roots;
    std::error_code ec;
    const bool own_shaders = std::filesystem::is_directory(project_shaders, ec) &&
                             !std::filesystem::equivalent(project_shaders, engine_shaders, ec);
    const std::filesystem::path built = compiled_shader_dir();
    if (built.empty()) {   // a build without the define: shaders compiled next to their sources
        if (own_shaders) roots.push_back(project_shaders.string());
        roots.push_back(engine_shaders.string());
        roots.push_back(std::string(PROJ_DIR) + "/gfxcoopa/assets/shaders");
        roots.push_back(std::string(PROJ_DIR) + "/uicoopa/assets/shaders");
        return roots;
    }
    if (own_shaders) {
        roots.push_back((built / "project_shaders").string());
        const std::filesystem::path theirs = project_root / "build" / "shaders" / "project_shaders";
        if (!std::filesystem::equivalent(theirs, built / "project_shaders", ec)) roots.push_back(theirs.string());
    }
    roots.push_back((built / "toyengine_shaders").string());
    roots.push_back((built / "shaders").string());           // gfxcoopa's target is plain "shaders"
    roots.push_back((built / "uicoopa_shaders").string());
    return roots;
}

std::filesystem::path RuntimeLayout::compiled_shader_dir() {
#ifdef TOY_SHADER_BUILD_DIR
    return std::filesystem::path(TOY_SHADER_BUILD_DIR);
#else
    return {};
#endif
}

std::string RuntimeLayout::app_id() const {
    if (manifest) {
#if defined(__APPLE__)
        if (!manifest->bundle_id.empty()) return manifest->bundle_id;
#endif
        if (!manifest->name.empty()) return manifest->name;
    }
    std::string project = project_root().filename().string();
    if (project.empty()) project = "game";
    return "toyengine-dev/" + project;
}

} // namespace core
} // namespace toy

namespace toy {
namespace core {
namespace detail {

std::filesystem::path home_dir() {
    const char* home = std::getenv("HOME");
    return std::filesystem::path(home && *home ? home : "/tmp");
}

std::filesystem::path xdg_dir(const char* var, const char* fallback_rel) {
    if (const char* v = std::getenv(var); v && *v) return v;
    return home_dir() / fallback_rel;
}

} // namespace detail
} // namespace core
} // namespace toy

namespace toy {
namespace core {

std::filesystem::path user_data_dir(const std::string& app) {
#if defined(__APPLE__)
    return detail::home_dir() / "Library" / "Application Support" / app;
#else
    return detail::xdg_dir("XDG_DATA_HOME", ".local/share") / app;
#endif
}

std::filesystem::path user_log_dir(const std::string& app) {
#if defined(__APPLE__)
    return detail::home_dir() / "Library" / "Logs" / app;
#else
    return detail::xdg_dir("XDG_STATE_HOME", ".local/state") / app / "logs";
#endif
}

void prepare_runtime_environment() {
    static bool done = false;
    if (done) return;
    done = true;
    const RuntimeLayout& layout = RuntimeLayout::current();
    if (!layout.packaged()) return;
#if defined(__APPLE__)
    const std::filesystem::path icd = layout.resources_root / "vulkan" / "icd.d" / "MoltenVK_icd.json";
    std::error_code ec;
    if (std::filesystem::is_regular_file(icd, ec)) {
        ::setenv("VK_DRIVER_FILES", icd.c_str(), 0);
        ::setenv("VK_ICD_FILENAMES", icd.c_str(), 0);
    }
#endif
}

} // namespace core
} // namespace toy
