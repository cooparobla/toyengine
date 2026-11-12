/**
 * @file aabb_tree.h
 * @brief Dynamic AABB tree broadphase (the Box2D b2DynamicTree design): cost-minimizing
 *        leaf insertion with fat AABBs, so a proxy only needs re-insertion once it escapes
 *        its enlarged bounds.
 *
 * No AVL-style rotation rebalancing: insertion still refits every ancestor's AABB on the way
 * up (so query correctness is unaffected), it just doesn't rebalance subtree height
 * afterward. That trades some query performance on pathological insertion orders for a
 * smaller, lower-risk implementation -- the requirement is "identical pair set to brute
 * force," which an unbalanced-but-correct tree satisfies exactly as well as a balanced one.
 */

#ifndef PHYSXCOOPA_BROADPHASE_AABB_TREE_H
#define PHYSXCOOPA_BROADPHASE_AABB_TREE_H

#include <physxcoopa/geometry/aabb.h>
#include <physxcoopa/geometry/ray.h>

#include <glm/glm.hpp>

#include <algorithm>
#include <cstdint>
#include <vector>

namespace coopa {
namespace physx {
namespace broadphase {

/** @brief Sentinel meaning "no node" (null child/parent, empty tree). */
inline constexpr int32_t k_null_node = -1;

/**
 * @class AABBTree
 * @brief A dynamic AABB tree over opaque `void*` user data, keyed by proxy id.
 */
class AABBTree {
public:
    explicit AABBTree(float margin = 0.1f) : margin_(margin) {}

    /**
     * @brief Inserts a new proxy with a fattened version of `tight_bounds`.
     * @return The new proxy's id, stable until destroy_proxy() is called on it.
     */
    int32_t create_proxy(const geometry::AABB& tight_bounds, void* user_data) {
        int32_t proxy = allocate_node_();
        nodes_[proxy].aabb = tight_bounds.expand(margin_);
        nodes_[proxy].user_data = user_data;
        nodes_[proxy].height = 0;
        insert_leaf_(proxy);
        return proxy;
    }

    /** @brief Removes a proxy. `proxy` must not be used again after this call. */
    void destroy_proxy(int32_t proxy) {
        remove_leaf_(proxy);
        free_node_(proxy);
    }

    /**
     * @brief Updates a proxy's tight bounds, PREDICTIVELY fattened along `displacement` -- the
     *        swept volume the leaf is expected to cover before its next refit, so a fast body's
     *        candidate-pair set includes a thin target it's about to pass through, not just what
     *        it already overlaps this instant. This is what CCD's speculative-contact path
     *        (collision/sat.h's `allow_separated`, PhysicsWorld::narrowphase_()) needs from
     *        broadphase: the pair has to be DISCOVERED before a per-pair speculative test can
     *        ever run on it. Only triggers a tree re-insertion (relatively expensive) when the
     *        swept bounds have escaped the proxy's current fat bounds.
     *
     * @param displacement Swept offset to fatten the refit bounds by, in addition to the usual
     *                      `margin_` -- normally `velocity * h` for the upcoming substep (see
     *                      PhysicsWorld::sync_broadphase_()'s call site). A zero vector
     *                      degenerates to plain un-swept refitting.
     * @return True if the proxy was actually re-inserted.
     */
    bool move_proxy(int32_t proxy, const geometry::AABB& tight_bounds, const glm::vec3& displacement) {
        geometry::AABB swept = tight_bounds;
        swept.min = glm::min(swept.min, tight_bounds.min + displacement);
        swept.max = glm::max(swept.max, tight_bounds.max + displacement);
        if (nodes_[proxy].aabb.contains(swept)) return false;
        remove_leaf_(proxy);
        nodes_[proxy].aabb = swept.expand(margin_);
        insert_leaf_(proxy);
        return true;
    }

    /** @brief Returns a proxy's current fat AABB. */
    const geometry::AABB& fat_bounds(int32_t proxy) const { return nodes_[proxy].aabb; }

    /** @brief Returns a proxy's user data. */
    void* user_data(int32_t proxy) const { return nodes_[proxy].user_data; }

