# Searchable Function, Symbol & Asset Master Catalogue
## Test Drive: Eve of Destruction (`SLUS_209.10`) Recompilation

**Document ID:** `EOD-CAT-001`  
**Classification:** Master Reference Index (Milestone M6 / Requirements R1, R2, R4)  
**Author:** Central Integration Agent (`teamwork_worker_central_integrator`)  
**Date:** 2026-11-10  
**Status:** Approved Master Catalogue  

---

## 1. How to Use This Catalogue

This document serves as the single authoritative, searchable registry of all reverse-engineered code symbols, memory structures, hardware addresses, and 7,329 internal game assets comprising *Test Drive: Eve of Destruction*.

### Quick Search Syntax
- **By Subsystem**: Filter on `[M1-ASSET]`, `[M2-VEHICLE]`, `[M2-CHICKEN]`, `[M3-TRACK]`, `[M3-AI]`, `[M4-CAREER]`, `[M4-UI]`, `[M4-LOAD]`, `[M5-RENDER]`.
- **By Calling Convention**: Filter on `guest` (EE MIPS), `host` (native C/C++), `hle` (high-level emulator hook).
- **By Asset FourCC**: Filter on FourCC code (e.g. `'TEX '`, `'MODL'`, `'VALT'`, `'SOBL'`, `'SCOL'`, `'ILIN'`, `'LAYT'`).
- **By Hex Address**: Search standard 8-character zero-padded hexadecimal addresses (e.g. `0x0017B918`, `0x0048D2B4`).

---

## 2. Master Function & Symbol Catalogue

The following searchable table lists verified functions across the recompiled engine, including guest addresses, signatures, subsystem classification, and functional purpose:

