# Engine & Subsystem Documentation Index (`SLUS_209.10`)

This index maps every reverse-engineered subsystem in *Test Drive: Eve of Destruction* (`SLUS_209.10`, North American PS2 release) to its documentation in `docs/`, its runtime HLE/patch implementation in `src/` and `runtime/`, and its canonical symbol addresses in [`config/symbols.txt`](../config/symbols.txt).

---

## 1. Symbol Database (`config/symbols.txt`)

[`config/symbols.txt`](../config/symbols.txt) is the single source of truth for function and global-variable annotations across the binary:

```text
<hex_addr> <SymbolName> [V|I]  # <description> -> docs/<file>.md
```

- **`[V]` (Verified)**: Confirmed via retail debug strings, Sony PS2SDK / newlib signatures, or runtime-traced HLE contracts.
- **`[I]` (Inferred)**: Deduced from call-graph structure, struct layouts, and caller/callee register contracts.
- **Generator integration (`tools/recomp.py`)**: Emits symbol names, `[V]`/`[I]` tags, and notes in `gen/rec_*.c` function header comments and inline `/* SymbolName */` comments on `jal` call sites.
- **Disassembler integration (`tools/dis.py`)**: Prints symbol headers (`SymbolName [V]: ; note`) at entry points and resolves function and global operand names inline.

---

## 2. Subsystems & Documentation Map

### System, Entry, Frame Loop & Memory (`0x00100000`–`0x0012FFFF`, `0x00330000`–`0x003BC290`)
- **Entry & Frame Synchronization**: `_start` (`0x00100008`), `main` (`0x00100138`), `FrameWaitVblank` (`0x0035C460`), `FrameWaitDrawDone` (`0x0035C528`).
- **Allocator & Diagnostics**: `MemAlloc` (`0x0012DDB8`), `MemFree` (`0x0012DB78`), `MemPoolGetAvailable` (`0x0012DEC8`), `Panic` (`0x00331DF0`), `DebugPrintf` (`0x00331E48`). Checked at runtime by `src/hle_heapcheck.cpp` (`EOD_HEAP_CHECK=1`).
- **StreamFS & Archives**: `StrmMountArchive` (`0x0033D408`), `StrmFindFile` (`0x0033D4F8`), `StrmOpenFile` (`0x0033D8E8`), `ResLoadAsset` (`0x001323E8`).
- **Docs**: [`DEVELOPER_HANDOFF.md`](DEVELOPER_HANDOFF.md), [`STATUS.md`](STATUS.md), [`MACOS.md`](MACOS.md), [`WORKER_SHUTDOWN.md`](WORKER_SHUTDOWN.md), [`TIMING_TRACE.md`](TIMING_TRACE.md).

### Career Mode, AutoShop, Garage & Unlocks (`0x002C0000`–`0x002EFFFF`, `0x003698E8`–`0x003702F0`)
- **Functions**: `AutoShopSetAvailable` (`0x002E35F0`), `AutoShopUpdateSelection` (`0x003702F0`), `HomebaseBuildMenu` (`0x003698E8`), `CareerIsEveRankEligible` (`0x002E5200`), `CareerLoadChallengesVal` (`0x002E5758`), `CareerApplyPostEveRank` (`0x002E6C78`), `CareerGetPlayerRank` (`0x002E0F10`), `CareerCalcTradeInDepreciation` (`0x002E3BC8`), `CareerGetOwnedCarResaleValue` (`0x002E3E70`), `CareerFreeRoamRun` (`0x002D58E8`), `CareerIsFreeRoamActive` (`0x001741C0`).
- **Globals**: `g_CareerState` (`0x00496E80`: `+0x10` cash, `+0x1C8` rank, `+0x2C4` garage, `+0x3D4` 5-slot dealer stock), `g_CareerChallengesTable` (`0x003CD910`), `g_CareerChallengePromptState` (`0x003CDA24`), `g_CareerActiveChallengerPtr` (`0x003CDA78`).
- **Docs**: [`CAREER_UNLOCKS.md`](CAREER_UNLOCKS.md), [`GAMEPLAY_MODS_RE.md`](GAMEPLAY_MODS_RE.md), [`OPTIONAL_GAMEPLAY_MODS.md`](OPTIONAL_GAMEPLAY_MODS.md).

