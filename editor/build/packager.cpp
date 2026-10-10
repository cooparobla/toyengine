#include "editor/build/packager.h"

namespace toy {
namespace editor {

const std::vector<std::string>& engine_runtime_dirs() {
    static const std::vector<std::string> dirs = {"fonts", "palettes", "ui", "sounds"};
    return dirs;
}

std::vector<PackageLayer> default_library_layers(const std::filesystem::path& project_root,
                                                        const std::filesystem::path& libs_dir) {
    std::vector<PackageLayer> layers;
    const std::vector<std::string> roots = core::RuntimeLayout::current().shader_roots(project_root);
    for (size_t i = 0; i < roots.size(); ++i) {
        layers.push_back({roots[i], "shaders", true, /*warn_shadowed=*/i + 1 == roots.size()});
    }
    layers.push_back({libs_dir / "uicoopa" / "assets" / "sounds", "sounds", false, false});
    return layers;
}

} // namespace editor
} // namespace toy

namespace toy {
namespace editor {
namespace detail {

std::string shipping_config_text_(const std::filesystem::path& src) {
    fkyaml::node doc = coopa::yaml::load_document(src);
    if (doc.is_mapping()) {
        if (!doc.contains("output") || !doc["output"].is_mapping()) doc["output"] = fkyaml::node::mapping();
        doc["output"]["save_on_exit"] = fkyaml::node(false);
    }
    return coopa::yaml::emit(doc);
}

void stage_file_(const std::filesystem::path& from, const std::filesystem::path& out,
                        bool is_root_config, const PackageOptions& opt, PackageReport& rep) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const std::string ext = from.extension().string();
    fs::create_directories(out.parent_path(), ec);
    ++rep.files;
    rep.bytes_in += fs::file_size(from, ec);
    if (is_yaml_ext_(ext)) {
        const std::string text = is_root_config && opt.shipping ? shipping_config_text_(from)
                                                                : coopa::yaml::read_text(from);
        if (opt.encode_yaml) {
            fs::path caml = out;
            caml.replace_extension(".caml");
            const std::vector<uint8_t> bytes = core::encode_caml_text(text, opt.passphrase);
            std::ofstream f(caml, std::ios::binary | std::ios::trunc);
            f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            if (!f) throw std::runtime_error("cannot write " + caml.string());
            rep.bytes_out += bytes.size();
            ++rep.encoded;
            if (opt.keep_yaml) std::ofstream(out, std::ios::binary | std::ios::trunc) << text;
        } else {
            std::ofstream f(out, std::ios::binary | std::ios::trunc);
            f << text;
            if (!f) throw std::runtime_error("cannot write " + out.string());
            rep.bytes_out += text.size();
        }
        return;
    }
    fs::copy_file(from, out, fs::copy_options::overwrite_existing, ec);
    if (ec) throw std::runtime_error("copy failed: " + ec.message());
    rep.bytes_out += fs::file_size(out, ec);
}

bool shadowed_(const std::filesystem::path& out) {
    std::error_code ec;
    if (std::filesystem::exists(out, ec)) return true;
    if (is_yaml_ext_(out.extension().string())) {
        std::filesystem::path caml = out;
        caml.replace_extension(".caml");
        return std::filesystem::exists(caml, ec);
    }
    return false;
}

void copy_layer_(const std::filesystem::path& src, const std::filesystem::path& dst,
                        const std::string& label, bool compiled_shaders, bool warn_collisions,
                        const PackageOptions& opt, PackageReport& rep) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::is_directory(src, ec)) return;
    for (auto it = fs::recursive_directory_iterator(src, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) { rep.errors.push_back(ec.message()); break; }
        if (!it->is_regular_file()) continue;
        const std::string ext = it->path().extension().string();
        const std::string fname = it->path().filename().string();
        if (fname.size() > 5 && fname.compare(fname.size() - 5, 5, ".tmp~") == 0) continue;
        if (fname == ".DS_Store") continue;
        // Shaders ship compiled; their GLSL sources and glslc depfiles stay behind.
        if (compiled_shaders && ext != ".spv") continue;
        const fs::path rel = fs::relative(it->path(), src, ec);
        const fs::path out = dst / rel;
        if (shadowed_(out)) {
            if (warn_collisions) rep.warnings.push_back(label + " " + rel.string() + " is shadowed by a higher layer");
            continue;
        }
        try {
            stage_file_(it->path(), out, false, opt, rep);
        } catch (const std::exception& e) {
            rep.errors.push_back(label + " " + rel.string() + ": " + e.what());
        }
    }
}

} // namespace detail
} // namespace editor
} // namespace toy

namespace toy {
namespace editor {

PackageReport package_project(const Project& project, const PackageOptions& opt) {
    namespace fs = std::filesystem;
    PackageReport rep;
    std::error_code ec;
    const fs::path src = project.assets();
    const fs::path dst = opt.out_dir / "assets";
    fs::create_directories(dst, ec);
    if (ec) { rep.errors.push_back("Cannot create " + dst.string() + ": " + ec.message()); return rep; }

    // 1. The project's own assets/: everything (shaders compiled only).
    for (auto it = fs::recursive_directory_iterator(src, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) { rep.errors.push_back(ec.message()); break; }
        const fs::path rel = fs::relative(it->path(), src, ec);
        const fs::path out = dst / rel;
        if (it->is_directory()) { fs::create_directories(out, ec); continue; }
        if (!it->is_regular_file()) continue;
        const std::string ext = it->path().extension().string();
        const std::string fname = it->path().filename().string();
        if (fname.size() > 5 && fname.compare(fname.size() - 5, 5, ".tmp~") == 0) continue;
        if (fname == ".DS_Store") continue;
        // shaders/: the compiled .spv come from the build tree (library layers); GLSL never ships.
        const bool in_shaders = rel.begin() != rel.end() && *rel.begin() == "shaders";
        if (in_shaders) continue;
        try {
            detail::stage_file_(it->path(), out, rel == fs::path("config.yaml"), opt, rep);
        } catch (const std::exception& e) {
            rep.errors.push_back(rel.string() + ": " + e.what());
        }
    }

    // 2. The engine checkout's runtime directories, where the project doesn't shadow them.
    std::error_code eq_ec;
    if (!opt.engine_assets.empty() && fs::is_directory(opt.engine_assets, ec) &&
        !fs::equivalent(opt.engine_assets, src, eq_ec)) {
        for (const std::string& d : engine_runtime_dirs()) {
            detail::copy_layer_(opt.engine_assets / d, dst / d, "engine", false, false, opt, rep);
        }
    }

    // 3. Library layers: compiled shaders in the runtime's order (a project shader overriding
    // the engine's is intended; see default_library_layers() for which clashes warn), uicoopa's
    // default sounds.
    for (const PackageLayer& layer : opt.library_layers) {
        detail::copy_layer_(layer.src, dst / layer.dest, layer.src.filename().string(),
                            layer.compiled_shaders, layer.warn_shadowed, opt, rep);
    }

    if (!opt.game_binary.empty()) {
        fs::copy_file(opt.game_binary, opt.out_dir / opt.game_binary.filename(), fs::copy_options::overwrite_existing, ec);
        if (ec) rep.errors.push_back("Binary copy failed: " + ec.message());
    }
    return rep;
}

} // namespace editor
} // namespace toy