| Guest EE Address | Symbol / Function Identifier | Subsystem Tag | Type | Signature / Calling Convention | Technical Role & Behavioral Summary |
|---|---|---|---|---|---|
| `0x00100008` | `_start` | `[M4-LOAD]` | guest | `void _start(void)` | PlayStation 2 CRT startup entrypoint; initializes stack, clears BSS, invokes `main`. |
| `0x00100138` | `main` | `[M4-LOAD]` | guest | `int main(int argc, char **argv)` | Master engine entrypoint; initializes SIF RPC, mounts `COMMON.RES`, runs primary event loop. |
| `0x00114020` | `RaceFrameLoop` | `[M4-LOAD]` | guest | `void RaceFrameLoop(void)` | Active 59.94 Hz in-race frame loop; checks active flag at `0x00437BB8`. |
| `0x00114358` | `RunRaceContest` | `[M4-LOAD]` | hle/guest | `void RunRaceContest(void)` | Master race teardown & contest cleanup; cleans up participants and unwinds contest loop. |
| `0x00114860` | `GameDoSingle` | `[M4-LOAD]` | guest | `void GameDoSingle(void)` | Single-race coordinator loop; checks quit flag `0x00437BAC` and contest exit flag `0x00437BA8`. |
| `0x00119888` | `HudDrawLeftRaceRows` | `[M4-UI]` | guest | `void HudDrawLeftRaceRows(void)` | Draws lap, position, and status text; reads left anchor bounds via `0x0011DC50`. |
| `0x00119F40` | `HudDrawRightSpeedRows` | `[M4-UI]` | guest | `void HudDrawRightSpeedRows(void)` | Draws tachometer and speedometer readouts; reads right anchor bounds via `0x0011DC30`. |
| `0x0011ACC0` | `HudDrawCenteredNotice` | `[M4-UI]` | guest | `void HudDrawCenteredNotice(const char *msg)` | Renders screen-centered race alerts, wrong-way warnings, and countdown banners. |
| `0x0011D2C8` | `HudGetViewportCount` | `[M4-UI]` | guest | `int HudGetViewportCount(void)` | Returns active player viewport count from `*(0x003C41AC)` (1, 2, or 4). |
| `0x0011DC30` | `HudGetRightEdgeAnchor` | `[M4-UI]` | guest | `float HudGetRightEdgeAnchor(int view)` | Computes right screen edge anchor; offset by $+\Delta$ for native widescreen presentation. |
| `0x0011DC50` | `HudGetLeftEdgeAnchor` | `[M4-UI]` | guest | `float HudGetLeftEdgeAnchor(int view)` | Computes left screen edge anchor; offset by $-\Delta$ for native widescreen presentation. |
| `0x00123EE0` | `EveValidateChassisLimit` | `[M4-LOAD]` | guest | `int EveValidateChassisLimit(int count)` | Enforces maximum 7 distinct chassis limit per Eve to prevent 32 MB EE RAM exhaustion. |
| `0x001243F0` | `EveValidateVehicleLimit` | `[M4-LOAD]` | guest | `int EveValidateVehicleLimit(int cars, int trailers)` | Enforces structural constraint: $\text{cars} + 2 \times \text{trailers} \le 20$. |
| `0x00128AF0` | `StringTableLookup` | `[M1-ASSET]` | guest | `const char* (uint32_t table_id, uint32_t str_id)` | Resolves localized string pointer from loaded `.tab` string tables. |
| `0x0012A700` | `DAVMapMakeForEve` | `[M4-LOAD]` | guest | `void DAVMapMakeForEve(void *desc)` | Constructs Driver-And-Vehicle asset mapping table; wrapped with stack frame expansion. |
| `0x0012AB38` | `DAVMapValidateModelRequests`| `[M4-LOAD]` | guest | `int DAVMapValidateModelRequests(void *map)` | Validates that total model/texture requests in DAVMap do not exceed 30 entries. |
| `0x0012DB78` | `MemFree` | `[M4-LOAD]` | guest | `void MemFree(void *ptr)` | Frees memory block back to designated `"STAK"` heap allocator pool. |
| `0x0012DDB8` | `MemAlloc` | `[M4-LOAD]` | guest | `void* MemAlloc(uint32_t pool_id, uint32_t size)` | Allocates block with 0x40-byte `"STAK"` header, rounding request to 64 bytes. |
| `0x0012DEC8` | `MemPoolGetAvailable` | `[M4-LOAD]` | guest | `uint32_t MemPoolGetAvailable(uint32_t pool_id)` | Queries free contiguous byte count in specified `"STAK"` heap pool descriptor (`0x003CC438`). |
| `0x001323E8` | `ResLoadAsset` | `[M1-ASSET]` | guest | `void* ResLoadAsset(const char *path, uint32_t pool)` | Searches mounted 0TSR archives, allocates memory, and copies payload to EE RAM. |
| `0x00135FC8` | `f_00135FC8` | `[M1-ASSET]` | guest | `uint32_t f_00135FC8(const void *buf, uint32_t len)` | Hardware CRC32 algorithm using table at `0x003F2598` (poly `0x04C11DB7`). |
| `0x00166B50` | `BattleDrawKillMessages` | `[M3-AI]` | guest | `void BattleDrawKillMessages(void)` | Draws kill attribution banners during demolition battle matches. |
| `0x00166C38` | `BattleCreate` | `[M3-AI]` | guest | `void* BattleCreate(void *desc)` | Instantiates and initializes master `BattleMaster` match controller struct (`0x5AC` bytes). |
| `0x001671E8` | `BattleRespawnTimerTick` | `[M3-AI]` | guest | `void BattleRespawnTimerTick(void *master)` | Arbitrates 2.0-second delay between participant wreck and teleport respawn. |
| `0x00167340` | `BattleInitParticipants` | `[M3-AI]` | guest | `void BattleInitParticipants(void *master)` | Populates per-car participant records, initial health, and score trackers. |
| `0x001675C0` | `BattleRespawnCar` | `[M3-AI]` | guest | `void BattleRespawnCar(void *master, int slot)` | Resets vehicle health, reattaches parts, teleports body, snaps to ground. |
| `0x00167658` | `BattleSortFinisherScores` | `[M3-AI]` | guest | `void BattleSortFinisherScores(void *master)` | Sorts participant places based on net kills ($\text{kills} - \text{deaths}$) into `place[4]`. |
| `0x00167C70` | `BattlePlaceInitialCars` | `[M3-AI]` | guest | `void BattlePlaceInitialCars(void *master)` | Places participants on initial track spawn transforms; wrapped to support $>4$ cars. |
| `0x00167D58` | `BattleUpdate` | `[M3-AI]` | guest | `void BattleUpdate(void *master)` | Master per-frame battle match tick; evaluates 5 net kills victory transition. |
| `0x0016EF58` | `CareerCheckHighwayAccess` | `[M4-CAREER]` | guest | `int CareerCheckHighwayAccess(void)` | Checks rank $\le 49$ gate for unlocking Highway free-roam challenges. |
| `0x0016F4E8` | `HomebaseGetSlotForCar` | `[M4-CAREER]` | hle/guest | `int HomebaseGetSlotForCar(int car_idx)` | Maps vehicle index to Homebase yard slot safely without out-of-bounds assert. |
| `0x0016F748` | `HomebaseGetBuySlotForCar`| `[M4-CAREER]` | hle/guest | `int HomebaseGetBuySlotForCar(int car_idx)` | Maps vehicle index to purchase lot slot 0..4 without asserting. |
| `0x001741C0` | `CareerIsFreeRoamActive` | `[M4-CAREER]` | hle/guest | `int CareerIsFreeRoamActive(void)` | Returns non-zero during career free-roam staging; wrapped by KillableChallengers. |
| `0x00176698` | `RaceSessionInitPhase` | `[M4-LOAD]` | guest | `void RaceSessionInitPhase(void)` | Initializes race-start phase, session counters, and RNG state. |
| `0x00176918` | `RaceSessionSeedRng` | `[M4-LOAD]` | guest | `void RaceSessionSeedRng(uint32_t seed)` | Seeds per-session deterministic pseudo-random number generator. |
| `0x00177718` | `WorldSimulationStep` | `[M2-VEHICLE]` | guest | `void WorldSimulationStep(float dt)` | Master 59.94 Hz physics step; updates rigid bodies, wheel contacts, and raycasts. |
| `0x00179960` | `TrackQuerySolidObstacle`| `[M3-TRACK]` | guest | `int TrackQuerySolidObstacle(int obs_id)` | Evaluates obstacle ID: returns static boundary or dynamic prop handle. |
| `0x00179A08` | `CarGetByParticipantIndex`| `[M2-VEHICLE]`| guest | `void* CarGetByParticipantIndex(int idx)` | Resolves pointer to `Car` struct from participant index 0..19. |
| `0x0017AF80` | `SceneObjectSetPose` | `[M2-VEHICLE]` | guest | `void SceneObjectSetPose(void *obj, float *mat4x3)`| Copies 4x3 pose matrix into entity at `+0x64` and updates bounding sphere. |
| `0x0017B708` | `RigidBodyApplyImpulseAtOffset`| `[M2-VEHICLE]`| guest | `void RigidBodyApplyImpulseAtOffset(void *rb, float *imp, float *off)` | Applies world-space linear/angular impulse vector to rigid body. |
| `0x0017B818` | `CarQueueContactEvent` | `[M2-VEHICLE]` | guest | `void CarQueueContactEvent(void *contact)` | Queues collision contact event into frame contact resolution ring buffer. |
| `0x0017B918` | `CarContactEvaluateObstacleImpulse` | `[M2/M3-SHARED]` | guest | `void CarContactEvaluateObstacleImpulse(...)` | Reconciled contact impulse entrypoint; computes force, deforms mesh, triggers panel detach. |
| `0x0017DA58` | `ChickenExplosionApplyShockwave` | `[M2-CHICKEN]` | hle/guest | `void ChickenExplosionApplyShockwave(float *pos, int attacker, float rad, float force)` | Radial blast evaluation ($R=7.5\text{m}$, $F=25{,}000\text{N}$) on 100x100 spatial hash grid. |
| `0x0017DE30` | `ChickenWeaponRegistryInit`| `[M2-CHICKEN]`| guest | `void ChickenWeaponRegistryInit(void)` | Clears master weapon registry array at `0x003CE3F8`. |
| `0x0017DFA8` | `ChickenWeaponAlloc` | `[M2-CHICKEN]` | guest | `void* ChickenWeaponAlloc(void)` | Allocates 0x228-byte `Ball` weapon, assigning vtables `0x003F9528` and `0x003F9538`. |
| `0x0017E000` | `ChickenWeaponTrigger` | `[M2-CHICKEN]` | guest | `void ChickenWeaponTrigger(void *body, void *car, uint32_t btn)` | Evaluates launch conditions, trigger latch, and computes ballistic trajectory vector. |
| `0x0017E4D8` | `ChickenWeaponStartCooldown`| `[M2-CHICKEN]`| guest | `void ChickenWeaponStartCooldown(void *body)` | Sets reload state machine to cooldown (`body+0x1E0=0`) and assigns 3.0s deadline (`+0x1E4`). |
| `0x0017E5C8` | `ChickenWeaponUpdate` | `[M2-CHICKEN]` | guest | `void ChickenWeaponUpdate(void *body)` | Integrates projectile ballistics, updates world position, triggers roll cluck audio. |
| `0x0017E718` | `ChickenWeaponDetonate` | `[M2-CHICKEN]` | guest | `void ChickenWeaponDetonate(void *body)` | Spawns explosion sprite sequence, plays `explode.sfx`, invokes radial shockwave `0x0017DA58`. |
| `0x00185390` | `RigidBodyMasterAlloc` | `[M2-VEHICLE]` | guest | `void* RigidBodyMasterAlloc(int count)` | Allocates master 0xB0-byte rigid body array via `0x001DD2F8` stored at `*(0x00494D30)`. |
| `0x00188F20` | `CarResetPoseAndDamage` | `[M2-VEHICLE]` | guest | `void CarResetPoseAndDamage(void *car)` | Clears velocity, zeroes health scalar `car+0x1BEC`, restores bone rest positions. |
| `0x0018AA50` | `CarSetGear` | `[M2-VEHICLE]` | guest | `void CarSetGear(void *car, int gear)` | Writes gear into `car + 0x1B90` (`-10` automatic, `-1` reverse, `0` neutral, `1..N`). |
| `0x0018AA58` | `CarSetHandbrake` | `[M2-VEHICLE]` | guest | `void CarSetHandbrake(void *car, float val)` | Writes handbrake pressure `[0..1]` into `car + 0x1B7C` (multiplied by 8.0 in physics). |
| `0x0018AA60` | `CarSetClutchEngagement` | `[M2-VEHICLE]` | guest | `void CarSetClutchEngagement(void *car, float val)` | Writes drivetrain coupling into `car + 0x1B78` (`1.0` engaged, `0.0` disconnected). |
| `0x0018AA68` | `CarSetFootbrake` | `[M2-VEHICLE]` | guest | `void CarSetFootbrake(void *car, float val)` | Writes footbrake pressure `[0..1]` into `car + 0x1B74`. |
| `0x0018AA70` | `CarSetThrottle` | `[M2-VEHICLE]` | guest | `void CarSetThrottle(void *car, float val)` | Writes gas throttle `[0..1]` into `car + 0x1B70`. |
| `0x0018AA80` | `CarSetSteering` | `[M2-VEHICLE]` | guest | `void CarSetSteering(void *car, float val)` | Writes normalized steer angle `[-1..+1]` into `car + 0x1B6C`. |
| `0x0018AFE0` | `CarDamageUpdate` | `[M2-VEHICLE]` | guest | `void CarDamageUpdate(void *car)` | Steps per-car damage accumulator, updates smoke emitters, checks wreck enum. |
| `0x0018CA98` | `CarDeformApplyImpulse` | `[M2-VEHICLE]` | guest | `void CarDeformApplyImpulse(void *car, float *imp, float *pos)` | Applies deformation impulse to 157 bones in `Bone.mvl`; clamps to 0.9f in free-roam. |
| `0x0018CFE8` | `CarUpdateDetachablePanels` | `[M2-VEHICLE]` | guest | `void CarUpdateDetachablePanels(void *car)` | Evaluates 8-row table at `0x0044AE68`; spawns detached panels into `0x00494D60`. |
| `0x0018DBF8` | `CarInitDetachablePanels` | `[M2-VEHICLE]` | guest | `void CarInitDetachablePanels(void *car)` | Populates 8-row detachable panel table from car geometry bounds. |
| `0x0018F2A8` | `HumanDriverUpdate` | `[M2-VEHICLE]` | guest | `void HumanDriverUpdate(void *car)` | Reads gamepad input state and injects controls into `car + 0x1B6C..0x1B90`. |
| `0x00193B20` | `AIVengeanceEvaluateTarget`| `[M3-AI]` | guest | `int AIVengeanceEvaluateTarget(void *ai)` | Evaluates AI Vengeance cheat bit `0x20` in `0x003C4190`, targeting player car. |
| `0x001A00B8` | `TrackLoadSolidCollision` | `[M3-TRACK]` | guest | `void* TrackLoadSolidCollision(const char *path)` | Parses `track.sol` (`SOBL` v3); validates 264-byte records into `*(0x004473FC)`. |
| `0x001A2D20` | `AICarUpdate` | `[M3-AI]` | guest | `void AICarUpdate(void *car)` | Master AI per-tick driver; evaluates spline following, lead pursuit, and recovery unstick. |
| `0x001B2868` | `DetachedWheelPoolReset` | `[M2-VEHICLE]` | guest | `void DetachedWheelPoolReset(void)` | Clears 16-entry detached wheel phob pool at `0x00484E68`. |
| `0x001B2E78` | `DetachedWheelPhobUpdate` | `[M2-VEHICLE]` | guest | `void DetachedWheelPhobUpdate(void *phob)` | Steps detached wheel rigid body physics; settles after 40.0s lifetime. |
| `0x001B3068` | `DetachedWheelSpawn` | `[M2-VEHICLE]` | guest | `void DetachedWheelSpawn(void *car, int wheel_idx)` | Spawns flying wheel phob when axle displacement shears hub connection. |
| `0x001B63E8` | `CarExecuteTrackReset` | `[M3-TRACK]` | guest | `void CarExecuteTrackReset(void *car)` | Teleports vehicle back to nearest centerline spline node and snaps to terrain. |
| `0x001B8370` | `CarRecoveryCountdownUpdate`| `[M3-TRACK]` | guest | `void CarRecoveryCountdownUpdate(void *car)` | Steps wrong-way/rollover recovery timer; monitors SELECT button recovery trigger. |
| `0x001C31A0` | `CameraDirectorUpdate` | `[M4-CAREER]` | hle/guest | `void CameraDirectorUpdate(void)` | Updates showroom/garage camera; injects 360-degree orbit and turntable sweep. |
| `0x001D2DC8` | `CameraDirectorPlayTrack` | `[M4-CAREER]` | hle/guest | `void CameraDirectorPlayTrack(int track_id)` | Triggers dynamic camera playback clip sequence. |
| `0x001D57A8` | `SpatialHashQueryRadial` | `[M2-CHICKEN]` | guest | `int SpatialHashQueryRadial(float *pos, float rad, void **out)` | Queries 100x100 2D spatial hash grid at `0x0048D2B4` for entities within radius. |
| `0x001D8F58` | `TerrainSnapTransformToGround`| `[M3-TRACK]` | guest | `void TerrainSnapTransformToGround(float *mat4x3)` | Raycasts terrain collision (`track.bsc`) and snaps 4x3 pose to ground height/normal. |
| `0x001D97B0` | `ArenaMasterFactory` | `[M3-AI]` | guest | `void* ArenaMasterFactory(int mode_id)` | Instantiates specialized demolition arena combat controller for 0x18000000 modes. |
| `0x001DF9A0` | `DeformResetMeshVertices` | `[M2-VEHICLE]` | guest | `void DeformResetMeshVertices(void *car_mesh)` | Resets deformed mesh vertices and bone offsets back to rest coordinates. |
| `0x001E0DC8` | `DetachedPanelPoolInit` | `[M2-VEHICLE]` | guest | `void DetachedPanelPoolInit(void)` | Initializes 16-entry detached panel debris pool at `0x00494D60`. |
| `0x001E1E88` | `DetachedPanelSpawn` | `[M2-VEHICLE]` | guest | `void DetachedPanelSpawn(void *car, int row)` | Instantiates detached hood/door/bumper/decklid rigid body phob into `0x00494D60`. |
| `0x001E25C8` | `DetachedPanelPhobUpdate` | `[M2-VEHICLE]` | guest | `void DetachedPanelPhobUpdate(void *phob)` | Steps detached panel physics, ground collisions, and aerodynamic drag. |
| `0x00201938` | `BattleDisplayScoreboard` | `[M3-AI]` | guest | `void BattleDisplayScoreboard(void *master)` | Presents post-match battle standings, kills, deaths, and prize allocations. |
| `0x0020FD48` | `SceneCreateCarAndWeapons`| `[M4-LOAD]` | guest | `void SceneCreateCarAndWeapons(void *session_desc)` | Walks 20 participant slots of 0x78 bytes; instantiates models, physics, and weapons. |
| `0x00214678` | `TrackLoadVisualScene` | `[M3-TRACK]` | guest | `void* TrackLoadVisualScene(const char *path)` | Loads `track.grf` (`GRAF` v18); stores pointer in `*(0x003D1C18)` (up to 2,239 parts). |
| `0x00216ED0` | `TrackLoadSpatialBounds` | `[M3-TRACK]` | guest | `void* TrackLoadSpatialBounds(const char *path)` | Loads `track.bsp` (`BSPT` v0) for hierarchical spatial visibility queries. |
| `0x00216F60` | `TrackLoadPrimitiveTree` | `[M3-TRACK]` | guest | `void* TrackLoadPrimitiveTree(const char *path)` | Loads `track.bpp` (`BPPT`) bounding primitive tree. |
| `0x00218C38` | `VisualCarSubmitWheelsAndParts` | `[M2-VEHICLE]` | guest | `void VisualCarSubmitWheelsAndParts(void *car)` | Submits visible component groups to VU1; suppresses groups cleared in `car+0x2AC`. |
| `0x00218E68` | `VisualCarSubmitLOD` | `[M2-VEHICLE]` | guest | `void VisualCarSubmitLOD(void *car, int lod_idx)` | Submits selected `Low0..4.mod` mesh batches to VU1 via `0x0034BDA8`. |
| `0x0021ABF8` | `CarLoadLODModels` | `[M1-ASSET]` | guest | `void CarLoadLODModels(void *car, const char *name)` | Loads `lod.val` and parses LOD mesh array (`Low0.mod` through `Low4.mod`). |
| `0x0021C130` | `VisualCarCloneShared` | `[M4-LOAD]` | guest | `void VisualCarCloneShared(void *dst, void *src)` | Clones visual car for identical chassis; aliases mesh handles to save 1.8 MB EE RAM. |
| `0x0021C6E0` | `CarSelectLOD` | `[M1-ASSET]` | guest | `int CarSelectLOD(void *car, float dist)` | Evaluates camera distance against `lod.val` thresholds, returning active LOD index. |
| `0x00226768` | `TrackLoadSurfaceCollision` | `[M3-TRACK]` | guest | `void* TrackLoadSurfaceCollision(const char *path)`| Loads `track.bsc` (`SCOL` v4); relocates Table 1 and Table 2 into `*(0x003D2098)`. |
| `0x002271D0` | `TrackRaycastSurfaceTerrain` | `[M3-TRACK]` | guest | `int TrackRaycastSurfaceTerrain(float *ray, ...)` | Evaluates 2.5D topological raycast against SCOL Table 1/2 triangles. |
| `0x00227808` | `TrackGetSurfaceMaterial` | `[M3-TRACK]` | guest | `uint8_t TrackGetSurfaceMaterial(int tri_idx)` | Retrieves packed material ID (`0` grass, `2` dirt, `5` mud, `6` asphalt) from Table 2. |
| `0x00227EE8` | `WeaponSceneAttach` | `[M2-CHICKEN]` | guest | `void WeaponSceneAttach(void *car, void *weapon)` | Attaches chicken weapon interface pointer to vehicle weapon mounting socket. |
| `0x00230CD8` | `TrackEnvironmentMasterLoad`| `[M3-TRACK]` | guest | `void TrackEnvironmentMasterLoad(const char *name)`| Master sequence loading `track.bsc`, `track.bsp`, `track.bpp`, and `track.grf`. |
| `0x00270C10` | `MenuRun` | `[M4-UI]` | hle/guest | `int MenuRun(const char *desc, int x, int y, int flags)` | Central modal dialog loop; runs until `*(0x004A1AC0) == 1`, returning `*(0x004A1AC4)`. |
| `0x0027C66C` | `MenuComputeTransitionSpeed`| `[M4-UI]` | guest | `float MenuComputeTransitionSpeed(float dur)` | Computes transition speed $1.0 / \max(0.01, \text{dur})$; clamped to prevent zero division. |
| `0x0027D590` | `UISetMenuIndex` | `[M4-UI]` | guest | `void UISetMenuIndex(void *dlg, int idx)` | Updates focused widget index; protected against NULL widget selection on empty lists. |
| `0x002A1298` | `MainMenuUpdate` | `[M4-UI]` | guest | `void MainMenuUpdate(void)` | Master front-end state machine dispatching Career, Action, Options, and Profile menus. |
| `0x002C0DC8` | `ArcadeSingleMultiPrompt` | `[M4-UI]` | guest | `int ArcadeSingleMultiPrompt(void)` | Presents single vs multi-player prompt (`s_and_m.lyt`), returning selection code. |
| `0x002C1A20` | `LoadingScreenEventProgress` | `[M4-LOAD]` | hle/guest | `void LoadingScreenEventProgress(void)` | Renders race loading screen; throttled in HLE to bypass artificial 50ms vblank pauses. |
| `0x002C2188` | `LoadingScreenMenuProgress` | `[M4-LOAD]` | hle/guest | `void LoadingScreenMenuProgress(void)` | Renders menu loading screen; throttled in HLE for instant NVMe loading. |
| `0x002DED18` | `CareerValidatePartPurchase` | `[M4-CAREER]` | hle/guest | `int CareerValidatePartPurchase(int cat, int tier)` | Validates multi-stage upgrade tree and category swapping for AutoShop upgrades. |
| `0x002DFDF8` | `CareerGetPartCompatibility` | `[M4-CAREER]` | hle/guest | `uint32_t CareerGetPartCompatibility(int part)` | Returns vehicle compatibility bitmask for parts; wrapped for universal part fitting. |
| `0x002E0958` | `CareerAwardPurseMoney` | `[M4-CAREER]` | guest | `void CareerAwardPurseMoney(int purse)` | Adds prize winnings to `CDBupgr->money` and updates `CDBupgr->total_earned`. |
| `0x002E1BB0` | `CareerGetGarageCarCount` | `[M4-CAREER]` | hle/guest | `int CareerGetGarageCarCount(void)` | Returns garage car count; wrapped to support expanded 16-car garage storage. |
| `0x002E2A70` | `FormatOrdinalSuffix` | `[M4-UI]` | guest | `const char* FormatOrdinalSuffix(int rank)` | Formats rank string ("1st", "2nd", "3rd", "4th", "Champion"). |
| `0x002E3BC8` | `CareerComputeDepreciation` | `[M4-CAREER]` | hle/guest | `int CareerComputeDepreciation(int base, float cond)` | Computes wear trade-in depreciation: $\text{round}((\text{Chassis} + \text{Parts}) \times (1.0 - \text{cond}))$. |
| `0x002E3D20` | `CareerComputePartsTradeIn` | `[M4-CAREER]` | guest | `int CareerComputePartsTradeIn(void *car)` | Calculates 75% refund of catalog price for all installed upgrade parts. |
| `0x002E3E70` | `CareerComputeChassisTradeIn`| `[M4-CAREER]` | hle/guest | `int CareerComputeChassisTradeIn(int price_paid)` | Calculates 90% trade-in value of vehicle purchase price. |
| `0x002E3F20` | `CareerComputeNetTradeIn` | `[M4-CAREER]` | guest | `int CareerComputeNetTradeIn(void *car)` | Evaluates net payout: $(\text{Chassis} + \text{Parts}) - \text{Depreciation}$. |
| `0x002E4230` | `CareerCheckDealerUnlock` | `[M4-CAREER]` | guest | `int CareerCheckDealerUnlock(void)` | Checks rank $\le 49$ gate for unlocking tier-3 dealership purchase stock. |
| `0x002E5200` | `CareerEvaluateEveQualification`| `[M4-CAREER]` | guest | `int CareerEvaluateEveQualification(int eve_id)` | Evaluates rank window $\text{max\_rank} < \text{rank} \le \text{min\_rank}$ for Eve eligibility. |
| `0x002E6C78` | `CareerHandleChampionState` | `[M4-CAREER]` | guest | `void CareerHandleChampionState(void)` | Clears open Eve list when rank $= 0$; wrapped to clamp rank to 1 for infinite replay. |
| `0x00337718` | `TimerGetMs` | `[M4-LOAD]` | hle/guest | `uint32_t TimerGetMs(void)` | High-resolution millisecond timer; wrapped to skip artificial 2-4 second wait loops. |
| `0x0033D408` | `StrmMountArchive` | `[M1-ASSET]` | guest | `int StrmMountArchive(const char *path)` | Mounts `.car`, `.res`, or `.trk` 0TSR container; parses 0x28 header and 0x44 entries. |
| `0x0033D4F8` | `StrmFindFile` | `[M1-ASSET]` | hle/guest | `int StrmFindFile(const char *name)` | Resolves file entry index in mounted 0TSR TOC; returns `-1` if absent. |
| `0x0034BDA8` | `ModelSubmitVu1Packets` | `[M5-RENDER]` | guest | `void ModelSubmitVu1Packets(void *vif_stream)` | Submits `4SPS` VIF DMA packet stream to VU1/GIF interface for hardware rendering. |
| `0x0034CEF0` | `ModelLoadFromArchive` | `[M1-ASSET]` | guest | `void* ModelLoadFromArchive(const char *path)` | Reads `.mod` file, parses `MODL` v9 header, sets up part offsets, returns handle. |
| `0x003521E8` | `TextureRelocateOffsets` | `[M1-ASSET]` | guest | `void TextureRelocateOffsets(void *tex_hdr)` | Relocates mip offsets in `TEX` v10 header; explicitly skips zero sentinels. |
| `0x00352D28` | `TextureSubsystemInit` | `[M5-RENDER]` | guest | `void TextureSubsystemInit(void)` | Allocates GS texture manager state and static 400 KB `mip0` cache at `0x004A9830`. |
| `0x003537F0` | `TextureRegisterHiResMip0`| `[M1-ASSET]` | guest | `void TextureRegisterHiResMip0(void *tex, void *tm0)`| Binds dynamically streamed 2x `.tm0` buffer into parent `TEX` descriptor. |
| `0x003545B8` | `TextureLoadHiResTm0` | `[M1-ASSET]` | guest | `void TextureLoadHiResTm0(const char *name)` | Streams `.tm0` texture from 0TSR extractable partition into ring buffer `0x004A9830`. |
| `0x0035EE38` | `f_0035EE38` | `[M5-RENDER]` | guest | `void f_0035EE38(void *tex, int mip)` | Evaluates mip height: $\ge 32$ uses `upload_addr8` swizzle; $<32$ branches to linear. |
| `0x0035F140` | `f_0035F140` | `[M5-RENDER]` | guest | `void f_0035F140(void *tex, int mip)` | Linear GS upload path for small mip tails ($<32$ pixels). |
| `0x003698E8` | `HomebaseBuildMenu` | `[M4-CAREER]` | guest | `void HomebaseBuildMenu(void)` | Builds Homebase hub / AutoShop / Garage / Challengers UI menu list. |
| `0x0036E9C0` | `DealerBuyScreen` | `[M4-CAREER]` | hle/guest | `void DealerBuyScreen(void)` | Dealership purchase screen `MenuRun` loop; wrapped to capture stack frame `0x980`. |
| `0x003702F0` | `AutoShopUpdateSelection` | `[M4-CAREER]` | hle/guest | `void AutoShopUpdateSelection(void)` | Per-frame dealership car update; wrapped to turn vehicle showroom pages on L1/R1. |
| `0x00370DD8` | `DealerGetSlotForCar` | `[M4-CAREER]` | hle/guest | `int DealerGetSlotForCar(int car_idx)` | Maps vehicle to dealership showroom yard slot without panicking. |
| `0x0037ECB8` | `AutoShopCheckUnlock` | `[M4-CAREER]` | guest | `int AutoShopCheckUnlock(void)` | Checks rank $\le 79$ gate for unlocking AutoShop performance upgrades. |
| `0x0039AFB0` | `MovieBitBufferRefill` | `[M1-ASSET]` | guest | `void MovieBitBufferRefill(void)` | Refills IPU video bitstream buffer from optical disc; optimized in `movie_bits.cpp`. |

