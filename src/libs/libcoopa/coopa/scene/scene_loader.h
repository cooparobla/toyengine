/**
 * @file scene_loader.h
 * @brief Loads a Scene from a YAML document using fkYAML.
 *
 * SceneLoader itself understands only hierarchy and Transform — the one
 * component type libcoopa defines. Every other component (mesh renderers,
 * cameras, lights, UI widgets, an Animator, ...) is parsed by callbacks
 * registered from outside libcoopa via register_component_parser(), so the
 * scene system never depends on gfxcoopa, uicoopa, the engine, or any other
 * consumer. See toyengine/anim/animation_yaml.h's
 * register_animation_components() for the Animator/AnimationClip registrant.
 *
 * Scene format:
 *   format: <free-form string, informational only>
 *   scene:
 *     inherit_from: <path.yaml>              # optional; whole-file inheritance, see below
 *     scene_name: <name>
 *     auto_transform: <bool, default true>   # add a TransformComponent to every object
 *     root_objects:
 *       - name: <str>
 *         active: <bool>
 *         inherit_from: <path.yaml[#Object]> # optional; this object's base, merged in first
 *         components:
 *           - !Transform  { position, rotation, scale }   # or "type: Transform"
 *           - !SomeTag    { ... }                          # dispatched via the registry
 *         children: [...]
 *
 * Both a leading-bang YAML tag ("!Foo") and a "type: Foo" key are accepted
 * and normalized to the same bare name before dispatch, so a single parser
 * registration matches either spelling.
 *
 * Known fkYAML limitation (vendored version): a block-style "!Tag" mapping
 * (tag on its own line, keys indented below it) parses fine as the FIRST item
 * of a sequence, but fails with "Detected invalid indentation" as the second
 * or later item — e.g. `- !RectTransform\n  x: 1\n- !Image\n  y: 2` throws on
 * the second entry. Flow-style ("!Tag { x: 1 }") and the "type: Foo" key form
 * are both unaffected. Prefer "type:" for any components[] list with more than
 * one entry until this is fixed upstream; every scene file in this codebase
 * uses that form for exactly this reason.
 *
 * By default, documents are read through coopa::yaml::load_document(), which
 * accepts plain YAML and any encoded container whose decoder the application
 * registered (toyengine registers caml's .caml format), and finds "x.caml"
 * when a reference names "x.yaml". Set a document loader
 * (set_document_loader()) to route through something else entirely.
 *
 * Before any of the above is parsed, every `inherit_from` reachable from the
 * document is expanded by SceneInheritance (see scene_inherit.h) into one
 * fully-merged node — this happens unconditionally, even for documents routed
 * through a custom document loader, so `inherit_from` works the same way
 * regardless of where the raw YAML came from. See scene_inherit.h for the
 * object-level/scene-level inheritance and merge rules; ParseContext::resolve()
 * below is the seam that keeps a merged-in file's relative asset paths
 * resolving against the directory that actually declared them.
 */

#ifndef COOPA_SCENE_SCENE_LOADER_H
#define COOPA_SCENE_SCENE_LOADER_H

#include <coopa/scene/scene.h>
#include <coopa/scene/scene_object.h>
#include <coopa/scene/scene_inherit.h>
#include <coopa/scene/components/transform_component.h>

#include <fkYAML/node.hpp>
#include <coopa/job/engine.h>
#include <coopa/yaml/document.h>

#include <atomic>
#include <chrono>
#include <limits>
#include <string>
#include <unordered_map>
#include <memory>
#include <filesystem>
#include <fstream>
#include <shared_mutex>
#include <stdexcept>
#include <functional>
#include <vector>

