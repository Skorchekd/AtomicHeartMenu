# Atomic Heart Menu - Project & SDK Guide

This document explains **how the project is built**, **how the dumped SDK fits in**,
and **how to add new cheats** using the SDK. For build/inject/run steps see
[`README.md`](README.md).

> Single-player only. Atomic Heart has no multiplayer - these modifications affect
> no other players.

> Maintenance rule: after every code or behavior change, update this markdown
> (and `README.md` when user-facing controls/build/run behavior changes) in the
> same pass so the repo does not accumulate stale feature notes.

---

## 1. What this is

An **internal mod menu**: a DLL (`AtomicHeartMenu.dll`) you inject into the running
game. Once inside, it:

1. **Hooks the DirectX 12 swapchain** and draws a [Dear ImGui](https://github.com/ocornut/imgui)
   overlay (toggle with **INSERT**).
2. **Reads/writes the game's objects** through a small Unreal Engine 4 runtime SDK,
   and calls game functions via `UObject::ProcessEvent`.

Hook enable/disable is batched with MinHook's queue so all three DX12 hooks apply
in one suspend/resume pass. SDK resolution and feature prewarm run on the worker
thread. `FindObject()` uses static dumped `GObjects` index hints for every known
function, weapon, ammo, and streaming helper object before falling back to a scan.
Weapon/ammo assets stay lazy instead of being bulk-prewarmed at injection; feature
commands use a no-scan lookup that falls through to a background short-name object
index when a dumped asset index is unstable at runtime.
Feature tick avoids reflected calls unless a feature needs them: location reads
are gated by coordinates/fly, infinite ammo uses throttled polling/direct field
writes, `Give all` is processed incrementally, and debug object dumping runs on a
background thread.

Target build: **Unreal Engine 4.27.2**, `AtomicHeart-Win64-Shipping.exe`
(64-bit, image base `0x140000000`).

---

## 2. Project layout

```
AtomicHeartMenu/
├── README.md                 Build / inject / run instructions
├── PROJECT_AND_SDK.md        <- this file
├── CMakeLists.txt            Builds the DLL + injector (static CRT, /EHa, x64)
├── build.bat                 One-click build -> bin\AtomicHeartMenu.dll
│
├── src/                      The menu's own source
│   ├── dllmain.cpp           Entry: worker thread, hook install, INSERT/END handling
│   ├── core/
│   │   ├── globals.h         Shared atomics (running / menuOpen / sdkReady) + module info
│   │   ├── log.{h,cpp}       Live console + AtomicHeartMenu.log next to the game exe
│   │   └── memory.{h,cpp}    Mem::IsReadable - pointer-safety guard used everywhere
│   ├── hooks/
│   │   ├── dx12_hook.{h,cpp} Present/ResizeBuffers/ExecuteCommandLists hooks + ImGui DX12
│   │   └── wndproc_hook.{h,cpp}  Window subclass: feeds input to ImGui, INSERT toggle
│   ├── menu/menu.{h,cpp}     The ImGui window (themed: Player / Weapons / AI·Squad / World / Visuals / Render / Misc / Debug tabs)
│   ├── features/features.{h,cpp}  Per-frame cheat logic (god mode, fly, one-hit, …)
│   └── sdk/                  *Minimal* hand-written UE4 runtime SDK (see §4)
│       ├── ue4.{h,cpp}       FName/UObject/GObjects/GWorld + ProcessEvent + lookups
│       ├── scanner.{h,cpp}   AOB scanner (fallback; unused now that offsets are static)
│       └── offsets.h         *** ALL build-specific offsets live here ***
│
├── tools/injector.cpp        Minimal LoadLibrary injector -> bin\injector.exe
├── deps/imgui, deps/minhook  Vendored dependencies
│
└── dumped-sdk/               *** The full Dumper-7 output for this build (see §3) ***
    ├── CppSDK/               Generated C++ SDK (every game class/struct/function)
    ├── Mappings/             .usmap (for UE tooling, e.g. UAssetGUI/FModel)
    ├── IDAMappings/          Symbol names for IDA/Ghidra
    ├── Dumpspace/            Machine-readable dump (Dumpspace format)
    ├── GObjects-Dump.txt              Every live UObject (index + full name)
    └── GObjects-Dump-WithProperties.txt  …same, plus property layouts
```

---

## 3. The dumped SDK (`dumped-sdk/`)

> **Not in this repo.** The `dumped-sdk/` folder is intentionally not committed (it is
> large, derived from the game, and only needed as reference for adding new features).
> The menu builds and runs without it because the offsets it relies on are already
> hard-coded in `src/sdk/offsets.h`. To get it back, run **Dumper-7** against your own
> copy of the game; it recreates the exact layout described below.

This is the raw output of **[Dumper-7](https://github.com/Encryqed/Dumper-7)**,
captured by injecting it into the running game. It is the **source of truth** for
every offset, class, and function in this exact build.

Folder name encodes the engine/build: `4.27.2-18319896+++UE4+Release-4.27-AtomicHeart`.

### What's inside and when to use it

| Path | Use it to… |
|---|---|
| `dumped-sdk/CppSDK/SDK/AtomicHeart_classes.hpp` | Find Atomic Heart's gameplay classes (the player, weapons, enemies, components) and their member offsets |
| `dumped-sdk/CppSDK/SDK/Engine_classes.hpp` | Find engine classes (UWorld, APlayerController, ACharacter, UCharacterMovementComponent) |
| `dumped-sdk/CppSDK/SDK/GameplayAbilities_*.hpp` | The GAS types - `FGameplayAttributeData`, attribute sets |
| `dumped-sdk/CppSDK/SDK/Basic.hpp` | The **global offsets**: `GObjects`, `GNames`, `GWorld`, `ProcessEventIdx` |
| `dumped-sdk/GObjects-Dump.txt` | Grep the live object graph to discover names/classes at runtime |

Runtime `GetFullName()` includes package paths such as
`Function /Script/Engine.Actor.K2_GetActorLocation`. Use those `/Script/...`
paths in `src/sdk/offsets.h` function-name constants; omitting `/Script/` makes
`FindFunction()` miss otherwise-valid functions.

### The global offsets (from `Basic.hpp`)

These are RVAs relative to image base `0x140000000`:

| Global | RVA |
|---|---|
| `GObjects` (FUObjectArray) | `0x06EB7BD0` |
| `GNames` (FNamePool) | `0x070ECBC0` |
| `GWorld` (UWorld**) | `0x070E93C0` |
| `ProcessEvent` vtable index | `0x44` |

They're already plugged into `src/sdk/offsets.h` (`USE_STATIC_OFFSETS = true`).

> ⚠️ These are valid for **this build only**. After a game patch, re-inject Dumper-7
> (it's built at `..\Dumper-7\out\build\vs2022\bin\Release\Dumper-7.dll`), re-read
> `Basic.hpp`, and update `offsets.h`.

---

## 4. Two SDKs - why both exist

There are deliberately **two** SDKs in this repo:

- **`src/sdk/` (hand-written, compiled in).** A ~tiny UE4 runtime: just `FName`,
  `UObject`, `GObjects` iteration, `GWorld`, and `ProcessEvent`. It is intentionally
  minimal and *safe* - every game-memory read goes through `Mem::IsReadable`. This is
  what the DLL actually compiles and runs.

- **`dumped-sdk/CppSDK/` (generated, reference).** The complete typed SDK - thousands
  of classes. **Not compiled** into the DLL (it's huge and would slow builds). It's the
  reference you read to discover the offsets/functions you then hard-code into
  `src/sdk/offsets.h`.

This keeps the shipping DLL small and crash-resistant, while giving you the full SDK
to mine for new features. (If you ever want fully-typed access, you *can* add
`dumped-sdk/CppSDK/SDK` to the include path and `#include "SDK.hpp"` - but expect a
much slower build and some cleanup.)

---

## 5. How a cheat actually works

Everything funnels through two engine primitives:

1. **Reach the player.** `World → OwningGameInstance → LocalPlayers[0] →
   PlayerController → AcknowledgedPawn`. The pawn is an **`AAHBaseCharacter`**.
   (`UE::GetLocalPawn()` does this walk, guarded.)

2. **Read/write or call.** Either poke a member at a known offset, or invoke a
   `UFunction` with `pawn->ProcessEvent(fn, &params)`.

Atomic Heart is a **GAS (Gameplay Ability System) game**, so health/stamina/energy
are not plain floats - they live in `UAHBaseCharacterAttributeSet` (pointer at
`AAHBaseCharacter + 0x568`) as `FGameplayAttributeData` (`{ BaseValue@0x08,
CurrentValue@0x0C }`). That's why "god mode" sets `IncomingDamageMultiplier = 0`
rather than freezing a health float.

### Wired offsets (`src/sdk/offsets.h`, `namespace Offsets::AH`)

| What | Offset | Used by |
|---|---|---|
| `ACharacter::CharacterMovement` | `0x2A8` | fly, speed |
| `…Movement::MovementMode` | `0x188` (5 = Flying) | fly |
| `…Movement::MaxWalkSpeed` | `0x1AC` | speed |
| `…Movement::MaxFlySpeed` | `0x1B8` | fly speed |
| `AAHBaseCharacter::AttributeSet` | `0x568` | all resource cheats |
| `AAHBaseCharacter::bIsDead` | `0x5E8` | god mode |
| AttributeSet `Health / MaxHealth` | `0x48 / 0x38` | god mode |
| AttributeSet `Stamina / MaxStamina` | `0xA8 / 0x98` | infinite stamina |
| AttributeSet `Energy / MaxEnergy` | `0xF8 / 0xE8` | infinite energy |
| AttributeSet `Air / MaxAir` | `0xD8 / 0xC8` | infinite air |
| AttributeSet `IncomingDamageMultiplier` | `0x178` (→0) | god mode |
| AttributeSet `InstigatedDamageMultiplier` | `0x188` (→big) | one-hit kill |
| `ABaseWeapon::AmmoSize / StartAmmoCount` | `0x5D8 / 0x5DC` | current weapon ammo top-up |
| `AAHPlayerCharacter::InventoryPlayer` | `0x1788` | reserve ammo refill |
| `UAHInventoryPlayer::AmmoCount / InfiniteAmmoCount` | `0x424 / 0x4A8` | reserve ammo capacity/infinite gate |
| `ARangeWeapon::Bullet / Barrel` | `0x9B8 / 0x9C0` | EBBarrel current ammo top-up |
| `UEBBarrel::Ammo / CycleAmmoCount / ChamberedBullet` | `0x538 / 0x548 / 0x560` | loaded ammo/chamber top-up |

Continuous toggle features capture the original direct fields they change on
enable and restore them on disable. Combat multipliers also sanitize stale saved
cheat values while disabled: incoming damage `0` and outgoing damage `>=900`
are reset to normal multiplier `1.0`. One-shot actions such as teleport, give
weapon, and **Refill ammo now** intentionally perform their action once and do
not own restore state.

### Feature status

Current AI follow note: `DriveFollow` is layered. It first uses Atomic Heart's
`AAHAIController` blackboard escort setters, then falls back to SDK-verified
`AIModule.AIController.MoveToActor` only when the controller is readable, owns the
AI pawn (`AController::Pawn 0x270` / `APawn::Controller 0x278`), and has a readable
`PathFollowingComponent` (`AAIController 0x2F8`). A small guarded
`Pawn.AddMovementInput` nudge covers scripted quest NPCs whose behavior tree only
turns toward the player. The public Follow command now adds successful targets to
the managed squad so the pump keeps driving them, and the default stop radius is 1m.

| Feature | Mechanism | Status |
|---|---|---|
| God mode | `IncomingDamageMultiplier=0` + Health topped + `bIsDead=false`; disabling restores captured incoming-damage and health attributes and normalizes stale saved `0` multipliers back to `1.0` | ✅ |
| One-hit kill | `InstigatedDamageMultiplier=1000`; disabling restores captured outgoing-damage multiplier and normalizes stale saved high multipliers back to `1.0` | ✅ |
| Infinite stamina/energy/air | attribute `Current=Max`/frame; disabling restores captured resource attributes | ✅ |
| Fly | `MOVE_Flying` + camera-relative `K2_SetActorLocation` free-fly; `W/A/S/D`, `Space` up, `Shift` down; throttled `StreamingUtils::InvalidateStreaming` + `AHWorldStreamingSubsystem::EnableLevelStreaming(true)` while flying; disabling restores captured movement mode/fly speed | ✅ |
| Noclip | shares the Fly free-fly movement/mode/fly-speed writes (`freeFly = flyHack \|\| noclip`) and additionally calls `Engine.Actor.SetActorEnableCollision(false)` on the pawn so it passes through geometry; disabling re-enables collision and restores the captured movement mode/fly speed | ✅ |
| Speed | `MaxWalkSpeed` / `MaxFlySpeed = 600×Movement slider`; disabling restores captured walk speed | ✅ |
| Coordinates / Save / Teleport | `K2_GetActorLocation` / `K2_SetActorLocation` | ✅ |
| Enemy ESP + World AI commands | a worker-thread pass over the **loaded levels' actor lists** (`UWorld::PersistentLevel` + `UWorld::Levels[]` → `ULevel::Actors`, `CollectLevelActors`) - a few thousand live actors, not ~360k UObjects - filters to `AHAICharacter` with a per-rebuild class-pointer cache. This is effectively **instant** (~16 ms measured) and never hangs or delays a command; the whole list is rebuilt each refresh so new spawns appear and dead actors drop out. Feeds both the ImGui ESP overlay and every World→Enemy AI command. A command pressed before the cache is populated is remembered and auto-fired once enemies are found (`DrainDeferredAiCommand`, render thread). **Crash-safety:** a cached actor can go stale between rebuild and use; calling a type-specific UFunction on it dispatches into game code where UE4's exception handler beats our `catch(...)`, so `AiUsable()` re-validates `IsReadable + IsA(AHAICharacter)` immediately before *every* AI `ProcessEvent`. The commands drive the **AHAICharacter-level functions** (`SetIsPassive`/`SetTargetEnemy`/`SetTargetAlly`/`SwitchTeamToMatchCharacterAttitude`/`Suicide`) **plus the AHAIController's own blackboard setters** (`SetBlackboardFollowLocation`/`SetFollowLocationSpeed`/`SetBlackboardTargetAlly`/`SetBlackboardTargetEnemy`/`SetBlackboardIsAggressive`). The AI is behaviour-tree driven, so its follow/attack come from those blackboard keys, **not** from a raw move request - a plain `MoveToActor` is overridden by the BT on its next tick. Each blackboard call is gated by `ControllerBlackboardReady()` (the controller's `Blackboard` component pointer must be readable) and runs on the game-thread pump, which is what makes it crash-safe. The **dangerous native nav/BT calls** (`AAIController::MoveToActor` base pathfollowing / `K2_SetFocus` / `Start|Stop|Pause|ResumeBehaviorTree`) stay removed - those faulted on a controller in a bad state and crashed "Fight each other" on a second press. **Freeze** = pure guarded write to `AActor::CustomTimeDilation≈0` (no `ProcessEvent`), auto-restored to `1.0` on toggle-off. **Bodyguards** (existing AI; `Bodyguard mode` toggle and the lighter `Follow me`) = put on your team (`SwitchTeamToMatchCharacterAttitude(player, Friendly)` + `SetTargetAlly(player)`). **Attack** reuses the *exact* `InjectAttack` pipeline that `Fight each other` uses (which is reliable): when a threat is within `kGuardEngageM` (40 m) **of you or of the guard itself**, the guard is run through `InjectAttack(guard, threat, teamRef=player, Friendly)` - raw aggro target + aggressive gates + the game's `SetTargetEnemy` + the team split + native `SetCharacterAggressive`, plus the controller's `SetBlackboardTargetEnemy`/`SetBlackboardIsAggressive`. A threat far from both is ignored so guards defend *you* instead of running off. **Follow** is a consistent **hard leash** (`DriveFollow`): every pump it re-asserts `SetBlackboardTargetAlly` + `SetFollowLocationSpeed` and refreshes `SetBlackboardFollowLocation` to your live position, and if the guard ever drifts past `kFollowSnapM` (8 m) - BT won't path, nav hole, you sprinted off - it `K2_TeleportTo`s right behind you; an **absolute** `kFollowHardSnapM` (22 m) leash snaps even mid-fight so a guard chasing something can never be stranded across the level. The recruited-guard pass drives up to `kAiGuardPerTick` (32) guards **per pump** (not the old rotating 10), so *every* guard gets its follow + threat refresh each ~200 ms - that, plus the 8 m leash, is what fixed the old "rarely follows / drops protection". Spawned allies (`DriveSpawnedAllies`) are driven the exact same way, with no toggle. **Fight each other**: all robots share one team (treat each other as allies, so a `SetTargetEnemy` on a teammate is ignored and they fall back to the player) - so we split them into two opposing teams, alternating AIs onto the player's side via `SwitchTeamToMatchCharacterAttitude(player, Friendly)`, leaving the rest on the robot team, then cross-team `SetTargetEnemy` → a real robot-vs-robot brawl (and the player-side half stops attacking you). **Spawn (streamed)** - every spawn request (model-dropdown pick, clone-nearest, or saved character) lands in a **spawn queue** that the AI pump drains **one per `kSpawnIntervalMs` (300 ms)** on the GAME THREAD via the shared `SpawnAndRegisterAlly`. Spawning is structural actor construction that crashes from the render-thread Present hook and hitches if several run in one game-thread borrow, so streaming one-at-a-time is what fixed the "spawn freezes the game" report - press the button a few times for a squad and they trickle in smoothly. **Model dropdown** (`AiSpawnModelName/Count` → `g_liveModels`, rebuilt ~1/sec): the deduped set of enemy **classes currently loaded** in the level. Every entry is therefore a live, configured class - so a spawn can never use a bare native template (that was the original crash). **Saved-character DB**: `AiSaveNearestCharacter` records the nearest live enemy's runtime class path to `AtomicHeartMenu_bodyguards.json`; `AiSpawnSavedCharacter` re-resolves it with `FindObject` on the game thread (only if that type is loaded). **Team / release (godmode-leak fix)**: team reads/writes go through the **character's own `Get/SetGenericTeamId`** (the engine `IGenericTeamAgentInterface`), which refreshes the attitude/perception solver - a raw controller team-byte poke did NOT, so released guards used to stay Friendly to you = unkillable. `ApplyAiRelease` now force-switches each unit **Hostile to the player** via `SwitchTeamToMatchCharacterAttitude(player, Hostile)` (the proven inverse of the friendly conversion) so a released guard is *always* killable again; `ReleaseNearbyInjected` releases every tracked unit, including ones that wandered out of radius. **Invincible allies** (`aiInvincibleAllies`, default on) tops spawned allies + recruited guards to full health each pump (raw guarded write, no restore bookkeeping) so your squad actually survives. All exposed in the **AI / Squad tab** (model dropdown + clone-nearest, save/list/delete/spawn saved characters, invincible/bodyguard/follow toggles, stand-down, release, fight/kill/passive/freeze, kill-all/launch-all). **Kill all / Launch all** queue the whole cached enemy list (`AiQueueKillAll` / `AiQueueLaunchAll` → `Suicide` / `LaunchCharacter` skyward). ESP world-to-screen uses the cached `APlayerCameraManager` POV; ESP can also draw a translucent **filled "chams" box** (through walls, alpha-controlled) since the overlay renders in our Present hook | ✅ |
| Max weapon upgrades | `BaseWeapon.FullUpgrade` on the current weapon (`GetCurrentWeapon` → `ProcessEvent`) | ✅ |
| Bullet time (matrix) | `SetGlobalTimeDilation(scale)` slows the world while the player's `AActor::CustomTimeDilation = 1/scale` keeps the player at real-time → player moves many× faster than everything. Owns global dilation while active (the plain time-dilation feature yields) | ✅ |
| Player scale (giant/tiny) | `Engine.Actor.SetActorScale3D` on the pawn, applied on change, restored to 1.0 on disable | ✅ |
| ProcessEvent hook (game-thread dispatch) | hooks `UObject::ProcessEvent` (vtable idx 0x44, shared by all classes) via MinHook, **lazily installed on first Spawn press**. The detour is a couple of instructions when idle; when a task queue is pending it borrows the first non-render thread it sees (the game thread, captured by excluding the Present/render thread id set in `Features::Tick`) and drains the queue there - reentrancy-guarded with a `thread_local`. This is how structural ops (SpawnActor) run safely on the game thread. Removed by `MH_Uninitialize` on eject. **Smoothness:** each safe callsite drains at most `kMaxTasksPerBorrow` (3) tasks, so a pile-up never does all the heavy work in one frame; and the "is this an AI/BT object?" safe-callsite test (`PeDispatchingOnAi`) is now memoised **per-UClass** (thread_local) instead of doing a `GetName()` string alloc + 8 substring scans on every pending-work dispatch - that overhead was a real stutter source while AI features were active | ✅ |
| Custom FOV | writes the pawn's active camera component `FieldOfView` (`AAHBaseCharacter::FPCamera 0x5B0` / `TPCamera 0x5B8` → `UCameraComponent::FieldOfView 0x288`) every frame so it persists into the next game tick; captures the original on enable and restores on disable. Slider 60-170° in the Visuals tab. (`APlayerCameraManager::SetFOV`/`LockedFOV` are not reflected in this build, so the component write is the reliable path.) | ✅ |
| Infinite ammo | fills known `DA_Item*Ammo` assets through `AHInventory.AddItemsToInventory`, raises `UAHInventoryPlayer::InfiniteAmmoCount`, enables the inventory overweight bypass, and tops the current weapon/EBBarrel loaded ammo; disabling restores captured inventory ammo-gate fields, disables the overweight bypass, and restores current weapon/EBBarrel fields. Manual **Refill ammo now** forces the same path and logs before/after counts without taking over toggle restore state | ✅ |
| Give weapon | dropdown resolves `DA_Item_*` weapon data assets and calls `AAHPlayerCharacter::InstantTakeWeapon`; selected grant also calls `EquipWeaponByDataAsset`, give-all grants every listed base weapon. Kalash is labelled `Kalash Rifle / AK-47` | ✅ |
| Misc puzzle bypass | resolves `DebugSubsystem_0` and calls `DebugSubsystem.SetInstantPuzzleResolve`, `InstantLockUnlock`, and `WinQTE`; includes an auto-resolve toggle plus one-shot solve/pass, lock, and QTE buttons | ✅ |
| Heal to full | button-driven raw write of the player attribute set `Health = MaxHealth` (`Features::FullHeal`); no `ProcessEvent`, so it's safe straight from the menu thread | ✅ |
| Invincible allies | `aiInvincibleAllies` (default on): each AI pump tops every spawned ally + recruited guard to `MaxHealth` (`SetCharacterHealthFull`, raw guarded write - no restore bookkeeping). Turn it off and they take normal damage again. This is what makes the squad actually usable instead of dying in seconds | ✅ |
| World / sky tint | recolors every `Engine.Light` in the loaded level via `ALight.SetLightColor` (the directional sun tints the whole sky/scene). **Perf-fixed:** the light list is cached and rebuilt only every ~4 s (`RebuildLightListIfStale`), and the colour is **quantised to ~32 steps/channel** so a static colour re-applies exactly once and a rainbow re-applies only a few dozen times per cycle - instead of re-dirtying hundreds of lights' render state at 10 Hz, which was tanking frame times. Off resets the cached lights to white | ✅ |
| Chams + console + viewmodes | enemy model recolour via `MeshComponent.SetVector/ScalarParameterValueOnMaterials` (+ optional custom-depth), and `KismetSystemLibrary.ExecuteConsoleCommand` for viewmodes / show-flags / `r.*` cvars. All run on the game-thread visual pump (mirror of the AI pump) so they never race the renderer | ✅ |
| Interactive puzzles (minigames + door locks) | the debug subsystem does **not** touch two families of puzzle: `BPC_MiniGameBase_C` minigames (dials/grids, tri-way electric lockpick `MiniGame_TriWay_C`) completed via `MiniGame_SetComplete`/`SetGameComplete`, and `BP_LockComponent_C` door locks (the CodeLock button grid, ColorsLockPick, CoinLock, UniversalLock) opened via `Unlock()`. Both are handled by one worker-thread routine driven from a `kPuzzleTargets` table: it resolves each component UClass (`Cls_MiniGameBase` / `Cls_LockComponent`) and walks its `Children` list for the functions by short name (the BP packages mount under `/Game/...`, so a `Function Pkg.Class.Fn` full-name needle never matches - `FindFunctionInClass` sidesteps the path), then a single IsA-sweep of GObjects enqueues live, non-template, not-already-done instances; the render `Tick` fires `ProcessEvent` on the queue. Driven continuously by the *Instant puzzle resolve* toggle and one-shot by the *Solve* button (which also dumps live puzzle-class candidates on a miss). **Discovery is worker-thread only** so the Present hook never stalls | ✅ |

Fly blocks normal game movement input while active, but leaves look input alone,
so mouse look still drives the camera and the menu's own free-fly movement reads
keyboard state directly. Enabling fly captures a return point and the Player tab
can teleport back to that point if level streaming leaves the pawn in an unloaded
area.

---

## 6. Adding a new cheat - worked example

Say you want **infinite money/NORA** (Atomic Heart's currency).

1. **Find it in the dump.** Grep the generated SDK:
   ```bash
   grep -rniE "Nora|Currency|Money" dumped-sdk/CppSDK/SDK/AtomicHeart_classes.hpp
   ```
   Note the owning class, the member name, and its `// 0x….` offset comment.

2. **Find how to reach that object** from the player (often a component on the
   character, or a subsystem reachable from `GameInstance`). Use
   `dumped-sdk/GObjects-Dump.txt` to confirm a live instance exists.

3. **Add the offset** to `Offsets::AH` in `src/sdk/offsets.h`.

4. **Implement it** in `src/features/features.cpp` inside `TickImpl()`, always
   guarding the pointer:
   ```cpp
   if (st.infiniteMoney) {
       uint8_t* comp = /* reach the wallet component, guarded */;
       if (Mem::IsReadable(comp + AH::Wallet_Amount, 4))
           *reinterpret_cast<int32_t*>(comp + AH::Wallet_Amount) = 999999;
   }
   ```

5. **Add the toggle**: a `bool` in `Features::State` (`features.h`) and a
   `ImGui::Checkbox` in the matching tab (`menu.cpp`).

6. Build (`build.bat`) and re-inject.

To **call a function** instead of poking memory, resolve it once via
`CachedFn("Function /Script/Package.Class.FunctionName")` and `ProcessEvent` it
with a param struct matching the dump's signature. Verify the parameter size in
`dumped-sdk/CppSDK/SDK/*_parameters.hpp`; for example `K2_SetActorLocation` is
`0x9C` bytes and contains an `FHitResult` of `0x88` bytes.

---

## 7. Safety model (why injecting can't crash the game)

Built with `/EHa`, so `catch(...)` traps **both** C++ exceptions and access
violations. Layered defenses:

- `Mem::IsReadable()` gates **every** game-memory dereference.
- `ValidateSdk()` requires real core UE4 names (`Object`, `Class`, `Property`…)
  before flipping `sdkReady` - wrong offsets degrade to "SDK: NOT resolved".
- `GetFullName()` caps the `Outer` chain depth (no runaway strings).
- `Features::Tick()`, the entire **Present hook body**, save/teleport, and the object
  dump are each wrapped in `try/catch`.
- This is what makes injecting at the **loading screen / menu** safe - if the renderer
  or object graph isn't ready, we skip our frame instead of taking the game down.

**Eject** any time with **END**.