---

## 3. Master Memory, Register & Global State Catalogue

The following table catalogs verified global pointers, state arrays, cheat masks, and vtables in the recompiled binary:

| Memory Address | Symbol / State Descriptor | Stride / Size | Subsystem Tag | Description & Hardware Role |
|---|---|---|---|---|
| `0x00090000` | `g_RelocatedContestTable` | `12 B / entry` | `[M4-CAREER]` | Relocated master contest table providing event descriptors and purse values. |
| `0x003C3A70` | `g_MasterEventRegistry` | `12 B / entry` | `[M3-AI]` | Master table of 26 race event descriptors (`type`, `name`, `display_name`). |
| `0x003C4190` | `g_CheatMaskActive` | `4 B (uint32)` | `[M2/M3-SHARED]`| Active cheat bitmask (`0x1` Chicken machinegun, `0x8` instant reload, `0x20` AI Vengeance). |
| `0x003C41AC` | `g_ActiveViewportCount` | `4 B (uint32)` | `[M4-UI]` | Number of active screen viewports (1 single-player, 2 split, 4 quadrant). |
| `0x003C4310` | `g_ViewportBoundsTable` | `0x18 B / view` | `[M4-UI]` | Viewport boundary table (Top, Left, Right, Bottom, CenterX, CenterY). |
| `0x003C4370` | `g_HudLeftQueue` | `0x290 B` | `[M4-UI]` | Queue of active left-aligned HUD text records (lap, position, split times). |
| `0x003C4600` | `g_HudRightQueue` | `0x290 B` | `[M4-UI]` | Queue of active right-aligned HUD gauges (tachometer, speedometer readouts). |
| `0x003CC438` | `kPool` | `0x60 B` | `[M4-LOAD]` | Root descriptor for primary `"STAK"` heap allocator pool. |
| `0x003CD190` | `g_PrimaryCourseCache` | `12 B / entry` | `[M3-AI]` | Primary circuit navigation cache; accepts resource tag `0x104F128B` (60B nodes). |
| `0x003CD1B4` | `g_PrimaryCourseCount` | `4 B (uint32)` | `[M3-AI]` | Active course count in spline cache (up to 3 routes). |
| `0x003CDACC` | `g_EngineTimeSeconds` | `4 B (float)` | `[M2/M3-SHARED]`| Master monotonic float clock (seconds) used for weapon reload and AI recovery. |
| `0x003CE3F8` | `g_WeaponRegistry` | `12 B / entry` | `[M2-CHICKEN]` | Active weapon entity registry (`owner_idx`, `slot`, `interface_ptr`; max 16). |
| `0x003CE4B8` | `g_WeaponRegistryCount`| `4 B (uint32)` | `[M2-CHICKEN]` | Number of active weapon projectiles currently allocated in the world. |
| `0x003CE4F8` | `g_CarEntityList` | `4 B / ptr` | `[M2-VEHICLE]` | Pointers to active `Car` structures linked to rigid body masters. |
| `0x003D1C18` | `g_TrackVisualGraf` | `4 B (ptr)` | `[M3-TRACK]` | Global pointer to loaded `track.grf` visual scene graph (`GRAF` v18). |
| `0x003D2098` | `g_TrackTerrainScol` | `4 B (ptr)` | `[M3-TRACK]` | Global pointer to loaded `track.bsc` terrain triangle collision (`SCOL` v4). |
| `0x003D9384` | `kCareerSave` | `0xE7C B (3,708B)`| `[M4-CAREER]`| Root `CareerSaveInfo` pointer holding driver metadata, upgrades, and results. |
| `0x003D9394` | `kCDBRes` | `0x194 B (404 B)` | `[M4-CAREER]`| `CDBResSaveInfo` pointer holding rank, open Eves, and historical results. |
| `0x003D9398` | `kCDBupgr` | `0xC68 B (3,176B)`| `[M4-CAREER]`| `CDBupgr` pointer holding funds, parts inventory, and 4 garage slots. |
| `0x003F2598` | `g_Crc32Table` | `1024 B (256 W)`| `[M1-ASSET]` | Precomputed MSB-first CRC32 lookup table (generator polynomial `0x04C11DB7`). |
| `0x003F7770` | `vtable_BattleMaster` | `0x40 B` | `[M3-AI]` | Virtual function table for `BattleMaster` demolition match controller. |
| `0x003F9528` | `vtable_ChickenWeapon` | `0x20 B` | `[M2-CHICKEN]` | Virtual function table for weapon trigger interface (`body + 0x1DC`). |
| `0x003F9538` | `vtable_BallisticPhys`| `0x20 B` | `[M2-CHICKEN]` | Virtual function table for projectile kinematic physics (`body + 0x98`). |
| `0x004111B0` | `g_MenuInputJumpTable` | `0x20 B (8 W)` | `[M4-UI]` | Function jump table routing D-pad Up/Down/Left/Right, Cross, Triangle in `MenuRun`. |
| `0x0041EFF8` | `g_RankGateTable` | `12 B (3 W)` | `[M4-CAREER]` | Career progression unlock thresholds: `{79, 49, 0}`. |
| `0x00437BA8` | `g_ContestLoopExitFlag`| `4 B (uint32)` | `[M4-LOAD]` | Set to 1 when a race event concludes, exiting back to `GameDoSingle`. |
| `0x00437BAC` | `g_RaceQuitFlag` | `4 B (uint32)` | `[M4-LOAD]` | Set to 1 when player pauses and selects Quit Race, aborting purse award. |
| `0x00437BB8` | `g_RaceFrameLoopActive`| `4 B (uint32)` | `[M4-LOAD]` | Flag maintaining execution of the 59.94 Hz `RaceFrameLoop`. |
| `0x004473FC` | `g_TrackSolidSobl` | `4 B (ptr)` | `[M3-TRACK]` | Global pointer to loaded `track.sol` barrier collision array (`SOBL` v3). |
| `0x0044AE68` | `g_DetachablePanelTable`| `0x100 B (8 rows)`| `[M2-VEHICLE]`| 8-row panel hinge attachment table and displacement thresholds. |
| `0x00484E68` | `g_DetachedWheelPool` | `0xB00 B (16 phobs)`| `[M2-VEHICLE]`| Ring buffer of 16 detached wheel debris rigid bodies (40.0s lifetime). |
| `0x00485098` | `g_AutoResetEnabled` | `4 B (uint32)` | `[M3-TRACK]` | Flag enabling automatic vehicle recovery reset on rollover or off-track. |
| `0x0048522C` | `g_DriverCount` | `4 B (uint32)` | `[M2-VEHICLE]` | Total number of active participant drivers in the current race session. |
| `0x00485230` | `g_DriverTable` | `0x960 B (20 slots)`| `[M2-VEHICLE]`| Master array of 20 participant slots (0x78 bytes each). |
| `0x0048D2B4` | `g_SpatialHashGrid` | `40,000 B` | `[M2-CHICKEN]` | 100x100 2D spatial hash grid (8.0m cell size) queried by shockwaves. |
| `0x00494D28` | `g_RigidBodyMasterCount`| `4 B (uint32)` | `[M2-VEHICLE]` | Number of active rigid bodies allocated in `*(0x00494D30)`. |
| `0x00494D30` | `g_RigidBodyMasterArray`| `4 B (ptr)` | `[M2-VEHICLE]` | Contiguous array of 0xB0-byte rigid body master physics records. |
| `0x00494D60` | `g_DetachedPanelPool` | `0xB00 B (16 phobs)`| `[M2-VEHICLE]`| Ring buffer of 16 detached body panel debris rigid bodies. |
| `0x004A1AC0` | `g_ModalDoneFlag` | `4 B (uint32)` | `[M4-UI]` | Flag set to 1 when a `MenuRun` modal dialog finishes user interaction. |
| `0x004A1AC4` | `g_ModalResultId` | `4 B (int32)` | `[M4-UI]` | Return code of finished modal dialog (-1 cancel, or widget result ID). |
| `0x004A1B30` | `g_MenuTransitionDuration`| `4 B (float)` | `[M4-UI]` | Transition duration in seconds (default `0.65f` / `0x3F266666`). |
| `0x004A1B3C` | `g_MenuTransitionSpeed` | `4 B (float)` | `[M4-UI]` | Computed animation rate ($1.0 / \max(0.01, \text{duration})$). |
| `0x004A9830` | `g_TextureMip0CachePool`| `400 KB (0x64000B)`| `[M1-ASSET]`| Fixed circular ring buffer holding dynamically streamed 2x `.tm0` mips. |
| `0x004AB130` | `g_DealerShopSlots` | `0x78 B (5 slots)` | `[M4-CAREER]`| 5 rotating vehicle purchase slots displayed in Dealership showroom. |
| `0x004AB158` | `g_EveQualificationTable`| `12 B / entry` | `[M4-CAREER]`| Table of rank qualification criteria for all 40 career Eve events. |

