/**
 * @file nav_agent.h
 * @brief NavAgent -- a scene object that moves itself across the navmesh: to a point or a
 *        (moving) object by A*, or down a shared flow field toward an object, with crowd
 *        separation, wall sliding, stairs and off-mesh links handled by NavSystem.
 */

#ifndef PHYSXCOOPA_COMPONENTS_NAV_AGENT_H
#define PHYSXCOOPA_COMPONENTS_NAV_AGENT_H

#include <physxcoopa/nav/nav_types.h>

#include <coopa/scene/component.h>
#include <coopa/event/signal.h>

#include <glm/glm.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace coopa {
namespace physx {
namespace components {

/**
 * @class NavAgentComponent
 * @brief Pure data plus a small command API; NavSystem does the work. The agent drives its
 *        object's Transform directly (position on the surface + `base_offset`, yaw toward the
 *        direction of travel), so it should not also carry a dynamic Rigidbody -- give it a
 *        kinematic one if physics should see it.
 *
 * Modes, in priority order:
 *   - `flow_target` set: follow the flow field toward that object (shared by every agent with
 *     the same target and agent type, which is what makes thousands of followers cheap).
 *   - `destination_object` set: A* to that object, re-planning as it moves.
 *   - set_destination(): A* to a fixed point.
 */
class NavAgentComponent : public coopa::scene::Component {
public:
    std::string type_name() const override { return "NavAgent"; }

    /** @brief Agent profile name from navigation settings; empty = the first profile. */
    std::string agent_type;
    float speed = 3.5f;
    float acceleration = 12.0f;
    /** @brief Turn rate toward the direction of travel, degrees per second (0 = instant). */
    float angular_speed = 720.0f;
    float stopping_distance = 0.15f;
    float slowdown_distance = 1.0f;
    /** @brief Separation radius; negative = the profile's radius. */
    float radius = -1.0f;
    /** @brief Neighbour push strength; 0 turns crowd avoidance off for this agent. */
    float separation_weight = 2.0f;
    /** @brief Height of the object's origin above the surface. */
    float base_offset = 0.0f;
    bool update_rotation = true;
    /** @brief Which local axis faces the direction of travel: "Y", "-Y", "X" or "-X". */
    std::string forward = "Y";
    /** @brief Area names this agent never enters. */
    std::vector<std::string> avoid_areas;
    /** @brief A* heuristic weight for this agent's queries (see QueryFilter). */
    float heuristic_weight = 1.1f;

    /** @brief Object to chase by A* (re-planned as it moves). */
    std::string destination_object;
    /** @brief Object whose flow field to follow. */
    std::string flow_target;

    /** @brief Fired once each time the agent reaches its destination. */
    coopa::event::Signal<NavAgentComponent&> on_arrived;

    // --- Commands -----------------------------------------------------------------------------

    /** @brief A* to a fixed point (clears destination_object and flow_target). */
    void set_destination(const glm::vec3& p) {
        destination_ = p;
        has_destination_ = true;
        destination_object.clear();
        flow_target.clear();
        ++command_;
    }
    /** @brief Follow `object`'s flow field. */
    void set_flow_target(const std::string& object) {
        flow_target = object;
        destination_object.clear();
        has_destination_ = false;
        ++command_;
    }
    /** @brief Chase `object` by A*. */
    void set_destination_object(const std::string& object) {
        destination_object = object;
        flow_target.clear();
        has_destination_ = false;
        ++command_;
    }
    void stop() {
        has_destination_ = false;
        destination_object.clear();
        flow_target.clear();
        ++command_;
    }
    /** @brief Teleports the agent (it re-snaps to the surface next update). */
    void warp(const glm::vec3& p) {
        warp_to_ = p;
        warp_pending_ = true;
    }

    // --- State (written by NavSystem) ----------------------------------------------------------

    bool has_destination() const { return has_destination_; }
    const glm::vec3& destination() const { return destination_; }
    bool arrived() const { return arrived_; }
    bool path_pending() const { return path_pending_; }
    nav::PathStatus path_status() const { return path_status_; }
    const nav::NavPath& path() const { return path_; }
    const glm::vec3& velocity() const { return velocity_; }
    float remaining_distance() const { return remaining_distance_; }
    bool on_navmesh() const { return on_navmesh_; }

    // --- NavSystem plumbing ------------------------------------------------------------------

    uint32_t command_revision() const { return command_; }
    bool take_warp(glm::vec3& out) {
        if (!warp_pending_) return false;
        out = warp_to_;
        warp_pending_ = false;
        return true;
    }
    void set_state_(bool arrived, const glm::vec3& velocity, float remaining, bool on_navmesh) {
        arrived_ = arrived;
        velocity_ = velocity;
        remaining_distance_ = remaining;
        on_navmesh_ = on_navmesh;
    }
    void set_path_(nav::NavPath path) {
        path_status_ = path.status;
        path_ = std::move(path);
        path_pending_ = false;
    }
    void set_path_pending_(bool v) { path_pending_ = v; }

private:
    glm::vec3 destination_{0.0f};
    bool has_destination_ = false;
    uint32_t command_ = 0;
    glm::vec3 warp_to_{0.0f};
    bool warp_pending_ = false;

    bool arrived_ = false;
    bool path_pending_ = false;
    bool on_navmesh_ = false;
    nav::PathStatus path_status_ = nav::PathStatus::NoPath;
    nav::NavPath path_;
    glm::vec3 velocity_{0.0f};
    float remaining_distance_ = 0.0f;
};

} // namespace components
} // namespace physx
} // namespace coopa

#endif // PHYSXCOOPA_COMPONENTS_NAV_AGENT_H