    /**
     * @brief Invokes `fn(void* user_data)` for every leaf whose fat AABB overlaps `bounds`.
     *
     * The traversal stack is `thread_local` (not a member) so that PhysicsWorld's job-parallel
     * pair-discovery pass (many worker threads calling query() on the same tree concurrently,
     * each read-only) never races another thread's traversal. Reused across calls on the same
     * thread, so traversal does not allocate in steady state.
     */
    template <typename Fn>
    void query(const geometry::AABB& bounds, Fn&& fn) const {
        if (root_ == k_null_node) return;
        thread_local std::vector<int32_t> stack;
        stack.clear();
        stack.push_back(root_);
        while (!stack.empty()) {
            int32_t idx = stack.back();
            stack.pop_back();
            const Node& node = nodes_[idx];
            if (!node.aabb.overlaps(bounds)) continue;
            if (is_leaf_(node)) {
                fn(node.user_data);
            } else {
                stack.push_back(node.child1);
                stack.push_back(node.child2);
            }
        }
    }

    /** @brief Invokes `fn(void* user_data)` for every leaf whose fat AABB the ray intersects.
     *         See query()'s doc -- same thread_local traversal stack, same reason. */
    template <typename Fn>
    void raycast(const geometry::Ray& ray, Fn&& fn) const {
        if (root_ == k_null_node) return;
        thread_local std::vector<int32_t> stack;
        stack.clear();
        stack.push_back(root_);
        while (!stack.empty()) {
            int32_t idx = stack.back();
            stack.pop_back();
            const Node& node = nodes_[idx];
            float t;
            if (!ray.intersect(node.aabb, t)) continue;
            if (is_leaf_(node)) {
                fn(node.user_data);
            } else {
                stack.push_back(node.child1);
                stack.push_back(node.child2);
            }
        }
    }

    /**
     * @brief Invokes `fn(void* user_data)` for every leaf whose fat AABB the ray intersects, IN
     *        TRAVERSAL ORDER, stopping as soon as `fn` returns true -- for an "is anything in
     *        the way" query that doesn't need the closest hit, just any hit. A separate method
     *        so raycast()'s `fn` can stay void-returning. See raycast()'s doc for the shared
     *        traversal-stack rationale.
     */
    template <typename Fn>
    void raycast_until(const geometry::Ray& ray, Fn&& fn) const {
        if (root_ == k_null_node) return;
        thread_local std::vector<int32_t> stack;
        stack.clear();
        stack.push_back(root_);
        while (!stack.empty()) {
            int32_t idx = stack.back();
            stack.pop_back();
            const Node& node = nodes_[idx];
            float t;
            if (!ray.intersect(node.aabb, t)) continue;
            if (is_leaf_(node)) {
                if (fn(node.user_data)) return;
            } else {
                stack.push_back(node.child1);
                stack.push_back(node.child2);
            }
        }
    }

    /** @brief True if the tree currently has no proxies. */
    bool empty() const { return root_ == k_null_node; }

    /**
     * @brief Invokes `fn(const geometry::AABB&, int32_t height)` for every allocated node
     *        (leaf and internal), for debug visualization (debug/debug_draw.h). `height` is 0
     *        for a leaf, >0 for an internal node -- free-list nodes (height == -1) are skipped.
     */
    template <typename Fn>
    void for_each_node(Fn&& fn) const {
        for (const Node& node : nodes_) {
            if (node.height < 0) continue;
            fn(node.aabb, node.height);
        }
    }

private:
    struct Node {
        geometry::AABB aabb;
        void* user_data = nullptr;
        int32_t parent = k_null_node; /**< Doubles as "next free" link when this node is free. */
        int32_t child1 = k_null_node;
        int32_t child2 = k_null_node;
        int32_t height = -1; /**< -1 = free node, 0 = leaf, >0 = internal. */
    };

    static bool is_leaf_(const Node& n) { return n.child1 == k_null_node; }

    int32_t allocate_node_() {
        if (free_list_ == k_null_node) {
            nodes_.push_back(Node{});
            return static_cast<int32_t>(nodes_.size()) - 1;
        }
        int32_t idx = free_list_;
        free_list_ = nodes_[idx].parent;
        nodes_[idx] = Node{};
        return idx;
    }