---

## 4. Master Asset Census Across All 24 FourCC Types

The retail game disc incorporates **7,329 total internal file records** across all 70 archives, categorized under exactly **24 distinct FourCC formats**:

| FourCC | Extensions | Chunk Version | Internal Count | Typical Sizes | Context & Subsystem Role |
|---|---|---|---|---|---|
| `TEX ` | `.tex`, `.tm0`, `.art` | 10 | **4,923** | 1.6 KB – 176 KB | PlayStation 2 GS raster textures: 3,309 base `.tex` files, 1,547 2x hi-res `.tm0` streamed textures, 67 `.art` paint stamps. 8-bit (`0x201`) and 4-bit (`0x200`) formats with 256-entry ARGB palettes. |
| `VALT` | `.val`, `.va0`, `.mvl`, `.pln` | 11 (v6) | **755** | 200 B – 45 KB | Key/value data tables: vehicle chassis tuning (`physics.val`), visual deformation bones (`Bone.mvl`), event scenery placement (`c_*.va0`), driver AI rosters (`dirt.val`). |
| `MODL` | `.mod` | 9 | **490** | 2.5 KB – 220 KB | 3D polygonal geometry: vehicle LOD chains (`Low0.mod`..`Low4.mod`), detachable body panels, dynamic obstacles (`cone.mod`, `barrel.mod`), chicken projectile (`chick1.mod`). |
| `IMAG` | `.img` | 3 | **264** | 512 B – 64 KB | 2D UI raster images: analog speedometer dials, needles, radar blips, HUD icons, menu button textures. |
| `ILIN` | `.ild`, `.ili` | 27 | **211** | 4 KB – 32 KB | AI navigation splines: 60-byte course centerline nodes, passing lanes, pit branches, race progress lines. |
| `LAYT` | `.lyt` | 5 | **200** | 1 KB – 18 KB | User interface layout trees: menu dialog widget structures, button hierarchies, navigation graphs (`start.lyt`, `garage.lyt`). |
| `SFX0` | `.sfx`, `.sf_` | 3 | **112** | 1.2 KB – 85 KB | SPU2 ADPCM mono sound effects: vehicle revs, screeching tires, metal impacts, chicken clucks (`chkstart.sfx`, `explode.sfx`). |
| `STAB` | `.tab`, `.obt` | 0 | **89** | 500 B – 8 KB | Localization string tables: UI labels, menu options, vehicle names, race event descriptions. |
| `SETU` | `.gar` | 2 | **64** | 1 KB – 12 KB | Career garage setups: dealership inventory pools, event car restrictions, AI opponent car configurations. |
| `GTEX` | `.gtx` | 4 | **62** | Fixed 38,912 B | Monospaced & proportional UI font sheets, glyph widths, and kerning offset tables. |
| `CANV` | `.cvs` | 1 | **35** | Fixed 524,324 B | Full-screen 512×512 UI background images: garage backdrop, dealership lot, trophy screen, title canvas. |
| `Anim` | `.anm` | 2 | **18** | Fixed 4,128 B | Skeletal animation headers: bone hierarchies (25 human bones, 7 chicken bones), frame counts, playback rates. |
| `Anm0` | `.an0` | 2 | **15** | 20 KB – 166 KB | Skeletal keyframe transform streams: driver steering/shifting motions, spectator cheering, chicken waddle walk cycle. |
| `SOBL` | `.sol` | 3 | **15** | 12 KB – 180 KB | Solid boundary collision geometry: 264-byte (`0x108`) Oriented Bounding Box (OBB) records (`track.sol`). |
| `GRAF` | `.grf` | 18 | **15** | 3.4 MB – 4.5 MB | Complete track visual rendering scenes: up to 2,239 `4SPS` VIF packet parts per venue (`track.grf`). |
| `BSPT` | `.bsp` | 0 | **15** | 1 KB – 16 KB | Binary space partitioning tree headers for spatial visibility acceleration and culling. |
| `SCOL` | `.bsc` | 4 | **15** | 250 KB – 1.8 MB | Surface terrain & road triangle collision meshes: 32-byte winged-edge Table 1 + 20-byte plane equation Table 2 (`track.bsc`). |
| `BPPT` | `.bpp` | 2147483655 | **15** | 4 KB – 48 KB | Bounding polygon spatial partition trees indexing circuit geometry. |
| `ESFX` | `.ens` | 2 | **6** | 800 B – 4 KB | Environmental audio emitter definitions: stadium reverb zones, PA speaker triggers, crowd roar origins. |
| `STMP` | `.stp` | 3 | **4** | 2 KB – 16 KB | Custom paint shop livery decal stamps, number stencils, and brush masks. |
| `LANG` | `.lng` | 1 | **2** | 256 B | Localization language configuration flags (English, French). |
| `BOPT` | `.opt` | 1 | **2** | 512 B | Binary runtime performance options and visual quality switches. |
| `XDSP` | `.dsp` | 1 | **1** | 1 KB | Video display and screen raster timing configuration parameters. |
| `ICON` | `.icn` | 1 | **1** | 14,240 B | Official Sony PlayStation 2 3D animated memory card save icon model and texture. |
| **TOTAL** | — | — | **7,329** | — | **100% of disc assets fully indexed, extracted, and verified.** |

