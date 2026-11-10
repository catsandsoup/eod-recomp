# Modding Suite Architectural Foundations & Interchange Specifications
## Test Drive: Eve of Destruction (`SLUS_209.10`) Recompilation

**Document ID:** `EOD-MOD-001`  
**Classification:** Modding Suite Architectural Specification (Milestone M6 / Requirement R3)  
**Author:** Central Integration Agent (`teamwork_worker_central_integrator`)  
**Date:** 2026-11-10  
**Status:** Approved Technical Foundation  

---

## 1. Scope, Purpose & Non-Goals

### 1.1 Purpose
This specification establishes the concrete schemas, data structures, object definitions, and interchange formats required to power a future native PC/Mac Modding Suite for *Test Drive: Eve of Destruction*.

### 1.2 Non-Goals
In strict accordance with Requirement R3 and the project mandate:
- **No GUI modding tools are implemented in this phase**. Authoring GUI desktop editors, node graph viewers, or visual 3D inspectors is explicitly an end-state goal deferred until after complete system reverse-engineering.
- **No legacy PS2 hacks**: The specifications defined herein bypass legacy 32 MB physical memory ceilings, 4 MB GS VRAM swizzling, 20-car participant caps, and VIF DMA packetization in favor of standard, extensible formats (glTF 2.0, JSON/YAML, Jolt Physics, and standard audio codecs).

---

## 2. Vehicle Systems Specification

### 2.1 Declarative Vehicle Schema (`vehicle.manifest.json`)
The following JSON schema defines custom vehicle packages:

```json
{
  "$schema": "https://eod-recomp.org/schemas/v1/vehicle.manifest.json",
  "identifier": "mod.chassis.phantom_gt",
  "display_name": "Phantom GT",
  "vehicle_class": "midsize",
  "manufacturer": "Custom Works",
  "year": 1974,
  "career": {
    "price_retail": 4200,
    "resale_factor": 0.90,
    "parts_tradein_factor": 0.75,
    "unlock_rank": 49,
    "dealer_stock_probability": 0.35,
    "starting_condition": 0.85
  },
  "geometry": {
    "source_model": "models/phantom.gltf",
    "lod_chain": [
      { "lod": 0, "max_distance_meters": 10.0, "cockpit_interior": true },
      { "lod": 1, "max_distance_meters": 25.0, "cockpit_interior": false },
      { "lod": 2, "max_distance_meters": 50.0, "cockpit_interior": false },
      { "lod": 3, "max_distance_meters": 100.0, "cockpit_interior": false }
    ],
    "shadow_proxy": "models/phantom_shadow.gltf"
  },
  "physics": {
    "chassis": {
      "mass_kg": 1360.77,
      "wheelbase_meters": 2.74,
      "track_front_meters": 1.57,
      "track_rear_meters": 1.57,
      "center_of_mass": [0.0, 0.45, 0.12],
      "moment_of_inertia": [2800.0, 3100.0, 750.0],
      "aerodynamic_drag": 0.32,
      "frontal_area_m2": 1.95,
      "chassis_health_max": 1.0
    },
    "engine": {
      "displacement_liters": 5.7,
      "peak_power_bhp": 385.0,
      "peak_power_rpm": 5800,
      "peak_torque_nm": 520.0,
      "peak_torque_rpm": 3600,
      "idle_rpm": 850,
      "redline_rpm": 6500,
      "soundbank": "audio/v8_heavy.json"
    },
    "tires": {
      "tire_model": "pacejka_magic_formula",
      "magic_b": 0.714,
      "magic_c": 1.400,
      "magic_d": 1.000,
      "magic_e": -0.200,
      "nominal_friction": 0.92,
      "removal_impulse_threshold_ns": 45000.0
    }
  },
  "sockets": [
    { "name": "wheel_fl", "type": "wheel_socket", "position": [-0.785, 0.35, 1.37], "rotation": [0,0,0,1] },
    { "name": "wheel_fr", "type": "wheel_socket", "position": [0.785, 0.35, 1.37], "rotation": [0,0,0,1] },
    { "name": "wheel_rl", "type": "wheel_socket", "position": [-0.785, 0.35, -1.37], "rotation": [0,0,0,1] },
    { "name": "wheel_rr", "type": "wheel_socket", "position": [0.785, 0.35, -1.37], "rotation": [0,0,0,1] },
    { "name": "hinge_hood", "type": "panel_hinge", "position": [0.0, 0.85, 1.15], "part_group": 2, "detachment_threshold_meters": 0.127 },
    { "name": "hinge_door_l", "type": "panel_hinge", "position": [-0.85, 0.65, 0.20], "part_group": 6, "detachment_threshold_meters": 0.0762 },
    { "name": "hinge_door_r", "type": "panel_hinge", "position": [0.85, 0.65, 0.20], "part_group": 5, "detachment_threshold_meters": 0.0762 },
    { "name": "hinge_decklid", "type": "panel_hinge", "position": [0.0, 0.92, -1.85], "part_group": 11, "detachment_threshold_meters": 0.0381 },
    { "name": "mount_bumper_f", "type": "panel_hinge", "position": [0.0, 0.42, 2.25], "part_group": 12, "detachment_threshold_meters": 0.0762 },
    { "name": "mount_bumper_r", "type": "panel_hinge", "position": [0.0, 0.45, -2.30], "part_group": 13, "detachment_threshold_meters": 0.0762 },
    { "name": "hitch_trailer", "type": "trailer_hitch", "position": [0.0, 0.38, -2.45], "rotation": [0,0,0,1] },
    { "name": "weapon_roof", "type": "weapon_hardpoint", "position": [0.0, 1.42, 0.00], "rotation": [0,0,0,1] },
    { "name": "driver_seat", "type": "interior_anchor", "position": [-0.38, 0.52, -0.15], "rotation": [0,0,0,1] }
  ]
}
```