namespace coopa {
namespace scene {

/**
 * @brief How SceneLoader builds a scene from its document (SceneLoader::LoadOptions).
 */
struct SceneLoaderOptions {
    /// Call Scene::start() once every object is built (the default, and what load() has always
    /// done). Off builds the scene unstarted: the caller installs its systems and starts it
    /// later -- an async load activates the scene frames after building it.
    bool start = true;
};

/**
 * @class SceneLoader
 * @brief Parses a YAML scene file into a Scene, dispatching component parsing
 *        to registered callbacks.
 *
 * Usage:
 * @code
 * // Once at startup, from whichever module defines each component:
 * SceneLoader::register_component_parser("MeshRenderer", [&](const auto& node, auto& obj, const auto& ctx) {
 *     auto* mr = obj.template add_component<MeshRenderer>();
 *     // ... populate mr from node, resolving relative asset paths via ctx.resolve(path) ...
 * });
 *
 * Scene scene = SceneLoader::load("assets/scenes/cube/scene.yaml");
 * @endcode
 */
class SceneLoader {
public:
    /**
     * @brief Context passed to every component parser alongside its YAML node.
     */
    struct ParseContext {
        std::string scene_path; /**< Full path of the root scene file being loaded. */
        std::string scene_dir;  /**< Its directory; resolve relative asset paths against this. */

        /**
         * @brief Directories to try first when resolving a relative asset
         *        path, nearest declaring file first.
         *
         * Populated per-component from that component's SceneInheritance
         * `__source_dirs` provenance, so a component merged in from a prefab
         * in a different directory (e.g. assets/prefabs/) still resolves its
         * own relative paths (e.g. `font: ../fonts/x.ttf`) against the file
         * that actually wrote them, not against the including scene's
         * directory. Empty when a node carries no provenance (e.g. an
         * object built without ever going through SceneLoader::load()).
         */
        std::vector<std::string> search_dirs;

        /**
         * @brief Resolves a relative asset path against this context's provenance.
         *
         * Tries search_dirs in order, then scene_dir, returning the first
         * candidate that exists on disk. Falls through to the path
         * unchanged if none exist, so callers with their own asset search
         * roots (e.g. coopa::asset::AssetSource) can still make sense of it.
         *
         * @param relative Path as written in the scene file.
         * @return The resolved path, or `relative` unchanged if no candidate exists.
         */
        /**
         * @brief The directory a component's relative asset paths start from: the file that
         *        actually wrote it (a prefab in objects/, say) when provenance is known, else
         *        the scene's. Hand it to AssetSource / AssetManager lookups, which then fall
         *        back to the project's asset roots.
         */
        const std::string& base_dir() const { return search_dirs.empty() ? scene_dir : search_dirs.front(); }

        std::string resolve(const std::string& relative) const {
            if (std::filesystem::path(relative).is_absolute()) return relative;
            for (const auto& dir : search_dirs) {
                std::filesystem::path candidate = std::filesystem::path(dir) / relative;
                if (std::filesystem::exists(candidate)) return candidate.string();
            }
            std::filesystem::path candidate = std::filesystem::path(scene_dir) / relative;
            if (std::filesystem::exists(candidate)) return candidate.string();
            // Last, the project's asset roots (set_search_roots()) -- the same fallback prefab
            // references use, so `fonts/x.ttf` or `ui/themes/dark.yaml` written in a prefab
            // that lives in another folder still finds the shared file.
            for (const auto& root : SceneInheritance::search_roots()) {
                std::filesystem::path in_root = std::filesystem::path(root) / relative;
                if (std::filesystem::exists(in_root)) return in_root.string();
            }
            // Then by name: the asset may sit in tag folders under its type folder (AssetIndex).
            if (auto found = coopa::asset::AssetIndex::find_in(SceneInheritance::search_roots(), relative)) return found->string();
            return relative;
        }
    };

    /**
     * @brief Signature for a registered component parser.
     *
     * Receives the raw component YAML node, the SceneObject it should attach
     * to (already carrying its TransformComponent if auto_transform is on),
     * and the ParseContext for resolving relative asset paths. Implementations
     * should call `obj.add_component<T>()` and populate it from `node`.
     */
    using ComponentParser = std::function<void(const fkyaml::node& node, SceneObject& obj, const ParseContext& ctx)>;

    /**
     * @brief Signature for a pluggable raw-document loader.
     *
     * Receives a scene file path and must return its parsed root fkyaml::node.
     * Lets an application route scene loading through something other than a
     * plain YAML file on disk (e.g. an encrypted/compressed container) without
     * SceneLoader depending on whatever library that requires.
     */
    using DocumentLoader = std::function<fkyaml::node(const std::string& path)>;