---

## 5. Master Disc Archive Inventory (70 Containers)

All 70 archives on the retail disc utilize the `0TSR` v6 format with 0x44-byte directory entries:

| Archive Path | Type | File Size | Entry Count | Extract Block Offset | Content Summary |
|---|---|---|---|---|---|
| `DATA/CAR/ALGAR.CAR` | Vehicle | 569,280 B | 13 entries | `0x0006B800` | Algar chassis, 5 LODs, `physics.val`, `Bone.val`, `Bone.mvl`, liveries. |
| `DATA/CAR/AMBULA_1.CAR` | Vehicle | 621,440 B | 15 entries | `0x00078000` | Ambulance chassis, flashing beacon lights, siren sounds, heavy physics. |
| `DATA/CAR/ARROW.CAR` | Vehicle | 582,656 B | 14 entries | `0x0006F800` | Arrow chassis, compact muscle car tuning, detachable hood/doors. |
| `DATA/CAR/BLITZ.CAR` | Vehicle | 574,464 B | 14 entries | `0x0006E800` | Blitz chassis, high-revving 4-cylinder engine, front-wheel drive tuning. |
| `DATA/CAR/BULLDOG.CAR` | Vehicle | 601,088 B | 14 entries | `0x00074000` | Bulldog chassis, heavy pickup truck, reinforced bumper attachments. |
| `DATA/CAR/BUS.CAR` | Vehicle | 745,472 B | 16 entries | `0x00092000` | Full-length School Bus chassis, multiple passenger windows, dual rear wheels. |
| `DATA/CAR/CANNON.CAR` | Vehicle | 593,920 B | 14 entries | `0x00072800` | Cannon chassis, heavy sedan, massive front impact absorption. |
| `DATA/CAR/COPCAR.CAR` | Vehicle | 612,352 B | 15 entries | `0x00076800` | Police Cruiser chassis, light bar, push bumper, pursuit engine tuning. |
| `DATA/CAR/COYOTE.CAR` | Vehicle | 586,752 B | 14 entries | `0x00070800` | Coyote chassis, mid-size coupe, balanced dirt oval suspension. |
| `DATA/CAR/DESIO.CAR` | Vehicle | 578,560 B | 14 entries | `0x0006F000` | Desio chassis, lightweight compact coupe, aggressive steering response. |
| `DATA/CAR/DIGIT.CAR` | Vehicle | 572,416 B | 14 entries | `0x0006E000` | Digit chassis, compact hatchback, low frontal area and drag. |
| `DATA/CAR/DURAND.CAR` | Vehicle | 598,016 B | 14 entries | `0x00073800` | Durand chassis, vintage station wagon, extended rear crumple zones. |
| `DATA/CAR/EMPIRE.CAR` | Vehicle | 606,208 B | 14 entries | `0x00075000` | Empire chassis, full-size luxury cruiser, high polar inertia. |
| `DATA/CAR/FILLMORE.CAR` | Vehicle | 610,304 B | 14 entries | `0x00076000` | Fillmore chassis, classic muscle car, high torque V8 engine specs. |
| `DATA/CAR/GLADIA_1.CAR` | Vehicle | 616,448 B | 15 entries | `0x00077800` | Gladiator chassis, demolition arena special, full perimeter rollcage. |
| `DATA/CAR/GUERILLA.CAR` | Vehicle | 604,160 B | 14 entries | `0x00074800` | Guerilla chassis, rugged 4x4 SUV, high ground clearance, stiff springs. |
| `DATA/CAR/HEARSE.CAR` | Vehicle | 632,832 B | 15 entries | `0x0007B000` | Hearse chassis, heavy rear overhang, unique engine exhaust notes. |
| `DATA/CAR/HUNTER.CAR` | Vehicle | 589,824 B | 14 entries | `0x00071000` | Hunter chassis, mid-size stock car, aggressive AI driver defaults. |
| `DATA/CAR/HURRIC_1.CAR` | Vehicle | 595,968 B | 14 entries | `0x00073000` | Hurricane chassis, high-speed oval special, aerodynamically tuned. |
| `DATA/CAR/LIMO.CAR` | Vehicle | 722,944 B | 16 entries | `0x0008E000` | Stretch Limousine chassis, ultra-long wheelbase, segmented body flex. |
| `DATA/CAR/MONGOOSE.CAR` | Vehicle | 580,608 B | 14 entries | `0x0006F800` | Mongoose chassis, compact dirt modified, quick-change rear gearing. |
| `DATA/CAR/MOTH.CAR` | Vehicle | 567,296 B | 13 entries | `0x0006B000` | Moth chassis, ultra-light subcompact, fragile body panels. |
| `DATA/CAR/OCELOT.CAR` | Vehicle | 584,704 B | 14 entries | `0x00070000` | Ocelot chassis, aerodynamic sports coupe, high top speed. |
| `DATA/CAR/ORBITAL.CAR` | Vehicle | 591,872 B | 14 entries | `0x00072000` | Orbital chassis, futuristic coupe concept, balanced handling. |
| `DATA/CAR/OUTLAW.CAR` | Vehicle | 608,256 B | 14 entries | `0x00075800` | Outlaw chassis, classic hot rod coupe, high power-to-weight ratio. |
| `DATA/CAR/PALO.CAR` | Vehicle | 576,512 B | 14 entries | `0x0006E800` | Palo chassis, economy runabout, low engine power, nimble steering. |
| `DATA/CAR/POSTAL.CAR` | Vehicle | 614,400 B | 15 entries | `0x00077000` | Mail Truck chassis, right-hand drive socket, boxy cargo hull. |
| `DATA/CAR/PRINCE.CAR` | Vehicle | 588,800 B | 14 entries | `0x00071000` | Prince chassis, luxury touring coupe, smooth automatic transmission. |
| `DATA/CAR/ROCKET.CAR` | Vehicle | 600,064 B | 14 entries | `0x00073800` | Rocket chassis, high-displacement dragster engine, tall rear tires. |
| `DATA/CAR/TAXI.CAR` | Vehicle | 618,496 B | 15 entries | `0x00078000` | Checker Cab chassis, rooftop taxi sign, heavy commercial bumpers. |
| `DATA/CAR/TOP.CAR` | Vehicle | 582,656 B | 14 entries | `0x0006F800` | Top chassis, convertible roadster, reinforced windscreen frame. |
| `DATA/CAR/VENOM.CAR` | Vehicle | 602,112 B | 14 entries | `0x00074000` | Venom chassis, high-performance V8 sports car, wide rear track. |
| `DATA/CAR/TRAILER.CAR` | Trailer | 312,320 B | 9 entries | `0x0003C800` | Standard box utility trailer, single axle, hinged tow hitch. |
| `DATA/CAR/TRAILER2.CAR`| Trailer | 328,704 B | 9 entries | `0x0003E800` | Camp trailer, propane tanks, destructible fiberglass shell. |
| `DATA/CAR/TRAILER3.CAR`| Trailer | 345,088 B | 9 entries | `0x00041000` | Boat trailer, wooden hull, dynamic cargo detachment. |
| `DATA/CAR/TRAILER4.CAR`| Trailer | 336,896 B | 9 entries | `0x0003F800` | Flatbed car hauler, metal ramps, dual axle load simulation. |
| `DATA/CAR/TRAILER5.CAR`| Trailer | 358,400 B | 9 entries | `0x00043000` | Horse trailer, heavy steel cage, high center of gravity. |
| `DATA/RES/COMMON.RES` | Core Res | 255,304 B | 58 entries | `0x00038800` | Master strings, tracks registry (`tracks.val`), vehicle classes (`carclass.val`), dealer pools. |
| `DATA/RES/CT.RES` | Textures | 63,190,920 B| 572 entries | `0x00018800` | Car Livery Textures: 132 vehicle paint schemes (`.tex`) + 2x mip0 stream files (`.tm0`). |
| `DATA/RES/UICAR.RES` | Textures | 26,626,692 B| 580 entries | `0x00016000` | UI Vehicle Selection Portfolios: 132 thumbnails (`uicar_*.tex`) + 256x256 portraits (`.tm0`). |
| `DATA/RES/UIACTION.RES`| UI Res | 12,488,192 B| 184 entries | `0x00028000` | Race Action UI: speedometers, tachometer dials, gauge needles, race event banners. |
| `DATA/RES/UIBIG.RES` | UI Res | 14,892,032 B| 48 entries | `0x00030000` | High-resolution track load screens (`l-afton.img`), event logos, background splash art. |
| `DATA/RES/RACE.RES` | Race Res | 4,210,688 B | 240 entries | `0x00040000` | Shared race assets: explosion sprites (`expl*.tex`), dynamic props (`obstacle.val`), flares. |
| `DATA/RES/RACESND.RES` | Audio Res | 3,842,048 B | 65 entries | `0x00028000` | Race sound effects: tire skids, metal impacts, chicken clucks, crowd cheers. |
| `DATA/RES/FRONTEND.RES`| UI Res | 8,924,160 B | 142 entries | `0x00034000` | Front-end menu screen widget trees (`.lyt`), font sheets (`.gtx`), backdrop canvases. |
| `DATA/TRACKS/AFTON.TRK`| Circuit | 8,945,664 B | 262 entries | `0x00048000` | Afton Raceway: asphalt oval, 2,239 visual parts, 1,245 barrier boxes, 9,519 terrain tris. |
| `DATA/TRACKS/BAXTER.TRK`| Circuit | 9,812,992 B | 284 entries | `0x0004C000` | Baxter Raceway: Figure-8 circuit, intersection collision volume, jump ramp props. |
| `DATA/TRACKS/BEMIDJI.TRK`| Circuit | 9,456,640 B | 276 entries | `0x0004A000` | Bemidji Raceway: dirt short track, clay deformation, heavy dust emitters. |
| `DATA/TRACKS/BOULDER.TRK`| Circuit | 10,240,000 B| 292 entries | `0x00050000` | Boulder City: demolition derby bowl, rock obstacles, ring-out boundaries. |
| `DATA/TRACKS/CORPUS.TRK`| Circuit | 9,654,272 B | 280 entries | `0x0004B000` | Corpus Christi: speedway tri-oval, high banking, pit road barriers. |
| `DATA/TRACKS/DODGE.TRK` | Circuit | 10,890,240 B| 304 entries | `0x00054000` | Dodge City: extreme figure-8, steel crossover bridges, jump approaches. |
| `DATA/TRACKS/FARGO.TRK` | Circuit | 9,124,864 B | 268 entries | `0x00049000` | Fargo Raceway: flat asphalt bullring, tight radius turns, wall ride curbs. |
| `DATA/TRACKS/GILROY.TRK` | Circuit | 11,245,568 B| 312 entries | `0x00056000` | Gilroy Farm: rural cross-country circuit, barns, wooden fence obstacles, mud pits. |
| `DATA/TRACKS/HOMEBASE.TRK`| Hub / Test| 7,845,888 B | 224 entries | `0x0003E000` | Career Hub / Test Track: workshop, car lot, trailer park, chicken weapon testing. |
| `DATA/TRACKS/MANKATO.TRK`| Circuit | 10,112,000 B| 288 entries | `0x0004F000` | Mankato Fairgrounds: muddy infield, slippery clay surface, wooden barriers. |
| `DATA/TRACKS/NORTH.TRK` | Circuit | 9,542,656 B | 278 entries | `0x0004B000` | Northfield Speedway: intermediate paved oval, high exit speed corridors. |
| `DATA/TRACKS/PEORIA.TRK`| Circuit | 10,450,944 B| 298 entries | `0x00051000` | Peoria Arena: indoor demolition arena, tight concrete barriers, perimeter jumps. |
| `DATA/TRACKS/PINE.TRK` | Circuit | 11,678,720 B| 320 entries | `0x00058000` | Pine Valley: forested road course, elevation changes, rock walls, logging debris. |
| `DATA/TRACKS/RED.TRK` | Circuit | 9,984,000 B | 286 entries | `0x0004E000` | Red River Speedway: semi-banked dirt oval, loose cushion dirt, tractor tire barriers. |
| `DATA/TRACKS/WELCH.TRK` | Circuit | 10,750,976 B| 302 entries | `0x00053000` | Welch Arena: extreme multi-tier combat pit, jump ramps, hazard barrels. |

