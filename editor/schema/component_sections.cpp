/**
 * @file component_sections.cpp
 * @brief Each component's inspector LAYOUT: which of its keys sit in which collapsible
 *        section, in what order, under what heading, and when.
 *
 * Kept apart from component_schema.cpp, which is already a thousand lines of key tables, so
 * that a layout edit reads on its own and recompiles on its own. The keys themselves, their
 * widgets, ranges and defaults all still live there -- a section only names keys that file
 * already describes, and a name that matches nothing is caught by the editor's schema tests.
 *
 * A component with no entry here is drawn as the flat list of its fields, which is what every
 * component looked like before sections existed; small components are fine that way. Keys no
 * section lists are drawn in a trailing "More" block, so adding a key to the schema can never
 * make it vanish from the inspector.
 */

#include "editor/schema/component_schema.h"

namespace toy {
namespace editor {

void apply_component_sections(std::map<std::string, ComponentSchema>& table) {
    // Gives `type` its sections, if the schema table has that type at all (a project build may
    // not register every one).
    auto layout = [&](const std::string& type, std::vector<InspectorSection> sections) {
        auto it = table.find(type);
        if (it == table.end()) return;
        it->second.sections = std::move(sections);
    };
    layout("MeshRenderer", {
        sec("Mesh", "Mesh").says("The mesh asset this object draws.").keys({"mesh_path"}).done(),
        sec("Material", "Material")
            .says("How its surface looks: an inline material, a shared asset, or an asset with a few values changed.")
            .keys({"material"}).done(),
        sec("Level of Detail", "Zoom").toggle("lods_enabled")
            .says("Swap in the mesh's simpler LOD copies as it shrinks on screen.")
            .keys({"lod_bias"}).closed().done(),
        sec("Tessellation", "Proportional").toggle("tessellation")
            .says("Subdivide the mesh on the GPU near the camera, so the material's displacement map and surface "
                  "shader have vertices to move.")
            .keys({"tess_edge_pixels", "tess_max_factor", "tess_max_distance"}).closed().done(),
        sec("Rendering", "Render").keys({"affects_reflection_probes"}).closed().done(),
    });

    layout("Camera", {
        sec("Lens", "Camera").says("What this camera sees.")
            .keys({"main", "projection"})
            .sub("Perspective", "projection", {"Perspective"}).keys({"fov"})
            .sub("Orthographic", "projection", {"Orthographic"}).keys({"orthographic_size"})
            .done(),
        sec("Clipping", "Ortho")
            .says("Nothing nearer than the near plane or further than the far plane is drawn. A near plane close to "
                  "zero costs depth precision everywhere.")
            .keys({"near_clip_plane", "far_clip_plane"}).done(),
        sec("Depth of Field", "Zoom")
            .says("What this camera focuses on. Each value falls back to the render settings' when it is 0.")
            .keys({"lens", "aperture", "focus_distance", "focus_object"}).closed().done(),
        sec("Post", "Render").keys({"motion_blur"}).closed().done(),
    });

    layout("DirectionalLight", {
        sec("Light", "Sun").says("The sun: parallel light from one direction, over the whole scene.")
            .keys({"direction", "color", "intensity"}).done(),
        sec("Shadows", "ShadeSolid").toggle("cast_shadows")
            .says("How dark this light's shadows go. The render settings decide their resolution and softness.")
            .keys({"shadow_intensity"}).done(),
    });

    layout("PointLight", {
        sec("Light", "PointLight").says("A bulb: light in every direction, fading out by Range.")
            .keys({"color", "intensity", "range", "cast_shadows"}).done(),
        sec("Falloff", "Zoom").says("How sharply the light dies away inside its range.")
            .keys({"attenuation_constant"}).closed().done(),
    });

    layout("SpotLight", {
        sec("Light", "SpotLight")
            .says("A cone of light: full brightness inside the inner angle, fading to nothing at the outer one.")
            .keys({"color", "intensity", "direction", "range", "cast_shadows"})
            .sub("Cone").keys({"inner_angle", "outer_angle"})
            .done(),
        sec("Falloff", "Zoom").keys({"attenuation_constant"}).closed().done(),
    });

    layout("Rigidbody", {
        sec("Body", "Rigidbody").says("Mass, and whether the physics engine moves this object or you do.")
            .keys({"mass", "center_of_mass", "use_gravity", "is_kinematic"}).done(),
        sec("Damping", "Physics").says("How quickly it loses speed on its own -- air resistance, not surface friction.")
            .keys({"drag", "angular_drag", "wind_influence"}).done(),
        sec("Motion", "Play").keys({"interpolation"}).closed().done(),
    });

    // --- Physics -------------------------------------------------------------------------

    layout("Vehicle", {
        sec("Setup", "Gear")
            .says("A preset is a whole tuning; every row below overrides one of its values.")
            .keys({"preset", "player_input", "active"}).done(),
        sec("Steering", "Rotate")
            .says("How far the wheels turn, and how fast they get there.")
            .keys({"steer_mode", "max_steer_deg"})
            .sub("At speed").keys({"min_steer_deg", "steer_falloff_speed"})
            .sub("Rates").keys({"steer_rate_deg", "steer_return_rate_deg"})
            .done(),
        sec("Brakes", "Stop").keys({"brake_torque", "handbrake_torque"}).done(),
        sec("Grip & Roll", "Collider")
            .says("How the chassis leans in a corner and how much grip each tyre has to give.")
            .keys({"anti_roll", "nominal_wheel_load"}).done(),
        sec("Aerodynamics", "World").keys({"drag", "downforce"}).closed().done(),
        sec("In the Air", "Sun")
            .says("What happens once every wheel has left the ground.")
            .keys({"air_righting", "air_damping"}).closed().done(),
        sec("Recovery", "Restart")
            .says("Turning a flipped vehicle back onto its wheels.")
            .keys({"recover_time", "recover_lift", "auto_recover_time"}).closed().done(),
        sec("Wheels", "Rigidbody")
            .says("One entry per wheel: where it sits, how big it is, and what the suspension under it does.")
            .keys({"wheels"}).done(),
    });

    layout("Ragdoll", {
        sec("Setup", "Collider")
            .says("Whether the bones follow the animation or hang limp, and how the bodies are made.")
            .keys({"mode", "auto_generate", "link_character", "reposition_root"}).done(),
        sec("Bodies", "Rigidbody")
            .says("The physics bodies built for the bones.")
            .keys({"mass", "drag", "angular_drag", "layer", "collide_connected"}).done(),
        sec("Going Limp", "Physics").keys({"start_impulse", "blend_time"}).done(),
        sec("Getting Up", "Restart")
            .says("A ragdoll comes round once it has stopped moving for long enough.")
            .keys({"recover_state", "rest_time", "rest_travel", "max_limp_speed", "max_limp_spin"}).closed().done(),
        sec("Input", "Keyboard").keys({"input_toggle", "toggle_impulse"}).closed().done(),
        sec("Bones", "Mesh")
            .says("One entry per bone: its shape, its mass and the joint holding it to its parent.")
            .keys({"bones"}).done(),
    });

    layout("HingeJoint", {
        sec("Joint", "Collider").says("A door hinge: one axis of rotation.")
            .keys({"connected_object", "anchor", "axis", "enable_collision"}).done(),
        sec("Limits", "Rotate").toggle("use_limits").keys({"min_angle", "max_angle"}).done(),
    });

    layout("ConeTwistJoint", {
        sec("Joint", "Collider").says("A shoulder: swing in a cone, twist about the axis.")
            .keys({"connected_object", "anchor", "axis", "enable_collision"}).done(),
        sec("Limits", "Rotate").keys({"swing_limit", "twist_min", "twist_max"}).done(),
    });

    // --- Water ---------------------------------------------------------------------------

    layout("WaterBody", {
        sec("Shape", "Mesh")
            .says("A still body (lake, pond, pool) or water running downhill (river, stream).")
            .keys({"mode", "mesh_path", "size", "resolution", "tile_size"}).done(),
        sec("Waves", "World")
            .says("The surface's own motion. The weather's wind reshapes it by Wind Response.")
            .keys({"wave_amplitude", "wave_length", "wave_direction", "wave_steepness", "wind_response"}).done(),
        sec("Flow", "ArrowRight").when("mode", {"flowing"})
            .says("Which way the water runs, and how fast; the bed's slope sets the rest.")
            .keys({"flow_speed", "flow_min_speed", "flow_slope_gain"}).done(),
        sec("Foam & Shore", "Snap")
            .says("White water where the surface meets the land.")
            .keys({"foam_color", "foam_amount", "shore_foam_depth", "edge_fade_depth"}).done(),
        sec("Ripples", "Proportional")
            .says("Rings and wakes left by things moving through the water.")
            .keys({"ripple_strength", "ripple_scale", "obstacle_radius", "wake_length"}).closed().done(),
        sec("Underwater", "EnvLight")
            .says("What it looks like from below, and how far light gets down.")
            .keys({"clarity", "underwater_color", "underwater_visibility", "underwater_absorption", "caustics"}).closed().done(),
        sec("Buoyancy", "Physics")
            .says("What floating objects feel. Density is the water's, in kg/m3.")
            .keys({"density", "max_depth"}).closed().done(),
    });

    layout("Buoyancy", {
        sec("Flotation", "Physics")
            .says("How much of this object is under water, and how hard the water pushes back.")
            .keys({"volume", "buoyancy_scale", "subdivisions"}).done(),
        sec("Drag", "World").keys({"linear_drag", "angular_drag", "form_drag"}).closed().done(),
    });

    // --- World ---------------------------------------------------------------------------

    // The map a terrain is generated from (toyengine/mapgen). Target first, because what this
    // component does is APPLY -- the settings are secondary to which terrain they land on.
    layout("MapGenerator", {
        sec("Apply", "World")
            .says("A generated world is written into a terrain on demand, the same way a heightmap "
                  "image is. Once applied the terrain owns its data, and this keeps the map behind "
                  "it for biomes, regions and rivers.")
            .keys({"target", "auto_apply"}).done(),
        sec("Detail", "Zoom")
            .says("How finely the map is written when it is applied to a tiled terrain. A smooth "
                  "terrain uses its own resolution instead.")
            .keys({"world_tiles", "tiles_per_grid_unit", "height_scale"}).done(),
        sec("Water", "World")
            .says("How far the sea, lakes and rivers sit under the terrace they fill. Tiled terrain "
                  "only -- a smooth shoreline is wherever the ground crosses the water's level.")
            .keys({"water_inset", "river_inset"}).closed().done(),
        sec("Planet", "World").keys({"sphere"}).closed().done(),
    });

    layout("HeightmapTerrain", {
        // A live status line and the way into Terrain Mode, drawn by the terrain panel: the
        // inspector is where a user looks first, and sculpting is the thing they came to do.
        sec("Terrain", "Terrain").custom("HeightmapTerrain.edit").done(),
        sec("Size", "Terrain")
            .says("How big the landscape is and how finely it is sampled. Resolution is 2^n + 1.")
            .keys({"size", "height", "chunk_size", "resolution"}).done(),
        sec("Maps", "Image")
            .says("The 16-bit height map, and the splat maps that say which layer paints where.")
            .keys({"heightmap", "splat0", "splat1", "splat_resolution", "layer_texture_size"}).done(),
        sec("Layers", "Palette")
            .says("Up to eight painted surfaces. Each has its own textures, tiling and roughness.")
            .keys({"layers"}).done(),
        sec("Weather", "World")
            .says("Water is a surface over this ground too: each tile carries a kind and a level, and "
                  "the shoreline is wherever the ground crosses it. Paint it in Terrain Mode, or apply "
                  "a generated world that brings its own sea, lakes and rivers.")
            .keys({"deep_snow", "puddles"}).closed().done(),
        sec("Collision", "Collider").toggle("collision").keys({"physics_material"}).closed().done(),
        sec("Streaming", "Zoom")
            .says("How much terrain exists around the camera, and how much of it casts shadows.")
            .keys({"cast_shadows", "view_distance", "shadow_distance", "patch_resolution"}).closed().done(),
    });

    // --- Gameplay ------------------------------------------------------------------------

    layout("CameraController", {
        sec("Mode", "Camera")
            .says("Orbit circles a target, First Person rides it, Fly goes where you steer.")
            .keys({"mode", "tracker", "target"}).done(),
        sec("Framing", "Zoom").when("mode", {"Orbit"})
            .says("How far out the camera sits and how closely it chases what it tracks.")
            .keys({"distance", "min_distance", "max_distance", "follow_smoothing"}).done(),
        sec("Eye", "Eye").when("mode", {"FirstPerson"})
            .keys({"eye_height", "first_person_pitch_limit"}).done(),
        sec("Angles", "Orientation")
            .says("Where the camera looks, and how far up and down it may tilt.")
            .keys({"yaw_deg", "pitch_deg", "min_pitch_deg", "max_pitch_deg"}).done(),
        sec("Input", "MouseLeft")
            .keys({"look_button", "mouse_sensitivity", "zoom_speed", "invert_x", "invert_y", "capture_cursor"}).done(),
        sec("Motion", "Move")
            .keys({"move_speed", "movement_smoothing", "auto_rotate_deg_per_sec", "key_orbit_speed_deg_per_sec"}).closed().done(),
        sec("Collision", "Collider").toggle("collide")
            .says("Pull the camera in when the world gets between it and what it is watching.")
            .keys({"collision_radius", "collision_in_speed", "collision_out_speed"}).closed().done(),
    });

    layout("CharacterController", {
        sec("Capsule", "Collider")
            .says("The shape that collides with the world; the origin is at the feet.")
            .keys({"radius", "height", "skin"}).done(),
        sec("Movement", "Move")
            .keys({"move_speed", "sprint_multiplier", "acceleration", "turn_speed", "face_movement"}).done(),
        sec("Jump & Air", "ArrowDown")
            .says("Coyote time and the jump buffer are the two forgiveness windows that make a jump feel fair.")
            .keys({"jump_height", "gravity_scale", "air_control", "coyote_time", "jump_buffer"}).done(),
        sec("Ground", "Terrain")
            .says("What counts as walkable, and how the character sticks to it over steps and ramps.")
            .keys({"step_height", "snap_distance", "slope_limit", "step_smoothing"}).done(),
        sec("Pushing", "Rigidbody").toggle("push_dynamic_bodies").keys({"push_strength"}).closed().done(),
        sec("Driven By", "Play").keys({"player_input", "use_root_motion"}).closed().done(),
    });

    layout("Mount", {
        sec("Gaits", "Move")
            .says("The three speeds the mount moves at, and how quickly it reaches them.")
            .keys({"walk_speed", "trot_speed", "gallop_speed", "mount_max_speed"})
            .sub("Change of pace").keys({"acceleration", "braking"})
            .done(),
        sec("Turning", "Rotate").keys({"turn_rate", "gallop_turn_rate"}).done(),
        sec("Animation", "Play")
            .says("Which Animator state each gait plays, and how the clip's speed follows the real one.")
            .keys({"idle_state", "walk_state", "trot_state", "gallop_state", "crossfade"})
            .sub("Clip speed").keys({"min_anim_speed", "max_anim_speed"})
            .done(),
        sec("Rig Points", "Empty")
            .says("Objects on the mount the rider's hands, feet and seat are placed against.")
            .keys({"saddle", "stirrup_l", "stirrup_r", "rein_l", "rein_r", "mount_point_l", "mount_point_r"}).closed().done(),
        sec("Camera", "Camera").keys({"camera_offset_z", "camera_distance"}).closed().done(),
    });

    layout("Rider", {
        sec("Mounting", "Link")
            .says("How close the rider must be, and how long getting on and off takes.")
            .keys({"mount_radius", "mount_time", "dismount_time", "mount_on_start"}).done(),
        sec("Pose", "Empty")
            .says("How the rider sits once they are up.")
            .keys({"ride_state", "crossfade", "seat_offset", "knee_spread", "lean"}).done(),
        sec("IK Weights", "Proportional")
            .says("How strongly the hands and feet are pulled onto the reins and stirrups.")
            .keys({"leg_weight", "arm_weight", "priority"}).closed().done(),
    });

    layout("Destructible", {
        sec("Fracture", "Knife")
            .says("How the mesh is cut up. The same mesh, piece count and seed always break the same way.")
            .keys({"pieces", "seed", "irregularity", "prefracture"}).done(),
        sec("Breaking", "Physics")
            .says("What it takes to break, and what the pieces do once it does.")
            .keys({"impact_impulse", "impact_transfer", "scatter"}).done(),
        sec("Chunks", "Cube")
            .says("How long the debris lives and how much of it there may be.")
            .keys({"chunk_lifetime", "chunk_fade", "chunk_density", "max_chunks", "chunk_physics_material"}).closed().done(),
        sec("Chunk Collision", "Collider")
            .keys({"collider_fill", "max_boxes"}).closed().done(),
    });

    layout("KinematicMover", {
        sec("Motion", "Move").says("A scripted path with no physics: a platform, a lift, a spinning hazard.")
            .keys({"mode", "speed"}).done(),
        sec("Path", "ArrowRight").when("mode", {"PingPong"}).keys({"axis", "distance"}).done(),
        sec("Orbit", "Rotate").when("mode", {"Orbit"}).keys({"orbit_center", "orbit_radius"}).done(),
        sec("Spin", "Rotate").when("mode", {"Spin"}).keys({"spin_axis", "spin_speed"}).done(),
    });

    layout("Turntable", {
        sec("Drag", "MouseLeft").says("Spin the object by dragging it, the way a product viewer does.")
            .keys({"drag_button", "sensitivity", "axis", "invert_x", "invert_y"}).done(),
        sec("Spin", "Rotate").keys({"idle_spin_deg_per_sec", "damping", "max_spin_deg_per_sec"}).done(),
        sec("Tilt Limits", "Orientation").keys({"min_tilt_deg", "max_tilt_deg"}).closed().done(),
    });

    layout("SceneLink", {
        sec("Destination", "Scene")
            .says("Walking into the trigger box loads another scene.")
            .keys({"target_scene", "spawn_point", "size"}).done(),
        sec("Transition", "Restart")
            .keys({"transition"})
            .sub("Fade", "transition", {"fade"}).keys({"color", "fade_time"})
            .sub("Loading screen", "transition", {"loading_screen"}).keys({"loading_screen", "min_display_time"})
            .done(),
    });

    layout("CharacterAnimDriver", {
        sec("States", "Play")
            .says("Which Animator state plays for each thing the character is doing.")
            .keys({"idle_state", "walk_state", "run_state", "jump_state"}).done(),
        sec("Thresholds", "Move")
            .says("The ground speeds the states change at.")
            .keys({"walk_speed", "run_speed", "crossfade", "air_delay"}).done(),
    });

    // --- Audio ---------------------------------------------------------------------------

    layout("AudioSource", {
        sec("Sound", "Play").keys({"clip", "bus", "volume", "pitch"}).done(),
        sec("Playback", "Step").keys({"loop", "play_on_start", "priority"}).done(),
        sec("3D", "World").toggle("spatialize")
            .says("Positioned in the world: it gets quieter with distance and comes from where the object is.")
            .keys({"min_distance", "max_distance", "curve"}).done(),
    });

    // --- Animation -----------------------------------------------------------------------

    layout("Animator", {
        sec("Clips", "Play")
            .says("The rig's states. A clip path is shared: editing the clip changes every rig that names it.")
            .keys({"states", "auto_play"}).done(),
        sec("Playback", "Step").keys({"speed", "default_crossfade", "apply_root_motion"}).done(),
    });

    layout("TwoBoneIK", {
        sec("Chain", "Mesh").says("Two bones and the joint between them: an arm, or a leg.")
            .keys({"upper", "lower", "end"}).done(),
        sec("Goal", "Cursor").keys({"target", "pole"}).done(),
        sec("Blending", "Proportional").keys({"weight", "soft_limit", "match_target_rotation", "priority"}).done(),
    });

    layout("ChainIK", {
        sec("Chain", "Mesh").says("Any number of bones from root to tip: a tail, a tentacle, a rope.")
            .keys({"root", "tip"}).done(),
        sec("Goal", "Cursor").keys({"target"}).done(),
        sec("Solver", "Gear").keys({"iterations", "tolerance"}).closed().done(),
        sec("Blending", "Proportional").keys({"weight", "match_target_rotation", "priority"}).done(),
    });

    layout("LookAtIK", {
        sec("Bone", "Mesh").keys({"bone", "forward_axis", "up_axis"}).done(),
        sec("Goal", "Eye").keys({"target", "max_angle"}).done(),
        sec("Blending", "Proportional").keys({"weight", "smoothing", "priority"}).done(),
    });

    layout("FootIK", {
        sec("Rig", "Mesh").says("The pelvis and the two feet this places on the ground.")
            .keys({"pelvis", "left", "right", "mode"}).done(),
        sec("Ground Probe", "Terrain")
            .says("How far above and below each foot the ground is looked for.")
            .keys({"ray_up", "ray_down", "max_pelvis_drop"}).done(),
        sec("Foot Angle", "Rotate").toggle("align_feet").keys({"max_foot_angle"}).done(),
        sec("Body Pitch", "Orientation").toggle("body_pitch")
            .says("A quadruped's pelvis rises, drops and pitches between its front and hind legs.")
            .keys({"max_body_pitch"}).closed().done(),
        sec("Blending", "Proportional").keys({"weight", "blend_speed"}).done(),
    });

    // --- Environment -----------------------------------------------------------------------

    layout("EnvInfluence", {
        sec("Shape", "Sphere")
            .says("Where the influence reaches. It sits on the object, so moving the object moves it.")
            .keys({"shape", "radius", "height", "falloff"}).done(),
        sec("Wind", "World")
            .says("What the air does inside. A negative Vertical is a downwash: it presses foliage "
                  "flat as well as fanning it outward.")
            .keys({"mode", "wind", "outward", "vertical", "turbulence"}).done(),
        sec("Surface", "Material")
            .says("Ground and air state, independent of the wind.")
            .keys({"wetness", "temperature"}).closed().done(),
        sec("Ground Effects", "Terrain")
            .says("What it does to lying snow, and to water beneath it.")
            .keys({"snow_clear", "water_disturb"}).closed().done(),
        sec("Blending", "Proportional")
            .says("How it combines with the global weather and with other influences.")
            .keys({"blend", "strength", "enabled"}).done(),
    });

    layout("EnvPulseEmitter", {
        sec("Trigger", "Play")
            .says("What fires the gust, and where it is born.")
            .keys({"event", "offset", "strength"}).done(),
        sec("Gust", "World")
            .says("The shape and strength of the one-shot influence.")
            .keys({"radius", "falloff", "mode", "wind", "outward", "vertical", "turbulence"}).done(),
        sec("Envelope", "Step")
            .says("How it rises and fades. The decay is what makes foliage snap and then recover.")
            .keys({"attack", "hold", "decay"}).done(),
    });
}

} // namespace editor
} // namespace toy
