# physxcoopa/util (`coopa::physx::util`)

Shared math helpers, config, error reporting, and the world-space `Transform` bridge into libcoopa.

| File | Purpose |
|---|---|
| [`math.h`](math.h) | `k_epsilon`, `k_gravity_z`, `k_default_fixed_dt`, `k_max_frame_time`, `safe_normalize()`, `orthonormal_basis()` (Duff et al.'s branchless tangent-frame construction, used by the solver's friction directions). |
| [`config.h`](config.h) | `PhysicsConfig` — every tunable the solver/world reads: `velocity_iterations`, `relax_iterations`, `position_iterations`, `position_correction`, `linear_slop`, `restitution_threshold`, sleep thresholds, `aabb_margin`, `max_substeps`, `fixed_dt`, `max_linear_velocity`, `max_kinematic_speed`/`max_kinematic_spin` (past which a kinematic body's frame-to-frame move is read as a placement, not a motion), `use_exact_quaternion_integration`, `cloth_substeps`, `cloth_iterations`. |
| [`physics_settings.h`](physics_settings.h) | `PhysicsSettings` (gravity, `PhysicsConfig`, layer names, ignored layer pairs, default debug-draw flags, `parallel_threshold`) and `parse_physics_settings()` for a scene's `physics:` YAML block. |
| [`error.h`](error.h) | `throw_error(msg)` — `std::runtime_error` prefixed `[physxcoopa]`, the house convention every other file's failure path uses. |
| [`transform_bridge.h`](transform_bridge.h) | `Trs` + `world_trs()` / `set_world_trs()` — world-space position/rotation/scale read and write for `coopa::util::Transform`, built entirely from its public `parent()`/`get_world_matrix()` accessors, so `PhysicsSystem` needs nothing from libcoopa beyond `Transform`'s public API. |