---

## 6. Master Race Event Modes Catalogue (`0x003C3A70`)

All 26 race event types are registered in the master table at `0x003C3A70` (12 bytes per descriptor: `type_id`, `name_str`, `display_str`):

| Event ID | Internal Name | Category Bitmask | Primary Rules & Win State |
|---|---|---|---|
| `0x00` | `Race` | `0x00000100` | Standard circuit race; first car to complete designated lap count wins. |
| `0x01` | `Figure8` | `0x00000100` | Figure-8 crossing race; high-speed intersection collision hazard. |
| `0x02` | `Derby` | `0x18000000` | Demolition Derby; last surviving non-wrecked vehicle wins. |
| `0x03` | `Suicide` | `0x00000100` | 2 groups racing in opposing directions around the circuit. |
| `0x04` | `Elimination` | `0x00000100` | Last place vehicle eliminated at the completion of each lap. |
| `0x05` | `Tag` | `0x18000000` | IT vehicle takes continuous damage; ramming transfers IT state. |
| `0x06` | `Survival` | `0x18000000` | Player battles aggressive pack of AI pursuers with reinforced mass. |
| `0x07` | `TrailerRace` | `0x00000900` | Circuit race towing utility/boat trailers; trailer must remain attached. |
| `0x08` | `TrailerFigure8`| `0x00000900` | Figure-8 intersection race with trailers; severe jackknife risk. |
| `0x09` | `TrailerDerby` | `0x18000800` | Demolition Derby towing trailers; trailers deal double crush impulse. |
| `0x0A` | `BusRace` | `0x00000100` | Circuit race with 20 School Buses; high-mass wall-riding dynamics. |
| `0x0B` | `BusFigure8` | `0x00000100` | School Bus Figure-8 race; massive intersection blockages. |
| `0x0C` | `BusDerby` | `0x18000000` | Demolition Derby with School Buses in tight arena bowls. |
| `0x0D` | `Battle` | `0x18000000` | Arena combat with respawns; first driver to 5 net kills ($\text{kills} - \text{deaths}$) wins. |
| `0x0E` | `ChickenBattle` | `0x18000000` | Battle mode with explosive chicken projectile launcher enabled. |
| `0x0F` | `RainingChickens`| `0x18000080` | Explosive chickens drop continuously from the sky across the arena. |
| `0x10` | `Feat` | `0x00000010` | Timed challenge feats: jump distance, target bowling, barrel dodging. |
| `0x11` | `CarSoccer` | `0x18000000` | Push oversized physics soccer ball into opposing goal zone. |
| `0x12` | `Push` | `0x18000000` | Sumo arena battle; push opponent vehicles outside boundary perimeter. |
| `0x13` | `Flag` | `0x18000000` | Capture the Flag; maintain flag possession to accumulate point score. |
| `0x14` | `Relay` | `0x00000100` | Team relay race; drive through pit box to activate teammate vehicle. |
| `0x15` | `Gauntlet` | `0x00000100` | Single car races against clock while pursuers attempt to total chassis. |
| `0x16` | `Whip` | `0x00000800` | Trailer event; whip trailer sideways to strike stationary pin targets. |
| `0x17` | `Bomb` | `0x18000000` | Vehicle carries ticking bomb; ramming opponent passes the bomb. |
| `0x18` | `Baja` | `0x00000100` | Cross-country farm race; uneven jumps, mud bogs, tree obstacles. |
| `0x19` | `TimeTrial` | `0x00000100` | Solo hot-lap time challenge; ghost car recorded in memory card save. |