### Battle Mode, Chicken Weapons & AI Bots (`0x0010AFA0`–`0x0012C158`, `0x001515B8`–`0x0017EA10`, `0x001A2D20`)
- **Battle Lifecycle**: `EveLoadContests` (`0x001244A8`), `RosterBuildEventParticipants` (`0x0012C158`), `TrackLookupBattleSection` (`0x001515B8`), `BattleCreate` (`0x00166C38`), `BattleInitParticipants` (`0x00167340`), `BattleRespawnCar` (`0x001675C0`), `BattlePlaceInitialCars` (`0x00167C70`), `BattleUpdate` (`0x00167D58`), `AICarUpdate` (`0x001A2D20`).
- **Chicken Weapons**: `WeaponRegistryRegister` (`0x0017DE30`), `WeaponRegistryQuery` (`0x0017DE78`), `ChickenWeaponAlloc` (`0x0017DFA8`), `ChickenWeaponTrigger` (`0x0017E000`), `ChickenWeaponStartCooldown` (`0x0017E4D8`), `ChickenWeaponUpdate` (`0x0017E5C8`), `ChickenWeaponDetonate` (`0x0017E718`), `ChickenExplosionApplyShockwave` (`0x0017DA58`), `ChickenWeaponCreateForCar` (`0x00227EE8`).
- **Globals**: `g_EveContestTable` (`0x003C61A8`), `g_PrimaryCourseCache` (`0x003CD190`), `g_WeaponRegistry` (`0x003CE3F8`), `g_BattleMasterPtr` (`0x003CF194`).
- **Docs**: [`BATTLE_RECOVERY.md`](BATTLE_RECOVERY.md), [`BATTLE_NAVIGATION.md`](BATTLE_NAVIGATION.md), [`GAMEPLAY_MODS_RE.md`](GAMEPLAY_MODS_RE.md).

### Cars, Physics, Deformation, Recovery & Debris Phobs (`0x00180000`–`0x001EFFFF`)
- **Damage & Deformation**: `CarResetForRespawn` (`0x00188F20`), `CarCanTakeDamage` (`0x0018AAD0`), `CarSetScalarDamage` (`0x0018AD20`), `CarSetupDamageAndPanels` (`0x0018AFE0`), `CarApplyCollisionDamage` (`0x0018CA98`), `CarUpdateDetachablePanels` (`0x0018CFE8`), `CarInitDetachablePanelTable` (`0x0018DBF8`), `DeformResetMeshVertices` (`0x001DF9A0`).
- **Driving & Recovery**: `HumanDriverUpdate` (`0x0018F2A8`), `CarSetSteering` (`0x0018AA80`), `CarRecoveryCountdownUpdate` (`0x001B8370`).
- **Detached Debris Phob Pools**: `DetachedWheelPoolReset` (`0x001B2868`), `DetachedWheelPoolFreeAll` (`0x001B2DD8`), `DetachedWheelPhobUpdate` (`0x001B2E78`), `DetachedWheelSpawn` (`0x001B3068`), `DetachedPanelPoolInit` (`0x001E0DC8`), `DetachedPanelSpawn` (`0x001E1E88`), `DetachedPanelPhobUpdate` (`0x001E25C8`), `DetachedPanelPoolClear` (`0x001E29C0`).
- **Globals**: `g_DetachablePanelSpecTable` (`0x0044AE68`), `g_DetachedWheelPhobPool` (`0x00484E68`), `g_RecoveryConfigFlag` (`0x00485098`), `g_DetachedPanelPhobPool` (`0x00494D60`).
- **Docs**: [`MANUAL_RECOVERY.md`](MANUAL_RECOVERY.md), [`NASCAR_A2_RECOVERY.md`](NASCAR_A2_RECOVERY.md), [`NASCAR_CUP_WHEELS.md`](NASCAR_CUP_WHEELS.md), [`CUSTOM_VEHICLE_PIPELINE.md`](CUSTOM_VEHICLE_PIPELINE.md), [`ASSET_PIPELINE_RECOVERY.md`](ASSET_PIPELINE_RECOVERY.md).