    /**
     * @brief Loads a scene from a YAML file (or whatever set_document_loader() routes to).
     *
     * Constructs the full Scene hierarchy. Every non-Transform component is
     * handed to whatever parser is registered for its tag; an unregistered
     * tag is silently skipped for forward compatibility.
     *
     * @param path Absolute or relative path to the scene file.
     * @return Fully constructed Scene.
     * @throws std::runtime_error on parse errors or missing files.
     */
    static Scene load(const std::string& path) { return load(path, LoadOptions{}); }

    /** @brief How a scene is built from its document -- see SceneLoaderOptions. */
    using LoadOptions = SceneLoaderOptions;

    /** @brief load(), with LoadOptions (e.g. `{.start = false}` for an unstarted scene). */
    static Scene load(const std::string& path, const LoadOptions& options) {
        return build_(read_document(path), path, options);
    }

    /**
     * @brief Reads a scene file and expands every `inherit_from` / `prefab` reference into one
     *        fully-merged document -- everything load() does before it builds objects.
     *
     * Pure document work (file reads, the registered decoders, fkYAML, SceneInheritance), so it
     * is safe on a worker thread -- unless a custom document loader (set_document_loader()) is
     * installed that is not. Component parsers are never run here.
     *
     * @throws std::runtime_error on a missing file or a parse error.
     */
    static fkyaml::node read_document(const std::string& path) {
        return SceneInheritance::resolve(path, [](const std::string& p) { return load_node_(p); });
    }

    /**
     * @brief The result of read_document_async(): poll ready() each frame, then read
     *        document() (or error() when failed()). Shared with the job that fills it.
     */
    class DocumentRead {
    public:
        /// True once the read has finished, successfully or not.
        bool ready() const { return done_.load(std::memory_order_acquire); }
        /// True when ready() and the read threw; error() says why.
        bool failed() const { return ready() && failed_; }
        const std::string& error() const { return error_; }
        /// The resolved document (read_document()'s result). Only valid once ready() && !failed().
        const fkyaml::node& document() const { return document_; }
        /// The scene file it was read from.
        const std::string& path() const { return path_; }

    private:
        friend class SceneLoader;
        std::string       path_;
        fkyaml::node      document_;
        std::string       error_;
        bool              failed_ = false;
        std::atomic<bool> done_{false};

        void run_() {
            try {
                document_ = read_document(path_);
            } catch (const std::exception& e) {
                failed_ = true;
                error_  = e.what();
            }
            done_.store(true, std::memory_order_release);
        }
    };

    /**
     * @brief read_document() on a job: the file read, decode and inheritance expansion run on
     *        `jobs`' workers (Low priority, like asset decodes) while the caller keeps ticking.
     *
     * With no JobEngine the read happens inline and the result comes back already ready().
     * The returned object stays valid however long the job takes; dropping it early is safe
     * (the job finishes into its own copy and nothing reads it).
     */
    static std::shared_ptr<DocumentRead> read_document_async(const std::string& path, coopa::job::JobEngine* jobs) {
        auto read = std::make_shared<DocumentRead>();
        read->path_ = path;
        if (!jobs) {
            read->run_();
            return read;
        }
        coopa::job::JobHandle handle = jobs->create_handle();
        if (!handle.is_valid()) {   // handle pool exhausted: read inline rather than fail
            read->run_();
            return read;
        }
        jobs->submit([read]() { read->run_(); }, k_document_job_type, handle, nullptr, 0u, coopa::job::Priority::Low);
        // Completion is tracked by DocumentRead::ready(), so the handle can be closed right away;
        // its slot is reclaimed when the job finishes (see coopa/job/handle.h).
        handle.close();
        return read;
    }

