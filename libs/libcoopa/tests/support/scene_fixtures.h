#pragma once

/**
 * @file scene_fixtures.h
 * @brief Test-only ISceneSystem / Component doubles shared by the scene, scene_manager and
 *        routine_system suites.
 */

#include <coopa/scene/component.h>
#include <coopa/scene/scene.h>
#include <coopa/scene/scene_system.h>

#include <string>
#include <utility>
#include <vector>

namespace libcoopa_test {

/** @brief Appends its name to a shared log every time it executes -- for asserting call order. */
class RecordingSystem : public coopa::scene::ISceneSystem {
public:
    RecordingSystem(std::string name, std::vector<std::string>* log)
        : name_(std::move(name)), log_(log) {}
    void execute(coopa::scene::Scene&, const coopa::scene::FrameContext&) override {
        log_->push_back(name_);
    }
    const char* system_name() const override { return name_.c_str(); }

private:
    std::string               name_;
    std::vector<std::string>* log_;
};

/** @brief A test-only Component whose update() counts how many times it ran. */
class CountingComponent : public coopa::scene::Component {
public:
    std::string type_name() const override { return "Counting"; }
    int update_count = 0;
    void update(float) override { ++update_count; }
};

} // namespace libcoopa_test
