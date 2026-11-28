// decomp_chicken_weapon.h - Native C++ decompilation of SLUS_209.10's explosive
// chicken weapon subsystem (0x0017DA58..0x0017E968): radial blast shockwave,
// ballistic launch regimes (Straight / Mortar / Rain), reload cooldown state
// machine, projectile impact/detonation, weapon registry, and lifecycle helpers.
//
// Struct offsets and named constants are exposed here so mods, online netplay,
// and future authoritative MMO servers can inspect or tune weapon state cleanly
// without raw magic offsets.
#pragma once

#include <cstdint>

namespace decomp_chicken {

// Global guest addresses (SLUS_209.10)
inline constexpr uint32_t kCheatFlagsAddr      = 0x003C4190u; // bitmask of active cheats/battle modes
inline constexpr uint32_t kRaceClockAddr       = 0x003CDACCu; // float race clock (seconds)
inline constexpr uint32_t kWeaponRegistryAddr  = 0x003CE3F8u; // 16 x 12-byte rows {owner, slot, iface}
inline constexpr uint32_t kWeaponRegistryCount = 0x003CE4B8u; // int32_t active row count
inline constexpr uint32_t kSpatialHashGridPtr  = 0x0048D2B4u; // ptr to 100x100 cell table (20m pitch, [-1000,1000])

// Cheat / mode flag bits in *kCheatFlagsAddr (0x003C4190)
inline constexpr uint32_t kCheatMaster = 0x01u;
inline constexpr uint32_t kCheatWeapon = 0x08u; // Straight Battle / Chicken Fight
inline constexpr uint32_t kCheatMortar = 0x10u; // Feat Mortar launch arc
inline constexpr uint32_t kCheatRain   = 0x80u; // Rain Chickens from sky

// Weapon registry row layout (12 bytes per entry at kWeaponRegistryAddr)
inline constexpr uint32_t kRegistryRowStride = 0x0Cu;
inline constexpr uint32_t kRegistryOffOwner  = 0x00u; // int32_t participant index
inline constexpr uint32_t kRegistryOffSlot   = 0x04u; // int32_t weapon slot (0 = primary chicken)
inline constexpr uint32_t kRegistryOffIface  = 0x08u; // uint32_t interface ptr (body + kOffInterface)

// Chicken projectile / weapon body layout (0x228 bytes)
inline constexpr uint32_t kBodySize              = 0x228u;
inline constexpr uint32_t kBodyAllocTagAddr      = 0x003F9520u;
inline constexpr uint32_t kBodyInterfaceVtable   = 0x003F9528u;
inline constexpr uint32_t kBodySceneVtable       = 0x003F9538u;

inline constexpr uint32_t kOffReplaySlotOffset   = 0x004u; // int32_t byte offset in replay buffer
inline constexpr uint32_t kOffPoseMatrix         = 0x034u; // 48-byte 3x4 pose (Right/Up/Fwd/Pos)
inline constexpr uint32_t kOffWorldRight         = 0x064u; // Vec3 world right axis (post-SetPose)
inline constexpr uint32_t kOffWorldUp            = 0x070u; // Vec3 world up axis
inline constexpr uint32_t kOffWorldForward       = 0x07Cu; // Vec3 world forward axis
inline constexpr uint32_t kOffWorldPos           = 0x088u; // Vec3 world position
inline constexpr uint32_t kOffRegionId           = 0x094u; // int16_t track region / zone ID
inline constexpr uint32_t kOffSceneVtable        = 0x098u; // uint32_t vtable ptr (0x003F9538)
inline constexpr uint32_t kOffLinearVel          = 0x104u; // Vec3 linear velocity (m/s)
inline constexpr uint32_t kOffAngularVel         = 0x110u; // Vec3 angular velocity (rad/s)
inline constexpr uint32_t kOffVisibleActive      = 0x1D8u; // int32_t visible / active flag
inline constexpr uint32_t kOffInterface          = 0x1DCu; // weapon interface sub-object (vtable 0x003F9528)
inline constexpr uint32_t kOffReloadState        = 0x1E0u; // WeaponState (0=Cooldown, 1=AwaitRelease, 2=Armed)
inline constexpr uint32_t kOffCooldownDeadline   = 0x1E4u; // float race clock timestamp when cooldown ends
inline constexpr uint32_t kOffOwnerIndex         = 0x1E8u; // int32_t owning participant index
inline constexpr uint32_t kOffPendingRetire      = 0x1ECu; // int32_t triggers f_0017F530 on next update
inline constexpr uint32_t kOffImpactArmed        = 0x1F0u; // int32_t non-zero while detonation is permitted
inline constexpr uint32_t kOffInFlight           = 0x1F4u; // int32_t non-zero while projectile is airborne
inline constexpr uint32_t kOffLaunchSfxPos       = 0x1F8u; // Vec3 launch SFX / effect origin
inline constexpr uint32_t kOffBlastSfxPos        = 0x204u; // Vec3 detonation SFX origin
inline constexpr uint32_t kOffLaunchSfx          = 0x210u; // ptr to launch sound emitter
inline constexpr uint32_t kOffFlightSfx          = 0x214u; // ptr to flight loop sound emitter
inline constexpr uint32_t kOffBlastSfx           = 0x218u; // ptr to explosion sound emitter
inline constexpr uint32_t kOffMuzzleOffset       = 0x21Cu; // Vec3 local muzzle offset in car frame

// Car offsets read by ChickenWeaponTrigger
inline constexpr uint32_t kCarOffPoseMatrix      = 0x034u; // 48-byte 3x4 pose
inline constexpr uint32_t kCarOffRegionId        = 0x094u; // int16_t region ID
inline constexpr uint32_t kCarOffLinearVel       = 0x104u; // Vec3 linear velocity

// Sound emitter offsets touched on launch & detonation
inline constexpr uint32_t kSfxOffVolume          = 0x04u;  // float volume (1.0f)
inline constexpr uint32_t kSfxOffPlay            = 0x54u;  // int32_t play trigger
inline constexpr uint32_t kSfxOffStop            = 0x58u;  // int32_t stop trigger
inline constexpr uint32_t kSfxOffLoop            = 0x5Cu;  // int32_t loop flag
inline constexpr uint32_t kSfxOffActive          = 0x60u;  // int32_t active flag
inline constexpr uint32_t kSfxOffDirty           = 0x64u;  // int32_t volume-dirty flag

// Weapon reload / trigger state machine (body + kOffReloadState)
enum WeaponState : int32_t {
    kStateCooldown        = 0,
    kStateAwaitingRelease = 1,
    kStateArmed           = 2,
    kStateDisabled        = 3,
};

// Native PS2 IEEE-754 float bit patterns (exact constants from SLUS_209.10)
inline constexpr uint32_t kBitsOne               = 0x3F800000u; // 1.0f
inline constexpr uint32_t kBitsHalf              = 0x3F000000u; // 0.5f (upward blast lift bias)
inline constexpr uint32_t kBitsNeg0_2            = 0xBE4CCCCDu; // -0.2f (contact jitter min)
inline constexpr uint32_t kBitsPos0_2            = 0x3E4CCCCDu; // +0.2f (contact jitter max)
inline constexpr uint32_t kBitsMinDistSq         = 0x38D1B717u; // 1e-4f (blast singularity guard)
inline constexpr uint32_t kBitsCooldownSec       = 0x40400000u; // 3.0f (native reload duration)
inline constexpr uint32_t kBitsBlastRadius       = 0x40F00000u; // 7.5f m
inline constexpr uint32_t kBitsBlastForce        = 0x46C35000u; // 25000.0f N
inline constexpr uint32_t kBitsStraightBonusSpd  = 0x41E00000u; // 28.0f m/s
inline constexpr uint32_t kBitsMortarFwdWeight   = 0x3F34FDF4u; // 0.707f
inline constexpr uint32_t kBitsMortarUpWeight    = 0x3EFD6389u; // 0.4949f
inline constexpr uint32_t kBitsMortarSpeed       = 0x41C71C72u; // 24.888889f m/s
inline constexpr uint32_t kBitsRainVelY          = 0xC19CF5C3u; // -19.62f m/s
inline constexpr uint32_t kBitsParkBaseY         = 0xC47A0000u; // -1000.0f m (underground park Y)

// Collision / blast class constants
inline constexpr int32_t  kBlastContactClass     = 0x70;        // 112: standard explosive chicken hit class
inline constexpr int32_t  kTerrainClassLimit     = 100;         // classes < 100 are ground/scorch-eligible
inline constexpr int32_t  kMaxDetonateClass      = 1000;        // classes >= 1000 do not detonate

// HUD status string addresses returned by 0x0017E440
inline constexpr uint32_t kHudStrReloading       = 0x003F94F8u;
inline constexpr uint32_t kHudStrDisabled        = 0x003F9500u;
inline constexpr uint32_t kHudStrReady           = 0x003F9510u;
inline constexpr uint32_t kHudStrFallback        = 0x003F9518u;

} // namespace decomp_chicken

extern "C" {
// diff_run-backed native implementations called by the mod/netplay hooks in
// src/game_patches.cpp and src/chicken_replay.cpp.
void chicken_explosion_blast_native(void);
void chicken_launch_native(void);
void chicken_reload_start_native(void);
void chicken_impact_native(void);
}