    /**
     * @class Builder
     * @brief Builds a Scene from a resolved document a few root objects at a time, so a large
     *        scene can be constructed across several frames while another one keeps running.
     *
     * Component parsers run here, so this is main-thread work, like load(). Each step() builds
     * whole root objects (children included) until its time budget is spent -- always at least
     * one, so every call makes progress. The scene is not started unless LoadOptions::start
     * is set, and then only when take() hands it over.
     *
     * @code
     * SceneLoader::Builder b(SceneLoader::read_document(path), path, {.start = false});
     * while (!b.step(4.0)) render_a_frame();
     * std::unique_ptr<Scene> scene = b.take();
     * @endcode
     */
    class Builder {
    public:
        /**
         * @param document A resolved scene document (read_document() / DocumentRead::document()).
         * @param path     The scene file's path, for relative asset paths.
         * @param options  LoadOptions; `start` is applied by take().
         */
        Builder(fkyaml::node document, std::string path, LoadOptions options = {})
            : document_(std::move(document)), path_(std::move(path)), options_(options),
              ctx_{ path_, std::filesystem::path(path_).parent_path().string(),
                    { std::filesystem::path(path_).parent_path().string() } }
        {
            std::string scene_name = "Scene";
            if (document_.contains("scene")) {
                const auto& scene_node = document_.at("scene");
                if (scene_node.contains("scene_name")) scene_name = scene_node.at("scene_name").get_value<std::string>();
                if (scene_node.contains("auto_transform")) auto_transform_ = scene_node.at("auto_transform").get_value<bool>();
                if (scene_node.contains("root_objects") && scene_node.at("root_objects").is_sequence()) {
                    total_ = scene_node.at("root_objects").size();
                }
            }
            scene_ = std::make_unique<Scene>(scene_name);
        }

        /**
         * @brief Builds root objects until `budget_ms` of wall time has passed (at least one).
         * @return True once every root object is built (done()).
         * @throws whatever a component parser throws; the builder is then unusable.
         */
        bool step(double budget_ms) {
            if (done()) return true;
            const auto t0 = std::chrono::steady_clock::now();
            const auto& roots = document_.at("scene").at("root_objects");
            do {
                scene_->add_root_object(parse_object_(roots[built_], nullptr, auto_transform_, ctx_));
                ++built_;
            } while (!done() && std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() < budget_ms);
            return done();
        }

        /** @brief Builds everything that is left in one go. */
        void finish() { step(std::numeric_limits<double>::infinity()); }

        bool   done() const  { return built_ >= total_; }
        /// Root objects built so far, of total().
        size_t built() const { return built_; }
        size_t total() const { return total_; }
        /// built() / total(), 1 for an empty scene.
        float  progress() const { return total_ == 0 ? 1.0f : static_cast<float>(built_) / static_cast<float>(total_); }

        /// The scene being built (not yet started), for a caller that wants to look at it early.
        Scene* scene() const { return scene_.get(); }

        /**
         * @brief Hands over the built scene, starting it first if LoadOptions::start is set.
         *        Builds whatever is left if called before done(). Call once.
         */
        std::unique_ptr<Scene> take() {
            if (!done()) finish();
            if (options_.start && scene_) scene_->start();
            return std::move(scene_);
        }

    private:
        fkyaml::node           document_;
        std::string            path_;
        LoadOptions            options_;
        ParseContext           ctx_;
        std::unique_ptr<Scene> scene_;
        bool                   auto_transform_ = true;
        size_t                 total_ = 0;
        size_t                 built_ = 0;
    };

    /**
     * @brief Builds a scene from an already-parsed (not yet inheritance-resolved) document.
     *
     * The in-memory twin of load(): an editor keeps the scene document it is editing as a
     * node and rebuilds the live Scene from it without a round trip through disk. `path` is
     * where the document lives (or will live) -- it anchors relative asset paths and
     * `inherit_from` references exactly as load() would; base documents are still read from
     * disk.
     *
     * @param root The scene document (`scene:` at its top level).
     * @param path The document's file path, existing or not.
     */
    static Scene load_from_node(const fkyaml::node& root, const std::string& path) {
        return load_from_node(root, path, LoadOptions{});
    }

    /** @brief load_from_node(), with LoadOptions -- e.g. `{.start = false}` to build the scene
     *         unstarted, so a caller whose components bind eagerly in start() (not lazily, on
     *         their first real tick) can drain any load_async() asset loads the parsers just
     *         queued before starting it, instead of components binding against assets that
     *         are still Loading. */
    static Scene load_from_node(const fkyaml::node& root, const std::string& path, const LoadOptions& options) {
        std::error_code ec;
        const std::filesystem::path self = std::filesystem::weakly_canonical(path, ec);
        fkyaml::node resolved = SceneInheritance::resolve(path, [&](const std::string& p) {
            std::error_code ec2;
            if (std::filesystem::weakly_canonical(p, ec2) == self || p == path) return root;
            return load_node_(p);
        });
        return build_(resolved, path, options);
    }