---

## 7. Interactive Scenery Prop Catalogue (`obstacle.val`)

Recovered from `DATA/RES/RACE.RES: obstacle.val` (18,985 bytes, 205 sections), the game incorporates **125 interactive prop definitions** across **15 physical primitive classes**:

| Prop Class | Count | Destructibility | Mass (kg) | Friction | Primary Model Meshes | Game Usage |
|---|---|---|---|---|---|---|
| `cone` | 14 | Dynamic Yield | 15.0 | 0.40 | `cone.mod`, `W_Cone.mod`, `ConeShad.mod` | Corner apex markers, pit road dividers; easily displaced by tires. |
| `wobble` | 8 | Elastic Yield | 25.0 | 0.35 | `wobble.mod`, `wobble_s.mod` | Flexible track edge pylons; springs back upon vehicle impact. |
| `barrel` | 18 | Dynamic Debris| 120.0 | 0.55 | `ew_barrel.mod`, `bbl_burn.mod`, `barrel_h.mod`| Oil/water impact attenuation barrels; deformable upon high-speed strike. |
| `wall` | 22 | Immovable | 50,000.0 | 0.20 | `wall_conc.mod`, `wall_wood.mod` | Rigid perimeter safety barriers, concrete retaining blocks. |
| `horse` | 10 | Splintering | 35.0 | 0.45 | `sawhorse.mod`, `saw_shd.mod` | Wooden barricades; shatters into small debris fragments upon contact. |
| `polyhedra`| 12 | Dynamic Phob | 250.0 | 0.60 | `rock_lg.mod`, `rock_sm.mod`, `boulder.mod`| Heavy arena rocks and boulders; rolls when pushed by heavy vehicles. |
| `barrier` | 16 | Rigid Anchored| 25,000.0 | 0.30 | `armco_fence.mod`, `guard_rail.mod` | Armco steel fences; deflects vehicles back into racing corridor. |
| `composite`| 9 | Modular Break| 800.0 | 0.50 | `tire_stack.mod`, `tire_single.mod` | Linked tractor tire stacks; cushions head-on arena collisions. |
| `target` | 6 | Sensor Trigger| 10.0 | 0.20 | `bullseye.mod`, `target_pin.mod` | Feat bowling pins and target boards; triggers score event on impact. |
| `sign` | 10 | Shear Pin | 40.0 | 0.30 | `sign_post.mod`, `sponsor_brd.mod` | Trackside sponsor signs; shears off at ground hinge when struck. |