    void free_node_(int32_t idx) {
        nodes_[idx].height = -1;
        nodes_[idx].parent = free_list_;
        free_list_ = idx;
    }

    float cost_for_child_(int32_t child, const geometry::AABB& leaf_aabb) const {
        geometry::AABB combined = geometry::AABB::merge(nodes_[child].aabb, leaf_aabb);
        if (is_leaf_(nodes_[child])) return combined.surface_area();
        return combined.surface_area() - nodes_[child].aabb.surface_area();
    }

    void insert_leaf_(int32_t leaf) {
        if (root_ == k_null_node) {
            root_ = leaf;
            nodes_[leaf].parent = k_null_node;
            return;
        }

        geometry::AABB leaf_aabb = nodes_[leaf].aabb;
        int32_t idx = root_;
        while (!is_leaf_(nodes_[idx])) {
            int32_t c1 = nodes_[idx].child1;
            int32_t c2 = nodes_[idx].child2;

            float area = nodes_[idx].aabb.surface_area();
            geometry::AABB combined = geometry::AABB::merge(nodes_[idx].aabb, leaf_aabb);
            float combined_area = combined.surface_area();
            float cost = 2.0f * combined_area;
            float inherit_cost = 2.0f * (combined_area - area);

            float cost1 = cost_for_child_(c1, leaf_aabb) + inherit_cost;
            float cost2 = cost_for_child_(c2, leaf_aabb) + inherit_cost;

            if (cost < cost1 && cost < cost2) break;
            idx = (cost1 < cost2) ? c1 : c2;
        }

        int32_t sibling = idx;
        int32_t old_parent = nodes_[sibling].parent;
        int32_t new_parent = allocate_node_();
        nodes_[new_parent].parent = old_parent;
        nodes_[new_parent].aabb = geometry::AABB::merge(leaf_aabb, nodes_[sibling].aabb);
        nodes_[new_parent].height = nodes_[sibling].height + 1;
        nodes_[new_parent].child1 = sibling;
        nodes_[new_parent].child2 = leaf;
        nodes_[sibling].parent = new_parent;
        nodes_[leaf].parent = new_parent;

        if (old_parent == k_null_node) {
            root_ = new_parent;
        } else {
            if (nodes_[old_parent].child1 == sibling) nodes_[old_parent].child1 = new_parent;
            else nodes_[old_parent].child2 = new_parent;
        }

        refit_ancestors_(nodes_[leaf].parent);
    }

    void remove_leaf_(int32_t leaf) {
        if (leaf == root_) {
            root_ = k_null_node;
            return;
        }
        int32_t parent = nodes_[leaf].parent;
        int32_t grandparent = nodes_[parent].parent;
        int32_t sibling = (nodes_[parent].child1 == leaf) ? nodes_[parent].child2 : nodes_[parent].child1;

        if (grandparent == k_null_node) {
            root_ = sibling;
            nodes_[sibling].parent = k_null_node;
        } else {
            if (nodes_[grandparent].child1 == parent) nodes_[grandparent].child1 = sibling;
            else nodes_[grandparent].child2 = sibling;
            nodes_[sibling].parent = grandparent;
            refit_ancestors_(grandparent);
        }
        free_node_(parent);
    }

    void refit_ancestors_(int32_t idx) {
        while (idx != k_null_node) {
            int32_t c1 = nodes_[idx].child1;
            int32_t c2 = nodes_[idx].child2;
            nodes_[idx].aabb = geometry::AABB::merge(nodes_[c1].aabb, nodes_[c2].aabb);
            nodes_[idx].height = 1 + std::max(nodes_[c1].height, nodes_[c2].height);
            idx = nodes_[idx].parent;
        }
    }

    float margin_;
    std::vector<Node> nodes_;
    int32_t root_ = k_null_node;
    int32_t free_list_ = k_null_node;
};

} // namespace broadphase
} // namespace physx
} // namespace coopa

#endif // PHYSXCOOPA_BROADPHASE_AABB_TREE_H