### 2.2 Component Hierarchy & Damage Group Layout
Every custom vehicle model decomposes into 32 standardized component groups compatible with native rendering and debris detachment:

```
Group ID   Semantic Part Name         Paintable   Detachable   Hinge / Anchor Socket Name
────────   ──────────────────         ─────────   ──────────   ──────────────────────────
0          Chassis Subframe / Unibody   No          No           Chassis Center of Mass
1          Cockpit Interior / Rollcage  No          No           "driver_seat"
2          Engine Hood                  Yes         Yes          "hinge_hood" (Group 2)
3          Right Front Fender           Yes         Yes          "hinge_door_r" + 0.61m Z
4          Left Front Fender            Yes         Yes          "hinge_door_l" + 0.61m Z
5          Right Passenger Door         Yes         Yes          "hinge_door_r" (Group 5)
6          Left Driver Door             Yes         Yes          "hinge_door_l" (Group 6)
7          Right Rear Quarter Panel     Yes         No           Chassis Rear Right
8          Left Rear Quarter Panel      Yes         No           Chassis Rear Left
9          Roof Panel                   Yes         No           Chassis Roof
10         Engine Block & Radiator      No          No           "engine_block"
11         Trunk Decklid                Yes         Yes          "hinge_decklid" (Group 11)
12         Front Bumper                 Yes         Yes          "mount_bumper_f" (Group 12)
13         Rear Bumper                  Yes         Yes          "mount_bumper_r" (Group 13)
14         Front Windshield Glass       No          Shatter      A-Pillar Perimeter
15         Rear Window Glass            No          Shatter      C-Pillar Perimeter
16         Left Side Window Glass       No          Shatter      Door Frame Left
17         Right Side Window Glass      No          Shatter      Door Frame Right
18         Headlight Assembly           No          Shatter      Front Fascia
19         Taillight Assembly           No          Shatter      Rear Fascia
20         Exhaust System               No          Yes          "exhaust_emitter"
21..31     Custom Auxiliary Props       Optional    Optional     User Sockets
```