### Camera, Widescreen, Draw Distance, LOD & Texture Cache (`0x00211318`–`0x00220788`, `0x0033ECB8`–`0x003653F8`)
- **Camera & Frustum**: `DisplaySetAspectMode` (`0x0034D328`), `ProjectionSetPerspective` (`0x0034E530`), `FrustumTestSphere` (`0x0034E308`), `CameraBuildViewportProjection` (`0x00212B58`), `FogConfigureRange` (`0x0033ECB8`), `GsFogWriteTables` (`0x003653F8`), `HudDrawLeftRaceRows` (`0x00119888`), `HudDrawRightSpeedRows` (`0x00119F40`).
- **LOD & Model Submission**: `DrawDistanceApplyOption` (`0x00212808`), `VisualCarInit` (`0x00217DF0`), `VisualCarLoadLodTable` (`0x0021ABF8`), `VisualCarSelectLodIndex` (`0x0021C6E0`), `VisualCarBatchSelectAndSubmit` (`0x00218E68`), `VisualCarCloneShared` (`0x0021C130`), `WheelSelectSlowOrFastModel` (`0x00220788`), `ModelLoadFromArchive` (`0x0034CEF0`), `ModelSubmitVu1Packets` (`0x0034BDA8`).
- **Texture Streaming (`mip0` / `.tm0`)**: `TextureSubsystemInit` (`0x00352D28`), `TextureMip0PoolInit` (`0x003543D0`), `TextureLoadHiResTm0` (`0x003545B8`), `TextureRegisterHiResMip0` (`0x003537F0`), `TextureMip0PoolResize` (`0x00354838`).
- **Globals**: `g_ActiveViewportCount` (`0x003D1A74`), `g_VisualCarList` (`0x003D1C90`), `g_DisplayAspectXScale` (`0x003DD658`), `g_ViewCountLodFactors` (`0x004062A8`), `g_DrawDistanceFar` (`0x00495D58`), `g_TextureMip0CacheState` (`0x004A9830`), `g_CameraWorldPos` (`0x004CD31C`), `g_ProjectionNearFar` (`0x004CD370`).
- **Docs**: [`LOD_CULLING_INVESTIGATION.md`](LOD_CULLING_INVESTIGATION.md), [`LOD_RUNTIME.md`](LOD_RUNTIME.md), [`DETAIL_PRESETS.md`](DETAIL_PRESETS.md), [`NATIVE_CAMERA_ASPECT.md`](NATIVE_CAMERA_ASPECT.md), [`HUD_EDGE_ANCHORS.md`](HUD_EDGE_ANCHORS.md), [`TEXTURE_FORMAT_NOTES.md`](TEXTURE_FORMAT_NOTES.md).

### GS Renderer (Vulkan & Direct3D 11)
- **Docs**: [`GS_ALPHA_ORDER.md`](GS_ALPHA_ORDER.md), [`GS_CPU_WRITE_PRESERVATION.md`](GS_CPU_WRITE_PRESERVATION.md), [`GS_COLD_PACKET_REPLAY.md`](GS_COLD_PACKET_REPLAY.md), [`COLLISION_PERFORMANCE.md`](COLLISION_PERFORMANCE.md), [`VULKAN_SYNC_DIAGNOSTICS.md`](VULKAN_SYNC_DIAGNOSTICS.md), [`RECOMP_RENDERER_RECON.md`](RECOMP_RENDERER_RECON.md), [`RENDERER_REFERENCES.md`](RENDERER_REFERENCES.md).

### Input, Audio, Replay & Netplay (`0x0012F3C8`–`0x0012F4F0`, `0x00176698`–`0x00177718`, `0x00191548`, `0x0038F868`–`0x003919A0`)
- **Simulation & Determinism**: `WorldSimulationStep` (`0x00177718`), `RaceSessionInitPhase` (`0x00176698`), `RaceSessionSeedRng` (`0x00176918`), `ReplayRecordHumanTick` (`0x00191548`), `RngNextFloat01` (`0x0012F3C8`), `RngNextRange` (`0x0012F470`), `RngNextSignedFloat` (`0x0012F4F0`).
- **Pads & Memory Card**: `scePadInit` (`0x0038F868`), `scePadPortOpen` (`0x0038FB40`), `scePadRead` (`0x0038FF40`), `scePadGetState` (`0x00390228`), `scePadSetMainMode` (`0x003906A0`), `scePadSetActDirect` (`0x00390B60`), `sceMcInit` (`0x003919A0`).
- **Docs**: [`INPUT_CONTINUATION.md`](INPUT_CONTINUATION.md), [`KEYBOARD_CONTEXT.md`](KEYBOARD_CONTEXT.md), [`AUDIO_FORMAT_NOTES.md`](AUDIO_FORMAT_NOTES.md).