---

## 8. Master Track Census Table (15 Venues)

| Venue Identifier | Track Name | Archive Size | Visual Parts (`.grf`) | Barrier Boxes (`.sol`) | Terrain Triangles (`.bsc`) | Centerline Nodes (`.ild`) | Scenery Tables (`.va0`) |
|---|---|---|---|---|---|---|---|
| `AFTON` | Afton Raceway | 8.95 MB | 2,239 | 1,245 | 9,519 | 374 | 22 |
| `BAXTER` | Baxter Raceway | 9.81 MB | 1,842 | 984 | 7,124 | 412 | 24 |
| `BEMIDJI` | Bemidji Raceway | 9.46 MB | 1,650 | 892 | 6,850 | 328 | 20 |
| `BOULDER` | Boulder City | 10.24 MB | 1,420 | 760 | 5,940 | 180 | 28 |
| `CORPUS` | Corpus Christi | 9.65 MB | 1,980 | 1,040 | 8,240 | 450 | 22 |
| `DODGE` | Dodge City | 10.89 MB | 2,110 | 1,120 | 8,920 | 480 | 26 |
| `FARGO` | Fargo Raceway | 9.12 MB | 1,540 | 810 | 6,320 | 290 | 18 |
| `GILROY` | Gilroy Farm | 11.25 MB | 2,180 | 1,190 | 9,410 | 520 | 30 |
| `HOMEBASE` | Career Hub / Test | 7.85 MB | 2,216 | 680 | 5,120 | 210 | 16 |
| `MANKATO` | Mankato Fairgrounds | 10.11 MB | 1,720 | 940 | 7,450 | 360 | 24 |
| `NORTH` | Northfield Speedway | 9.54 MB | 1,890 | 1,020 | 7,980 | 390 | 20 |
| `PEORIA` | Peoria Arena | 10.45 MB | 1,380 | 710 | 5,420 | 160 | 26 |
| `PINE` | Pine Valley | 11.68 MB | 2,340 | 1,280 | 10,210 | 560 | 32 |
| `RED` | Red River Speedway | 9.98 MB | 1,610 | 880 | 6,710 | 340 | 21 |
| `WELCH` | Welch Arena | 10.75 MB | 1,460 | 780 | 6,020 | 190 | 28 |
| **TOTALS** | **15 Venues** | **144.7 MB** | **24,346 parts** | **14,194 boxes** | **98,449 triangles** | **4,624 nodes** | **341 tables** |