### 2.3 Skeletal Mesh Crumple & Deformation Specification
Deformation is defined using an array of deformation bones mapped to vertex skinning weights:
- **Bone Count**: Up to 160 bones (preserving parity with retail `Bone.mvl`'s 157 bones).
- **Influence**: 2 bones per vertex ($w_a + w_b = 256$, normalized to $1.0$).
- **Displacement**: Vertex displacement is calculated in world-space coordinates:
  $$\Delta \vec{P} = \frac{w_a}{256}\vec{D}_a + \frac{w_b}{256}\vec{D}_b$$
  where $\vec{D}_i$ is the accumulated impulse displacement vector stored on bone $i$.

---

## 3. Environment & Track Systems Specification

### 3.1 Declarative Track Schema (`track.manifest.json`)
The following schema defines custom circuit and arena venues:

```json
{
  "$schema": "https://eod-recomp.org/schemas/v1/track.manifest.json",
  "identifier": "mod.track.thunder_valley",
  "display_name": "Thunder Valley Speedway",
  "track_type": "circuit",
  "surface_type": "dirt_clay",
  "length_meters": 1250.0,
  "visual": {
    "scene_model": "geometry/track_scene.gltf",
    "skybox": "textures/skybox_sunset.hdr",
    "ambient_light": [0.25, 0.22, 0.20],
    "sun_direction": [-0.577, -0.577, -0.577],
    "sun_color": [1.0, 0.95, 0.85]
  },
  "collision": {
    "terrain_mesh": "geometry/terrain_collision.gltf",
    "barrier_mesh": "geometry/barrier_collision.gltf",
    "static_mesh_shape": "jolt_mesh_shape"
  },
  "ai_navigation": {
    "primary_spline": "navigation/centerline.spline.json",
    "passing_corridors": "navigation/passing_lanes.json",
    "pit_branches": "navigation/pit_road.json"
  },
  "scenery_props": "scenery/placed_props.json",
  "supported_events": [
    "Race", "Figure8", "Derby", "TrailerRace", "Battle", "ChickenBattle"
  ]
}
```

### 3.2 Track Scenery Prop Properties
Scenery props instantiated via `placed_props.json` adhere to standard physical behaviors:

```json
{
  "instances": [
    {
      "identifier": "prop_tractor_tire_stack_01",
      "model": "models/tire_stack.gltf",
      "position": [124.5, 0.0, -45.2],
      "rotation": [0.0, 0.707, 0.0, 0.707],
      "physics_archetype": "composite_cushion",
      "properties": {
        "mass_kg": 750.0,
        "friction": 0.65,
        "restitution": 0.25,
        "is_immovable": false,
        "is_indestructible": false,
        "breakaway_force_threshold_n": 15000.0,
        "maximum_height_limit_meters": 1.50
      }
    },
    {
      "identifier": "prop_concrete_retaining_wall_04",
      "model": "models/barrier_wall.gltf",
      "position": [150.0, 0.0, -60.0],
      "rotation": [0.0, 0.0, 0.0, 1.0],
      "physics_archetype": "static_boundary",
      "properties": {
        "mass_kg": 50000.0,
        "friction": 0.20,
        "restitution": 0.10,
        "is_immovable": true,
        "is_indestructible": true,
        "maximum_height_limit_meters": 2.20
      }
    }
  ]
}
```

### 3.3 AI Navigation Spline Format (`centerline.spline.json`)
AI centerline curves replace the binary `ILIN` v27 stream with JSON waypoint chains:

```json
{
  "spline_id": "main_racing_line",
  "is_closed_loop": true,
  "node_count": 3,
  "nodes": [
    {
      "index": 0,
      "position": [0.0, 0.0, 0.0],
      "tangent": [1.0, 0.0, 0.0],
      "recommended_speed_mps": 38.5,
      "track_width_left_meters": 8.0,
      "track_width_right_meters": 8.0,
      "banking_angle_degrees": 12.0,
      "surface_material_id": 6,
      "branch_next_indices": [1]
    },
    {
      "index": 1,
      "position": [150.0, 0.0, 20.0],
      "tangent": [0.95, 0.0, 0.31],
      "recommended_speed_mps": 32.0,
      "track_width_left_meters": 7.5,
      "track_width_right_meters": 7.5,
      "banking_angle_degrees": 15.0,
      "surface_material_id": 6,
      "branch_next_indices": [2]
    },
    {
      "index": 2,
      "position": [280.0, 0.0, 110.0],
      "tangent": [0.0, 0.0, 1.0],
      "recommended_speed_mps": 24.0,
      "track_width_left_meters": 9.0,
      "track_width_right_meters": 9.0,
      "banking_angle_degrees": 18.0,
      "surface_material_id": 6,
      "branch_next_indices": [0]
    }
  ]
}
```

---

## 4. Gameplay Objects & Projectiles Specification

### 4.1 Declarative Projectile Schema (`projectile.manifest.json`)
Supports creating new weapons or modifying the explosive chicken:

```json
{
  "$schema": "https://eod-recomp.org/schemas/v1/projectile.manifest.json",
  "identifier": "mod.weapon.explosive_chicken",
  "display_name": "Explosive Chicken Launcher",
  "geometry": {
    "flight_model": "models/chicken_flying.gltf",
    "impact_model": "models/chicken_detonation.gltf",
    "flight_animation": "animations/chicken_flail.gltf"
  },
  "kinematics": {
    "launch_regimes": {
      "straight": {
        "muzzle_velocity_offset_mps": 28.0,
        "inherit_vehicle_velocity": true
      },
      "mortar": {
        "elevation_angle_degrees": 35.0,
        "base_speed_mps": 24.89,
        "inherit_vehicle_velocity": true
      },
      "raining": {
        "spawn_height_meters": 45.0,
        "downward_speed_mps": 19.62
      }
    },
    "mass_kg": 2.5,
    "aerodynamic_drag": 0.15,
    "gravity_mps2": -9.81
  },
  "reload_logic": {
    "cooldown_seconds": 3.0,
    "require_trigger_release": true,
    "max_active_in_world": 16
  },
  "detonation": {
    "blast_radius_meters": 7.5,
    "peak_impulse_force_n": 25000.0,
    "damage_falloff": "linear",
    "visual_effect": {
      "sprite_sheet": "textures/explosion_atlas.png",
      "frame_count": 14,
      "duration_seconds": 0.35
    },
    "audio": {
      "sound_launch": "audio/chkstart.wav",
      "sound_flight": "audio/chkroll.wav",
      "sound_explode": "audio/explode.wav"
    }
  }
}
```

---

## 5. Event Scripting & Rules Engine

### 5.1 Event Definition Schema (`event.manifest.json`)
Allows modders to define custom race and combat game modes:

```json
{
  "$schema": "https://eod-recomp.org/schemas/v1/event.manifest.json",
  "identifier": "mod.event.demolition_endurance",
  "display_name": "Demolition Endurance",
  "category": "arena_combat",
  "category_bitmask": "0x18000000",
  "rules": {
    "scoring_metric": "net_kills",
    "target_score": 10,
    "time_limit_seconds": 300.0,
    "respawn": {
      "enabled": true,
      "delay_seconds": 2.0,
      "repair_chassis_on_respawn": true
    },
    "suicide_penalty_points": 1,
    "disqualification_on_timer_expiry": false
  },
  "restrictions": {
    "allowed_vehicle_classes": ["midsize", "muscle"],
    "weapons_enabled": true,
    "trailers_permitted": false,
    "max_participants": 12
  },
  "ai_profile": {
    "aggression_bias": 1.4,
    "target_switching_interval_seconds": 0.50,
    "hysteresis_factor": 0.65,
    "ramming_handbrake_slide": true
  }
}
```

---

## 6. Decoupled UI & Native Menu Contract

### 6.1 `VirtualListModel<T>` C++ Architecture
To replace original PS2 `.lyt` widget allocations and eliminate empirical crash vulnerabilities (such as buffer smashes in `HomebaseBuildMenu` and NULL selections in `UISetMenuIndex`), all native menus interface with a virtual data model:

```cpp
template <typename ItemType>
class VirtualListModel {
public:
    virtual ~VirtualListModel() = default;
    virtual size_t TotalCount() const = 0;
    virtual ItemType GetItem(size_t index) const = 0;
    virtual bool IsSelectable(size_t index) const = 0;
    virtual void OnItemSelected(size_t index, int action_code) = 0;
};
```

### 6.2 `INativeMenuHost` Interface Contract
```cpp
class INativeMenuHost {
public:
    virtual ~INativeMenuHost() = default;
    
    // Presents a modal dialog and halts until completion
    virtual int ShowModalDialog(const std::string &dialog_id,
                                const std::vector<std::string> &options,
                                int default_option_index) = 0;
    
    // Updates HUD overlay elements without GS hardware dependencies
    virtual void UpdateHudAnchor(float &left_anchor, float &right_anchor,
                                 float aspect_ratio) = 0;
};
```

---

## 7. Custom Mod Package Directory Hierarchy

A complete standalone mod package adheres to this standardized file layout:

```
mods/<mod_identifier>/
├── mod.json                     # Root mod package metadata and dependency manifest
├── vehicles/
│   ├── vehicle.manifest.json    # Physics, sockets, and hierarchy definition
│   ├── models/                  # glTF 2.0 3D meshes (LOD0..LOD3, shadow proxy)
│   ├── textures/                # RGBA PNG / BC7 GPU textures (albedo, normal, PBR)
│   └── audio/                   # 16-bit 48kHz WAV audio streams (engine, horn)
├── tracks/
│   ├── track.manifest.json      # Track scene, bounds, and surface materials
│   ├── geometry/                # glTF 2.0 visual and collision meshes
│   ├── navigation/              # JSON waypoint splines and passing corridors
│   └── scenery/                 # Dynamic prop placement descriptors
├── events/
│   └── event.manifest.json      # Custom event rules, scoring, and AI behaviors
└── ui/
    ├── screens/                 # Declarative screen layouts
    └── icons/                   # Custom car portraits, decals, and badges
```