    /**
     * @brief Builds one object (and its subtree) from an already-resolved object node.
     *
     * For an editor rebuilding a single edited object in place. The returned object is not
     * started and belongs to no scene; the caller parents it (Scene::add_root_object() +
     * Scene::adopt(), or SceneObject::add_child()) and then calls start().
     *
     * @param node             The object node (`name`, `components`, `children`).
     * @param parent_transform The new parent's TransformComponent, or null for a root object.
     * @param scene_path       The owning scene file's path, for relative asset paths.
     * @param auto_transform   As the scene's `auto_transform` key (default true).
     */
    static std::unique_ptr<SceneObject> build_object(const fkyaml::node& node,
                                                     TransformComponent* parent_transform,
                                                     const std::string& scene_path,
                                                     bool auto_transform = true) {
        const std::string scene_dir = std::filesystem::path(scene_path).parent_path().string();
        ParseContext ctx{ scene_path, scene_dir, { scene_dir } };
        if (node.is_mapping() && (node.contains("prefab") || node.contains("inherit_from"))) {
            const fkyaml::node resolved = SceneInheritance::resolve_object(node, scene_path, [](const std::string& p) { return load_node_(p); });
            return parse_object_(resolved, parent_transform, auto_transform, ctx);
        }
        return parse_object_(node, parent_transform, auto_transform, ctx);
    }

    /**
     * @brief The project's asset roots, searched for `prefab:` / `inherit_from` references
     *        after the declaring file's folder (so a scene anywhere can say `objects/crate`).
     */
    static void set_search_roots(std::vector<std::string> roots) { SceneInheritance::set_search_roots(std::move(roots)); }

    /**
     * @brief Builds an instance of an object asset (`objects/crate`, or a path), with optional
     *        overrides merged on top (`components:` / `children:` as in a scene entry). Not
     *        started and unparented -- see Scene::instantiate() for the usual one-call spawn.
     * @throws std::runtime_error if the asset can't be found or parsed.
     */
    static std::unique_ptr<SceneObject> instantiate(const std::string& ref, const fkyaml::node* overrides = nullptr,
                                                    TransformComponent* parent_transform = nullptr) {
        fkyaml::node obj = (overrides && overrides->is_mapping()) ? *overrides : fkyaml::node::mapping();
        obj["prefab"] = fkyaml::node(ref);
        const auto& roots = SceneInheritance::search_roots();
        const std::string anchor = roots.empty() ? std::filesystem::current_path().string() : roots.front();
        const std::string fake_path = (std::filesystem::path(anchor) / "__spawn__.yaml").string();
        const fkyaml::node resolved = SceneInheritance::resolve_object(obj, fake_path, [](const std::string& p) { return load_node_(p); });
        if (!resolved.contains("components") && !resolved.contains("children")) {
            throw std::runtime_error("[SceneLoader] instantiate: object asset '" + ref + "' not found or empty");
        }
        ParseContext ctx{ fake_path, anchor, { anchor } };
        return parse_object_(resolved, parent_transform, true, ctx);
    }

    /**
     * @brief Spawns an object asset into a running scene: builds it (instantiate()), parents
     *        it (root, or under `parent`), stamps it into the scene and calls start() on it.
     *        Main thread only (parsers are not thread-safe); from a job, enqueue a lambda on
     *        the scene's SceneCommandBuffer that calls this.
     * @return The new object (owned by the scene).
     */
    static SceneObject* spawn(Scene& scene, const std::string& ref, SceneObject* parent = nullptr,
                              const fkyaml::node* overrides = nullptr) {
        TransformComponent* parent_tc = parent ? parent->get_transform() : nullptr;
        auto obj = instantiate(ref, overrides, parent_tc);
        SceneObject* raw = obj.get();
        if (parent) parent->add_child(std::move(obj));
        else scene.add_root_object(std::move(obj));
        scene.adopt(*raw);
        raw->start();
        return raw;
    }

