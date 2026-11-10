/**
 * @file stat_block.h
 * @brief A named collection of Resources -- health, stamina, mana, etc. by name.
 */

#ifndef COOPA_STAT_STAT_BLOCK_H
#define COOPA_STAT_STAT_BLOCK_H

#include <coopa/stat/resource.h>
#include <string>
#include <unordered_map>

namespace coopa {
namespace stat {

/**
 * @class StatBlock
 * @brief Owns a set of named Resource instances (e.g. "health", "stamina").
 *
 * Backed by std::unordered_map, which is node-based -- unlike a
 * std::vector<Resource>, inserting a new named resource never invalidates a
 * `Resource*` obtained from an earlier resource() call, which matters because
 * a UI widget (uicoopa's ProgressBar::bind()) holds such a pointer for the
 * lifetime of its binding.
 */
class StatBlock {
public:
    /** @brief Returns the named resource, default-constructing one (100/100,
     *         no regen) on first access. */
    Resource& resource(const std::string& name) {
        return resources_[name];
    }

    Resource* find(const std::string& name) {
        auto it = resources_.find(name);
        return it != resources_.end() ? &it->second : nullptr;
    }
    const Resource* find(const std::string& name) const {
        auto it = resources_.find(name);
        return it != resources_.end() ? &it->second : nullptr;
    }

    bool contains(const std::string& name) const { return resources_.count(name) != 0; }
    void clear() { resources_.clear(); }
    size_t size() const { return resources_.size(); }

    using iterator = std::unordered_map<std::string, Resource>::iterator;
    using const_iterator = std::unordered_map<std::string, Resource>::const_iterator;
    iterator begin() { return resources_.begin(); }
    iterator end() { return resources_.end(); }
    const_iterator begin() const { return resources_.begin(); }
    const_iterator end() const { return resources_.end(); }

private:
    std::unordered_map<std::string, Resource> resources_;
};

} // namespace stat
} // namespace coopa

#endif // COOPA_STAT_STAT_BLOCK_H