    /**
     * @brief Signature for an object observer: called once per object built, after its
     *        components and before its children, with the object node it came from.
     */
    using ObjectObserver = std::function<void(const fkyaml::node& node, SceneObject& obj)>;

    /**
     * @brief Installs (or, with an empty function, removes) the process-wide object observer.
     *
     * Lets a tool map document nodes to the live objects built from them -- e.g. an editor
     * stamps a private id key into each object node and records id -> SceneObject*. Unknown
     * keys are otherwise ignored by the loader, so the stamp changes nothing else.
     */
    static void set_object_observer(ObjectObserver fn) {
        std::unique_lock<std::shared_mutex> lock(loader_mutex_());
        object_observer_() = std::move(fn);
    }

private:
    static Scene build_(const fkyaml::node& root, const std::string& path, const LoadOptions& options) {
        Builder builder(root, path, options);
        builder.finish();
        return std::move(*builder.take());
    }

    /// Job type for read_document_async()'s jobs: >= k_max_job_types, so like the asset IO jobs
    /// it opts out of thread dedication and the diagnostics counters.
    static constexpr coopa::job::JobType k_document_job_type = 0x5CE0D0C0u;

public:
    /**
     * @brief Registers a parser for a component's YAML tag/type name.
     *
     * Lets libraries outside libcoopa (e.g. gfxcoopa, uicoopa) extend scene
     * loading without SceneLoader depending on them. Registering the same
     * name twice replaces the previous parser. Names are matched after
     * normalization: a leading '!' is stripped, so registering "MeshRenderer"
     * matches both a "!MeshRenderer" YAML tag and a "type: MeshRenderer" key.
     * The built-in name ("Transform") cannot be overridden — it is handled
     * before the registry is consulted.
     *
     * @param name Component name as it appears in the scene file, with or without a leading '!'.
     * @param fn   Parser invoked when a component node resolves to this name.
     */
    static void register_component_parser(const std::string& name, ComponentParser fn) {
        std::unique_lock<std::shared_mutex> lock(parsers_mutex_());
        parsers_()[normalize_component_tag(name)] = std::move(fn);
    }

    /**
     * @brief Clears every registered component parser.
     *
     * Registered parsers commonly capture references (e.g. a Device or
     * Allocator) by reference in their closures; call this before those
     * referenced objects are destroyed to avoid dangling captures surviving
     * into a later SceneLoader::load() call.
     */
    static void clear_component_parsers() {
        std::unique_lock<std::shared_mutex> lock(parsers_mutex_());
        parsers_().clear();
    }

    /**
     * @brief Installs a custom raw-document loader, consulted by every subsequent load().
     *
     * Pass an empty std::function to restore the default fkYAML-file behavior.
     *
     * @param fn Loader callback, or an empty function to reset to the default.
     */
    static void set_document_loader(DocumentLoader fn) {
        std::unique_lock<std::shared_mutex> lock(loader_mutex_());
        document_loader_() = std::move(fn);
    }

private:
    /**
     * @brief Function-local static registry of registered component parsers,
     *        and the shared_mutex guarding it.
     *
     * These are process-wide mutable statics (every SceneLoader::load() call,
     * from any thread, reads them). register_component_parser()/
     * clear_component_parsers() take the unique lock; parse_component_()
     * takes the shared lock just long enough to copy out the one
     * ComponentParser it needs, then calls it unlocked -- so a parser that
     * itself calls back into register_component_parser() (or simply takes a
     * while) never holds the lock re-entrantly or blocks unrelated readers.
     * This makes the *registry* thread-safe; it does NOT make an individual
     * registered parser thread-safe to run from a worker -- most existing
     * parsers (e.g. gfxcoopa's, which capture an AssetManager& that is
     * itself documented main-thread-only) are not, so scene loading should
     * still be treated as main-thread work by default.
     */
    static std::unordered_map<std::string, ComponentParser>& parsers_() {
        static std::unordered_map<std::string, ComponentParser> registry;
        return registry;
    }

    static std::shared_mutex& parsers_mutex_() {
        static std::shared_mutex mutex;
        return mutex;
    }

    /** @brief Function-local static holder for the optional custom document loader, and its mutex. */
    static DocumentLoader& document_loader_() {
        static DocumentLoader loader;
        return loader;
    }

    static std::shared_mutex& loader_mutex_() {
        static std::shared_mutex mutex;
        return mutex;
    }

    static ObjectObserver& object_observer_() {
        static ObjectObserver observer;
        return observer;
    }

    /** @brief Loads and parses the root node, via the custom loader if one is installed. */
    static fkyaml::node load_node_(const std::string& path) {
        DocumentLoader loader_copy;
        {
            std::shared_lock<std::shared_mutex> lock(loader_mutex_());
            loader_copy = document_loader_();
        }
        if (loader_copy) {
            return loader_copy(path);
        }
        const std::filesystem::path resolved = coopa::yaml::resolve_variant(path);
        if (!std::filesystem::exists(resolved)) {
            throw std::runtime_error("[SceneLoader] Failed to open scene file: " + path);
        }
        return coopa::yaml::load_document(resolved);
    }

    /**
     * @brief Recursively parses a SceneObject from a YAML node.
     */
    static std::unique_ptr<SceneObject> parse_object_(
        const fkyaml::node& node,
        TransformComponent* parent_transform,
        bool auto_transform,
        const ParseContext& ctx)
    {
        std::string name   = node.contains("name")   ? node.at("name").get_value<std::string>() : "Object";
        bool        active = node.contains("active")  ? node.at("active").get_value<bool>()      : true;

        auto obj = std::make_unique<SceneObject>(name, active);

        TransformComponent* tc = nullptr;
        if (auto_transform) {
            tc = obj->add_component<TransformComponent>();
            if (parent_transform) {
                tc->set_parent_transform(&parent_transform->transform());
            }
        }

        if (node.contains("components")) {
            for (const auto& comp_node : node.at("components")) {
                ParseContext comp_ctx = ctx;
                if (comp_node.contains(SceneInheritance::kSourceDirsKey)) {
                    comp_ctx.search_dirs.clear();
                    for (const auto& d : comp_node.at(SceneInheritance::kSourceDirsKey)) {
                        comp_ctx.search_dirs.push_back(d.get_value<std::string>());
                    }
                }
                parse_component_(comp_node, *obj, comp_ctx);
            }
        }

        ObjectObserver observer;
        {
            std::shared_lock<std::shared_mutex> lock(loader_mutex_());
            observer = object_observer_();
        }
        if (observer) observer(node, *obj);

        if (node.contains("children")) {
            for (const auto& child_node : node.at("children")) {
                auto child = parse_object_(child_node, tc, auto_transform, ctx);
                obj->add_child(std::move(child));
            }
        }

        return obj;
    }

    /**
     * @brief Parses a single component node and attaches it to the SceneObject.
     *
     * fkYAML exposes YAML tags (e.g. "!Transform") via node.get_tag_name();
     * a "type:" key is the fallback for untagged nodes.
     */
    static void parse_component_(const fkyaml::node& node, SceneObject& obj, const ParseContext& ctx) {
        std::string raw_tag;
        if (!node.has_tag_name()) {
            if (!node.contains("type")) return;
            raw_tag = node.at("type").get_value<std::string>();
        } else {
            raw_tag = node.get_tag_name();
        }
        std::string tag = normalize_component_tag(raw_tag);

        if (tag == "Transform") {
            if (auto* tc = obj.get_transform()) {
                tc->transform().set_from_node(node);
            }
            // No TransformComponent to apply to (auto_transform: false) — silently skipped;
            // a scene that opts out of auto transforms should not carry !Transform nodes.
        } else {
            ComponentParser parser_copy;
            {
                std::shared_lock<std::shared_mutex> lock(parsers_mutex_());
                auto& registry = parsers_();
                auto it = registry.find(tag);
                if (it != registry.end()) parser_copy = it->second;
            }
            if (parser_copy) {
                parser_copy(node, obj, ctx);
            }
            // Unrecognized names fall through silently for forward compatibility.
        }
    }
};

} // namespace scene
} // namespace coopa

#endif // COOPA_SCENE_SCENE_LOADER_H
