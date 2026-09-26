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
│   ├── dllmain.cpp           Entry: worker thread, hook install, settings load, END eject
│   ├── core/
│   │   ├── globals.h         Shared atomics (running / menuOpen / sdkReady) + module info
│   │   ├── log.{h,cpp}       Live console + AtomicHeartMenu.log next to the game exe
│   │   └── memory.{h,cpp}    Mem::IsReadable - pointer-safety guard used everywhere
│   ├── hooks/
│   │   ├── dx12_hook.{h,cpp} Present/ResizeBuffers/ExecuteCommandLists hooks + ImGui DX12
│   │   └── wndproc_hook.{h,cpp}  Window subclass: feeds input to ImGui, INSERT toggle,
│   │                             numpad hotkeys, END key
│   ├── menu/menu.{h,cpp}     The ImGui window (themed: Player / Weapons / AI·Squad / World / Visuals / Render / Misc / Debug tabs)
│   ├── features/features.{h,cpp}  Per-frame cheat logic (god mode, fly, one-hit, …)
│   ├── features/bodyguards.{h,cpp}  Companion policy (targets, orders); engine calls go
│   │                             through BodyguardEngine, so tests can fake them
│   ├── features/possession.{h,cpp}  Play as: possess a character, drive it, fire its abilities
│   ├── features/sandbox.{h,cpp}  Sandbox: the game's map list and a save-free level load
│   └── sdk/                  *Minimal* hand-written UE4 runtime SDK (see §4)
│       ├── ue4.{h,cpp}       FName/UObject/GObjects/GWorld + ProcessEvent + lookups
│       ├── reflect_call.h    RefCall::Call - UFunction calls laid out from the game's reflection
│       ├── scanner.{h,cpp}   AOB scanner - the fallback when the static RVAs fail
│       └── offsets.h         *** ALL build-specific offsets live here ***
│
├── tests/bodyguard_policy_tests.cpp  Fake-engine policy checks (CMake AHM_BUILD_TESTS)
├── tools/injector.cpp        Minimal LoadLibrary injector -> bin\injector.exe
├── tools/find_globals.py     Recover GObjects/GNames/GWorld from the exe after a patch
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

Folder name encodes the engine version and the game's changelist - the dump this
repo was originally built against was `4.27.2-18319896+++UE4+Release-4.27-AtomicHeart`.
The changelist moves with each game patch, so a dump of buildid `24534183` or later
will not match that name.

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

### The global offsets

These are RVAs relative to image base `0x140000000`, captured from Steam buildid
`24534183`:

| Global | RVA |
|---|---|
| `GObjects` (TUObjectArray) | `0x06EC2BC0` |
| `GNames` (FNamePool) | `0x070F7BC0` |
| `GWorld` (UWorld**) | `0x070F43C0` |
| `ProcessEvent` vtable index | `0x44` |

They're already plugged into `src/sdk/offsets.h`.

> ⚠️ These are valid for **that build only** - every game patch moves them.

Resolution does not depend on them alone. `ResolveGlobals()` tries the static RVAs
and the `SIG_*` signature scan, in the order `USE_STATIC_OFFSETS` picks, and keeps
whichever passes validation. A patch that moves the RVAs but leaves the signatures
matching therefore still comes up on its own. Recovering the numbers by hand:

1. `python tools/find_globals.py` - static analysis of the shipping exe, no game
   running and no Dumper-7. Prints paste-ready constants for the three globals plus
   `ExpectedImageSize`, and flags whichever entry in `offsets.h` has gone stale.
2. **Debug tab → Verify member offsets** - reads the member layer off the running
   game. Every `UPROPERTY` carries its own byte offset in the engine's reflection
   data, so a class's property list answers "where does `RootComponent` live on
   *this* build" directly. The button diffs those live offsets against the
   constants in `offsets.h` and writes the verdict per entry to the log, naming
   the ones a patch has moved and the value to set them to. No Dumper-7 run.
3. Re-inject **Dumper-7** for what reflection cannot reach - the native
   `UObject`/`UStruct`/`FField` chain, the `GObjects` array layout and
   `VFUNC_PROCESSEVENT` are not `UPROPERTY`s. They are also the values a game
   patch is least likely to move, since they come from the engine version rather
   than from the game's own classes.

Validation is not just a null check: the object sweep confirms `GObjects` and
`GNames` by resolving core UE4 type names, and `ValidateGWorld()` walks `*GWorld`
to confirm it lands on a `World` class. A null `*GWorld` does not fail resolution -
it is legitimately null until a map loads - so it is logged as unverified, and the
check only proves the pointer out once a map is in.

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
| Give weapon | dropdown resolves `DA_Item_*` weapon data assets and calls `AAHPlayerCharacter::InstantTakeWeapon`; selected grant also calls `EquipWeaponByDataAsset`, give-all grants every listed weapon. Kalash is labelled `Kalash Rifle / AK-47`. **Game-thread only:** both grants go through `QueueGameThread` and the buttons return once the request is accepted, never once the weapon arrives - a grant mutates the inventory and spawns/attaches a weapon actor, and running that from the Present hook raised an access violation inside the engine that then hung the game (`Skorchekd/AtomicHeartMenu#6`). The pawn is resolved inside the queued task, so a possession change between click and drain cannot hand game code the wrong receiver. **Two type gates before either dispatch:** the receiver must `IsA(AtomicHeart.AHPlayerCharacter)` (ProcessEvent does not check its receiver, so an AHPlayerCharacter-only function reaching another pawn class runs the native thunk against members that class does not define), and the asset must `IsA` the class the UFunction's own `WeaponItemDataAsset` parameter declares, read via `Reflect::ObjectPropertyClassInStruct` - the object-name fallback that resolves these assets matches on a name substring alone and cannot rule a wrong type out by itself. Reading the expected class off the function means a patch that retypes the parameter is followed rather than guessed. **Weapons with no content in the install are skipped BEFORE the take** (`WeaponAssetHasContent`): some `kWeapons` entries resolve to a data asset carrying no content, and one of those still takes and lands in storage as a named but EMPTY slot. `LargeIcon` on the item data asset separates the two - set on every weapon confirmed working, unset on PTRD (the crash) and on the empty storage slots; it split 19 usable from 18 stubs, the SAME 18 in a base game session and a DLC2 one, so this tracks the install rather than which campaign is loaded. Read live off the asset so the set is never hardcoded, since a patch or a DLC purchase moves it; an absent `LargeIcon` property (a rename on a future build) passes rather than refusing every weapon. A correlate rather than a proven cause, so `WeaponModelReady` backs it up after the take by reading the spawned weapon's `Mesh` component and then its `SkeletalMesh`/`StaticMesh` (engine property names, so build-independent), fail-closed: anything unreadable counts as not ready, because a refused equip leaves a usable weapon in the inventory while a wrong "yes" ends the session. **Equip is deferred, not inline:** `InstantTakeWeapon` returns before the weapon actor it spawns exists, and `EquipWeaponByDataAsset` against an asset with no instance yet returns having done nothing - the weapon reached the inventory and the wheel but never the player's hands. The grant records a pending equip that the render tick paces (`ProcessPendingEquip`, 60 ms apart, 24 attempts) and the pump executes, using `FindWeaponByDataAsset` as the readiness test: a non-null answer is the spawned weapon, so the equip has something to switch to. Builds without that function fall back to equipping on the first deferred attempt. A grant that faults latches `g_giveWeaponFaulted` for the session and refuses every later grant, including an in-flight give-all: the fault leaves the engine part-way through an inventory mutation, and a second grant on top of that is how one survivable fault becomes an unrecoverable one | ✅ |
| Misc puzzle bypass | resolves `DebugSubsystem_0` and calls `DebugSubsystem.SetInstantPuzzleResolve`, `InstantLockUnlock`, and `WinQTE`; includes an auto-resolve toggle plus one-shot solve/pass, lock, and QTE buttons | ✅ |
| Heal to full | button-driven raw write of the player attribute set `Health = MaxHealth` (`Features::FullHeal`); no `ProcessEvent`, so it's safe straight from the menu thread | ✅ |
| Invincible allies | `aiInvincibleAllies` (default on): each AI pump sets a companion's `IncomingDamageMultiplier` (Base and Current) to `0`, the attribute god mode uses, and tops it to `MaxHealth`; topping health alone lost to a single burst between two pumps. `aiCompanionDamage` (default 3) scales `InstigatedDamageMultiplier`. The originals are captured per companion, keyed by pointer plus `InternalIndex` and `FName` so a recycled address starts clean, and are restored on release, on removal, when the option is turned off, and when a dead or dropped companion leaves the squad (`ApplyCompanionVitals` / `RestoreCompanionVitals`) | ✅ (live test pending) |
| World / sky tint | recolors every `Engine.Light` in the loaded level via `ALight.SetLightColor` (the directional sun tints the whole sky/scene). **Perf-fixed:** the light list is cached and rebuilt only every ~4 s (`RebuildLightListIfStale`), and the colour is **quantised to ~32 steps/channel** so a static colour re-applies exactly once and a rainbow re-applies only a few dozen times per cycle - instead of re-dirtying hundreds of lights' render state at 10 Hz, which was tanking frame times. Off resets the cached lights to white | ✅ |
| Chams + console + viewmodes | enemy model recolour via `MeshComponent.SetVector/ScalarParameterValueOnMaterials` (+ optional custom-depth), and `KismetSystemLibrary.ExecuteConsoleCommand` for viewmodes / show-flags / `r.*` cvars. All run on the game-thread visual pump (mirror of the AI pump) so they never race the renderer | ✅ |
| Interactive puzzles (minigames + door locks) | the debug subsystem does **not** touch two families of puzzle: `BPC_MiniGameBase_C` minigames (dials/grids, tri-way electric lockpick `MiniGame_TriWay_C`) completed via `MiniGame_SetComplete`/`SetGameComplete`, and `BP_LockComponent_C` door locks (the CodeLock button grid, ColorsLockPick, CoinLock, UniversalLock) opened via `Unlock()`. Both are handled by one worker-thread routine driven from a `kPuzzleTargets` table: it resolves each component UClass (`Cls_MiniGameBase` / `Cls_LockComponent`) and walks its `Children` list for the functions by short name (the BP packages mount under `/Game/...`, so a `Function Pkg.Class.Fn` full-name needle never matches - `FindFunctionInClass` sidesteps the path), then a single IsA-sweep of GObjects enqueues live, non-template, not-already-done instances; the world pump fires `ProcessEvent` on the queue on the game thread, with a zeroed parameter frame and after re-checking each object's identity. Driven continuously by the *Instant puzzle resolve* toggle and one-shot by the *Solve* button (which also dumps live puzzle-class candidates on a miss). **Discovery is worker-thread only** so the Present hook never stalls | ✅ |

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
- `Mem::IsExecutable()` gates every **indirect call** into game code. Readable is
  not executable: a bad function pointer that merely reads fine still DEP-faults
  on the call, and that fault happens after control has left our code, where
  `/EHa` and `catch(...)` can no longer contain it. `UObject::ProcessEvent`
  returns instead of dispatching when its vtable slot fails this check.
- `UE::IsLiveObject()` gates **cached** game pointers. Destroying a `UObject`
  does not unmap its memory - the allocator hands the block straight back out - so
  a dead object stays readable and its fields return whatever now occupies those
  bytes. Liveness is asked of the engine's own registry instead: a live object is
  reachable at its own `InternalIndex` in `GObjects`, and destruction clears that
  slot. `IsLiveObjectNamed()` adds a full-name confirmation for caches keyed by
  name, since a recycled slot can hold a different live object. It proves "still
  registered", not "safe to dispatch on": an object marked `PendingKill` by GC
  still round-trips until its destructor runs. The object caches, the fly path and
  the subsystem pins use it; the older AI caches still gate on `Mem::IsReadable`
  and have not been converted.
- `Scanner::IsFunctionEntry()` gates the **hardcoded native-hook RVAs** in
  `features.cpp`. `IsExecutableAddress` is true of every byte in `.text`, including
  mid-instruction, so it cannot catch an RVA that a patch has shifted; MinHook would
  then write its 5-byte JMP across an instruction boundary and rewrite the game's
  own code. Resolving the address against the image's exception directory makes a
  stale RVA fail closed - and beginning at an entry is not enough on its own, since
  a linker-split function has one per range, so an entry that chains to another is
  refused too. Injection reports any RVA that has gone stale. The signature-derived
  hooks in `src/hooks/` do not use it yet.
- `ValidateSdk()` requires real core UE4 names (`Object`, `Class`, `Property`…)
  before flipping `sdkReady` - wrong offsets degrade to "SDK: NOT resolved".
- `GetFullName()` caps the `Outer` chain depth (no runaway strings).
- `Features::Tick()`, the entire **Present hook body**, save/teleport, and the object
  dump are each wrapped in `try/catch`.
- This is what makes injecting at the **loading screen / menu** safe - if the renderer
  or object graph isn't ready, we skip our frame instead of taking the game down.

- **Eject quiesces before it frees anything.** Every detour, the window procedure
  and our worker threads hold a `G::HookScope`. `DX12Hook::Quiesce` disables every
  MinHook detour, unhooks the window procedure and waits until `G::hooksInFlight`
  has read 0 for 100 ms. On a timeout (5 s), or when another overlay subclassed the
  window after us, nothing is freed and the DLL stays loaded but inert.
- **ImGui input is locked.** The window procedure (game thread) appends to ImGui's
  input queue and `NewFrame` (render thread) drains it; both hold
  `WndProcHook::InputMutex`.

**Eject** with **END** (the dedicated key, with the game window focused) after
closing Nora. The first press switches fly/noclip off if needed; press again once
movement is restored. Cleanup is queued on the verified game thread; an unsafe or
unavailable cleanup defers eject.


## September 2026 official-build investigation

Steam build 24534183, image size 0x78F0000, UE 4.27.2-18319896.
Fresh Dumper-7 captures (ignored `work/`):

- `dump-2026-09-11`: main menu, 6471 ms.
- `dump-world-2026-09-11`: loaded world, 12968 ms, automatic dumper unload.
- GObjects RVA 0x06EC2BC0, GNames 0x070F7BC0, GWorld 0x070F43C0.
- ProcessEvent RVA 0x02750AB0, vtable index 0x44.
- FUObjectItem flags +0x08: native PlayOpenWorld checks PendingKill bit 29.
  Object liveness also rejects BeginDestroyed/FinishDestroyed EObjectFlags.

Native disassembly found shipping stubs at RVA 0x00F824B0 for DebugSubsystem
ToggleFreeResources, ToggleNoRecipesRequires, UnlockAllSkills and ToggleDebugMenu.
These dispatches cannot implement the advertised features. Workbench instead uses
FInventoryData Amount (+0x30, stride 0x50) in DA_ItemBase.ItemsToCraft,
WeaponUpgradeData.Price and SkillOwningConditionsForResources.PriceToBeOwned;
TryToGiveAllSkillsInCategory on the current CharacterSkillsComponent; and the
loaded WBP_MainMenuLevelSwitcher_C widget. Its GetIsShippingBuild override is
restricted to the owned widget, preserving normal entitlement checks.

Nora uses deferred Blueprint spawning, normal construction/BeginPlay,
LoadItemsFromSettings, and polls AreItemsFromSettingsLoaded before Used. No
synchronous wait or ToggleCraftWindow call is used. Voice and visual acceptance
remain runtime tests; component presence alone does not prove audible playback.

Free roam currently exposes native PlayOpenWorld, requiring an eligible save.
There is no verified arbitrary campaign-to-sandbox transition. Hide objective is
display-only and preserves the original tracked quest for restoration.

### Local agent harness

Disabled by default. Enable in Tests, or place `enable` in
`AtomicHeartMenu.tests/enable.once` beside the DLL before a development injection.
The marker is consumed once. Worker-thread file I/O and bounded game-thread
observations run twice a second; normal feature execution remains on its existing
thread. `status.json` is atomically replaced and carries protocol, session, PID,
write/sample timestamps, accepted/processed IDs and observations. A stale sample
timestamp means the game-thread pump stopped; it is not a passing result.

Write a complete `command.txt` using an atomic rename. Format:
`SESSION ID COMMAND VALUE` with an increasing positive ID and a finite number.
Only the current session token is accepted. Limit 256 bytes. Commands:
`observe`, `menu`, `nora_spawn`, `nora_use`, `nora_close`, `nora_choice` (1..6),
`nora_remove`, `prices` (0..2), `skills_unlock`, `fly`, `noclip`,
`fly_forward`, `fly_backward`, `fly_up`, `fly_down` (>0..2 seconds),
`fly_return`, `turn` (-180..180 degrees), `streaming`, `squad_spawn`,
`squad_recruit`, `squad_release`, `squad_aggressive`,
`verify_offsets`, `snapshot`, `stage_open`, `stage_close`, `open_world`,
`objective_hide`. Boolean controls take 0 or 1. Pulses require fly and a closed
mod menu. No arbitrary console commands, process execution, memory writes or
user-supplied paths are accepted. Dispatch acknowledgement is separate from
observations. Price verification samples up to 64 lists; squad samples up to 32.

Validation: Release builds pass. Live feature acceptance is pending and must
cover companion movement/player safety, Nora construction/use/voice/close/remove,
price change/restoration, skills, fly movement/restoration, and stage UI.

### Live validation checkpoint and remaining work

- `work/test-results/12-offsets-recheck.json` corresponds to the shared log's
  29 matching member offsets / zero moved / Level.Actors unreflected, and all 13
  parameter-frame checks passing. The mixed-navigation property names were fixed.
- The portable Nora and two existing world fridges share `SK_CraftMachine`,
  `ABP_CraftMachine_C`, `D_Craft`, `DA_Base_CraftMachine`, and `A_UI_Craft_Open`.
  Mesh visibility is enabled and the animation instance is live. In-world dialogue
  appeared and the game-thread heartbeat continued. This does not verify sound.
- Construction collision mode 3 rejected Nora because her composite component
  shapes overlap. Mode 2 permits engine adjustment. The latest source also checks
  floor slope and line of sight using two bounded LineTraceSingle calls. These
  new placement checks have compiled but have not yet run in game.
- Blueprint `InScen` remained false during dialogue. The latest guard additionally
  checks AHBaseCharacter.GetCurrentDialogComponent and the component's current
  widget. Response controls call only that portable Nora widget's OnVariantNPressed
  methods; StopDialog is an explicit close request. Never destroy an active Nora.
- New calls validate reflected parameter element size, array dimension and frame
  bounds. The harness observes mesh/animation presence, owned skill entry count,
  and restored/skipped/mismatched price values. These additions await live reload.
- Price scan reached 281 lists, with 64 sampled and zero mismatches; restoration
  was dispatched and logged. Actual purchases and final restoration counts need
  verification in the new build. Skill acquisition has not yet been exercised.
- Fly moved upward in a short indoor test and restored walking mode and height
  without a crash or health loss. Long-distance streaming/lighthouse reproduction
  remains untested. Do not call that issue fully resolved from an indoor test.
- RTSS overwrote a DXGI Present hook in one session. The compatibility resolver
  follows an observed RTSS trampoline to its validated function entry, but a fresh
  game restart with both overlays is still needed to validate coexistence.
- Optional experimental Twin selector/factory RVAs 0x1B93A50 and 0x1CA06E0 fail
  function-entry checks on this image and remain refused, not silently relocated.
- Ground companion follow/player safety, full Nora craft/skill/voice lifecycle,
  stage selector, and the revised sidebar still need live acceptance.

For a staged build while the active DLL is locked, MSBuild supports an isolated
`/p:OutDir=.../work/staging/` override. The client accepts `--directory` to select
that DLL's harness directory. Its default remains `bin/AtomicHeartMenu.tests`.
Do not inject a second copy while the current menu is loaded.


### September 12 continuation: regular companions and Nora regressions

Regular companion policy now lives in `src/features/bodyguards.cpp`. The
`SpawnAndRegisterAlly` regular path was logging registration without calling it;
it now registers before friendship/follow initialization. Hook Diagnostics remains
on its separate controller. The simplified roster exposes Follow + defend,
Follow only, Hold position and explicit attack; release controls on that page
exclude Hook Bodyguards. Player and roster members are rejected as attack targets.
The actual player's team is read back after conversion, and unconfirmed allegiance
keeps combat/movement disabled. Native team-switch exec RVA 0x21D0E10 copies the
other character's team for Friendly on build 24534183. Native movement, retaliation
and damage acceptance still require a loaded-save test.

The opt-in CMake `AHM_BUILD_TESTS` target `bodyguard_policy_tests` contains 28 fake
engine policy checks, including autonomous target reacquisition during Hold and
Follow only. Compilation succeeds; Windows Defender ASR rule
01443614-CD74-433A-B99E-2ECDC07BFC25 blocked launching this executable. These tests
have NOT passed execution yet and cannot substitute for native movement tests.

Object lookup persists build-keyed `AtomicHeartMenu.cache/object-names.json`;
crafting discovery persists `crafting.json`. Both cache names and index hints,
never live pointers or engine-owned arrays. Identity validation precedes use;
invalid/stale hints fall back to bounded worker discovery. File I/O stays off the
game thread. Price application consumes bounded batches instead of rescanning the
object array on the game thread. The name worker is joined before DLL unload.

`Unlock recipe requirements` learns loaded recipe assets through
AHInventory.AddItemsToInventory(asset,1). Reversed AHInventoryPlayer virtual +0x4A0
(RVA 0x1EE87E0) handles category Recipe=10 by adding RecipeDataAsset to the saved
PlayerItemsToCraft array. HasRecipeFor is checked before and after each grant;
there is no arbitrary TArray replacement or display-only permission flag.
This persists through the game's normal save behavior; disabling stops further
grants and does not unlearn recipes. Runtime grants/purchases remain unverified.
Harness commands now include recipes_unlock (0/1), squad_order (0 defend, 1 follow
only, 2 hold), and weapon_rgb (0/1). Observations include regular_companions and
recipe success/failure counts.

RGB now traverses MaterialInstance parents and the root Material's
CachedExpressionData.Parameters.RuntimeEntries[1].ParameterInfos. Live examples
matched vector-value counts. It preserves layer/blend associations and calls
SetVectorParameterValueByInfo; guessed names and non-colour vectors (hit locations,
UV transforms) are excluded. Replaced material slots trigger rebuilding; restoration
only touches slots still owned by the menu. Visible colour remains unverified.

Nora weapon-menu hitches are NOT fixed by the cache changes alone. Captured in
PID 1556: 10610 ms in BP_Base_CraftMachine_C.MultipleOptionUse while choosing the
weapon response, and 9453 ms in WBP_CraftWindowMain_C.OnBackToPreviousMenu on exit.
The deepest ProcessEvent timing still includes native work that does not dispatch
through ProcessEvent. A real stalled-thread capture is still required; the earlier
500 ms sampler threshold captured normal frames and is not root-cause evidence.
Free costs were present in sampled live arrays; both native resource precheck
0x1EEB900 and consumption 0x1EEB6A0 skip nonpositive amounts. Added native
UpdateCraftItemCount refresh, but actual successful crafting still needs testing.

The stage selector remains experimental. Its shipping query's native exec
0x1A26110 returns true; a ProcessEvent-only override does not establish that native
Blueprint VM calls are intercepted. The current process also lacks the loaded
selector Blueprint. No arbitrary mission-free campaign transition is implemented.
The desktop is locked and the current game is in MainMenuScene. Latest source
build is in bin, not injected into PID 22048; runtime acceptance is pending.


### September 26 continuation: crash fixes, companion policy and quality of life

Made without access to the game. The sources pass a clang syntax check against
MinGW headers; the existing MSVC-only `createHook` function-pointer conversion in
`dx12_hook.cpp` is the only error that check reports. The MSVC Release build and
every in-game behaviour below still need testing. `bodyguard_policy_tests` now has
47 checks and passes, built with g++ on Linux against a stub `Windows.h`, also
under AddressSanitizer and UndefinedBehaviorSanitizer. It has still not run on
Windows.

Threading. These moved from the Present hook to the game thread:

| Work | Now runs from |
|---|---|
| Infinite ammo tick, restores on disable | world pump (`TickInfiniteAmmo`) |
| Puzzle completions, instant-puzzle flag | world pump (`DrainPuzzleCompletions`, `UpdateInstantPuzzleResolveToggle`) |
| Global time dilation (bullet time and plain) | world pump (`ApplyTimeDilation`, single owner) |
| Player scale | world pump (`ApplyPlayerScale`) |
| Refill ammo now, Max weapon upgrades | `QueueGameAction` |
| Unlock lock, win QTE, skip objective, complete quests, solve puzzle | `QueueGameAction` (`QueueDebugCall`) |

The world pump is scheduled by the render tick one bounded task at a time, like
the AI and visual pumps, and keeps running while any of those features still owns
state it has to put back. Only the player's `CustomTimeDilation` field write stays
on the render thread. `QueueGameThread` reports a dropped task, and the AI, visual,
world and fly pumps clear their in-flight flags when that happens.

Other crash fixes:
- Puzzle completions call their UFunctions with a zeroed frame sized from
  `UStruct::PropertiesSize`, not `nullptr`, after re-checking the object's
  `InternalIndex` and class recorded by the worker.
- Death tombstones store `InternalIndex` and `FName`; an address now holding a
  different object is not treated as dead. Companions are never tombstoned.
- The death tracker memoises name-based death detection per UFunction, runs only
  while companions, Hook mode, fight-each-other or a horde run is active, and only
  on the game thread. The stale-target filter also runs on the game thread only,
  the Twin death-pipeline lookup probes at most once a second, and the
  fight-staging and action-container hooks remember a refusal instead of
  rescanning on every dispatch.
- Ammo restores skip an inventory, weapon or barrel that is no longer live.
  `AiDeleteActor` pins `InternalIndex` before queueing `K2_DestroyActor`.
- `Log::Write` ignores re-entry on the same thread.

Companion policy (`src/features/bodyguards.cpp`). `Bodyguards::UpdateAll` runs one
pass per AI pump for all regular companions, engaged ones first. For each
companion:
1. If its raw target (`CachedTargetEnemy`) or its blackboard `TargetEnemy` is the
   player or a protected unit, combat is cleared first.
2. Allegiance is re-checked every 750 ms; an unconfirmed companion neither fights
   nor follows.
3. Only Follow + defend and Follow + attack may fight. Each threat qualifies by
   the first matching rule: explicit order (inside the leash), attacks the player
   or attacks a companion (the nearer of enemy-to-player and enemy-to-companion
   inside the defend radius), continue the current fight (inside the leash),
   passive enemies never, hunt (Follow + attack, hostile, inside the defend
   radius), intercept (its target, blackboard target or `LastSensedCharacter` is
   you or a companion, hostile, within the intercept radius of you).
4. Scores: explicit -6000, attacks player -5000, continue -4000, attacks
   companion -3000, intercept -2000, hunt -1000, plus the distance to the player
   in metres, plus 12 per companion already assigned to that enemy, minus 15 for
   the current target. The lowest wins, so attackers are shared out without
   dropping ongoing fights.
5. With no target: Hold stops movement, every other order follows.

"Attacks the player" reads the blackboard through the controller's
`TargetEnemy` key-name field and `UBlackboardComponent.GetValueAsObject`, after
checking `IsA(AHAIController)` and `ControllerBlackboardReady`. Hostility is
`AIUtils.AreFriendlyCharacters(ai, player, CountNeutralAsFriendly=true)` negated,
with a team-id comparison as fallback, cached per actor for 2 s. Settings are
`Bodyguards::Configure`d every pass from `aiDefendRadiusM` (5 to 100),
`aiInterceptRadiusM` (0 to defend) and `aiLeashRadiusM` (defend to 150).
`InjectAttack` re-kicks a squad member's aggressive state every 5 s instead of
every 1.5 s, which restarted attack and ability montages.

Follow (`DriveSquadVelocityGameThread`): each regular companion stops at
max(follow distance, capsule radius + 0.9 m) plus 0.6 m for every second and
third slot. The capsule radius is read by reflection (`CapsuleComponent`,
`CapsuleRadius`) and memoised per class. The follow-location speed is 400, 650 or
900 depending on the gap; for a sprint `MaxWalkSpeed` is raised to 900 and put back
once caught up. The squad prune drops dead companions (`bIsDead` or zero health),
restores their vitals and erases their per-actor state.

Commands: `AiAttackAimTarget` (the cached enemy nearest the screen centre within
12 degrees and 150 m, companions and corpses excluded), `AiDispatchAttack`
(crosshair target first, then each companion's nearest combat-capable enemy),
`AiRegroup` (recall, then `K2_TeleportTo` beside the player for companions past
20 m), `AiToggleHoldAll`, `AiHealCompanions`, `AiOrderCompanion`. World AI
operations (kill, passive, launch, freeze) skip `IsProtectedUnit`, and
`AiDispatchKill` acts on the explicit selection only. Harness `squad_order` accepts
3 (Follow + attack).

Quality of life:
- Hotkeys: `WndProcHook` reports `WM_KEYDOWN` (not auto-repeat) to
  `Features::NoteHotkey`, which records a bit while the menu is closed and the SDK
  is ready; `ProcessHotkeys` runs the action on the next render tick, exactly like
  the menu control. Keys are never swallowed.
- Eject: the window procedure sets `G::ejectRequested` for `VK_END` with the
  extended-key bit (so not numpad 1 with NumLock off) unless an ImGui text field
  has focus. Before the window procedure is installed, the main thread falls back
  to `GetAsyncKeyState`, only while a window of the game process is in front.
- Notices: `Features::Notify` (any thread) keeps one message for 2.5 s; the overlay
  draws it at the top of the screen and stays active while a notice is up.
- Settings: `Features::LoadSettings` runs before any hook is installed.
  `SaveSettingsIfChanged` runs on the worker every 2 s and once after the hooks
  are quiesced on eject. It writes `AtomicHeartMenu_settings.json` beside the game
  exe through a `.tmp` file and `MoveFileExA`. Every value is clamped to its menu
  control's range on load. Cheat toggles are never stored.
- `Features::TurnOffAllCheats` clears every cheat toggle; each feature then
  restores its state through its normal disable path.

### September 26 continuation, part 2: AI integrity, play as and a save-free sandbox

Also made without access to the game. The same clang check passes on every source
(the only errors are still the three MSVC-only `createHook` conversions in
`dx12_hook.cpp`), and the 47 policy checks pass, also under AddressSanitizer and
UndefinedBehaviorSanitizer. Nothing below has run in the game yet.

**Invincible robots missing from every list.** Several mod paths could leave a
robot in a state nothing owned any more:

| Cause | What it left behind | Fix |
|---|---|---|
| The kill wrote `Health = 0` before `Suicide` | Death is an event pipeline (death event, `K2_OnDeath`, the death ability); a raw write fires none of it. When `Suicide` did not finish the job the robot stayed alive at 0 HP. The AI cache drops 0-health actors, so it vanished from the list and from Kill all, took no damage and kept fighting | Kills go through `Suicide` only (`ApplyAiKill`). `VerifyPendingKills` re-checks each kill after 3 s, tries `Suicide` once more, and 3 s later gives a robot that still will not die its health back and clears its tombstone, so it stays listed and killable. The horde's non-destroy path uses the same kill |
| Fight each other moved half the robots to team 231, unchecked | A team the game counts as neutral or friendly makes robots immune to your weapons and Shok. Neither the team nor the aggression flags were restored when the brawl ended | `ResolveFightTeamB` asks the game (`AIUtils.AreFriendlyCharacters`) on one brawler, trying 231, 1 to 16 and 0, skipping group A's team, yours and 255, and keeps the first candidate hostile to both group A and you. The brawler's own team is written straight back. No qualifying team means the robots are not split. Every robot the brawl drives is recorded in `g_fightParticipants` with a timestamp |
| Aggression flags written with no record | Robots latched "always aggressive" or passive for good | `RememberAggroFlags` records `bIsPassive`, `bPassiveButWithSenses` and `bIsAlwaysAggressive` before the first write; release and stand-down put them back |
| Freeze thawed only robots still inside its radius | A robot you walked away from stayed frozen, and one killed while frozen could never play its death out | `g_frozenAi` records every frozen robot; switching the freeze off thaws all of them, and a robot being killed is thawed first and never re-frozen |
| The squad prune dropped a member that was still alive without restoring it | A robot on your team, unlisted and immune to your weapons | Members dropped alive are released (Hook roster too) after `g_squadMutex` is let go; dead ones get their own team and flags back, so a revived robot is an ordinary enemy |
| Release ran while the robot was still a squad member | The ownership lock swallowed the switch back to its faction | `SquadRemove` now runs before `ApplyAiRelease` everywhere (Release selected, Release all, the Hook roster, the queued release) |
| Kill selected on a companion | Incoming damage multiplier 0, so the kill never took, and the robot stayed friendly | The companion is released first, then killed |
| The companion damage boost was never captured | Outgoing damage stayed boosted after a robot left the squad | `SetCharacterInstigatedDamage` captures the original the way the vitals do |

The safety net is the **AI state ledger**: `g_origTeam`, `g_origAggro`, `g_frozenAi`
and `g_fightParticipants`. `RestoreOrphanedAiState` runs every 2 s in the AI pump.
It restores every entry that no feature owns any more: not a companion, a Hook
guard or the character being played; not in the running horde; and not a brawler
while the brawl is on or drove it in the last 120 s. The pump keeps running until
the ledger and the pending kills are empty. Health-span checks in
`SetCharacterHealthFull`, `ReadCharacterHealth` and `FullHeal` now cover `Health`
(0x48), which sits after `MaxHealth` (0x38).

**Repair bugged AI** (`Features::AiRepairAnomalies`). The worker sweeps GObjects
for every live `AHAICharacter` (CDOs and templates skipped, capped like Deep kill),
so it reaches robots the AI cache misses. On the game thread,
`RepairAiAnomaliesGameThread` does the following for each robot:
- A robot alive at 0 HP gets full health and loses its tombstone.
- Companions, the horde and active brawlers are skipped from here on.
- A ledger team is restored. A robot still on a fight team (231, or one resolved
  earlier) with no record, for example from an older build or session, is switched
  hostile to you.
- A robot left at the freeze's exact 0.0001 time dilation is thawed while the
  freeze is off.
- A robot the game reports as hostile but with an incoming damage multiplier of 0
  gets 1 back.

`RefreshAiActors` counts 0-HP robots that are still possessed, and the menu shows
that count above the button.

**Eject.** Once the Workbench cleanup has succeeded, the eject lambda runs
`Possession::CleanupGameThread` and then `AiCleanupForUnload`:
- Fight and freeze are switched off and a horde run is ended.
- Companions the mod spawned (`g_modSpawned`, keyed by `InternalIndex`) are
  destroyed with `K2_DestroyActor`, and recruited ones are released.
- Brawlers stand down, every frozen robot is thawed, and whatever is left in the
  ledger is restored.
- Pending kills that left a 0-HP robot are healed.

**Shok and other inputs.** The mod never raises the engine's ignore-input
counters (`UpdateGameInputBlock` is a no-op) and never swallows a key; hotkeys only
observe `WM_KEYDOWN`. No code path was found that blocks an ability. The likely
causes were:
- Robots moved to a neutral fight team, which Shok will not treat as a target.
  This is fixed above.
- Player state left behind by a cutscene, a dialogue or an interrupted feature.

`Features::RepairPlayerState(rebindInput)` runs on the game thread and puts the
player back to normal without fighting a switch that is still on:
- `ResetIgnoreInputFlags` and `WidgetBlueprintLibrary.SetInputMode_GameOnly`.
- Movement mode None or Flying becomes Falling, and collision is switched back
  on, unless fly or noclip is on.
- Time dilation goes back to 1 unless bullet time is on.
- `bIsDead` is cleared when health is above 0.
- The incoming damage multiplier is reset from 0 unless god mode is on. The
  outgoing one is reset from above 50 or non-finite unless one-hit kill is on.

With `rebindInput` the controller unpossesses and re-possesses the character, as a
respawn does. That rebuilds its input component and its ability system's actor
info. It refuses while playing as another character.

**Save guard.** `hkProcessEvent` swallows `SaveProgress`, `SavePersistentData` and
`CheckpointSaveProgress` while the horde (`g_blockSaves`) or any bit of
`g_saveBlockOwners` is set. `Features::SetSaveBlock(owner, on)` takes
`SaveBlockPlayAs` or `SaveBlockSandbox`. It has to be switched on from the game
thread, because it resolves the save UFunctions; switching it off works from any
thread.

**Play as** (`src/features/possession.cpp`, experimental).
- `BeginGameThread` records the player's body and the target's AI controller. It
  then calls `PlayerController.Possess(target)` and checks `GetLocalPawn()`. If
  the game refuses, the body is re-possessed and the AI controller put back.
- While playing, `SaveBlockPlayAs` is held and the body's incoming damage
  multiplier is held at 0. The target's is held at 0 too while Invulnerable is
  ticked. Both originals are put back afterwards.
- AI characters bind no player input, so `Possession::Tick` (render thread) reads
  the keys with `GetAsyncKeyState`, only while the game window is in front and
  the menu is closed. The window procedure adds raw `WM_INPUT` mouse deltas.
- One `Drive` task is queued on the game thread at a time. It calls:
  - `AddYawInput` and `AddPitchInput` with the mouse delta times 0.07 times the
    sensitivity.
  - `AddMovementInput` relative to the `GetControlRotation` yaw. Space adds up
    and Ctrl adds down, for fliers.
  - `Jump` and `StopJumping`.
  - For sprint, `MaxWalkSpeed` is raised to max(1.8 times, 900) and restored.
- The third-person camera is a `CameraComponent` added with `AddComponentByClass`
  and placed every drive with `K2_SetWorldLocationAndRotation`. If the add is
  refused once, the view stays first-person.
- Abilities are the distinct classes in `AbilitySystemComponent.ActivatableAbilities.Items[i].Ability`.
  The item stride comes from `GameplayAbilitySpec`'s reflected `PropertiesSize`.
  They fire with `TryActivateAbilityByClass`. First the crosshair character
  becomes the target (`SetTargetEnemy`, `CachedTargetEnemy`), and the body turns
  to face it.
- Play ends when the character dies or disappears, or the game moves the
  controller elsewhere. The body is only re-possessed when the controller is
  still on the target or on nothing. The character gets its old AI controller
  back, or `SpawnDefaultController`.
- `IsProtectedUnit` covers the played character, so world commands, brawls and
  repairs leave it alone. The Num . hotkey toggles play.

**Sandbox** (`src/features/sandbox.cpp`). This replaces the save-dependent
free-roam route for players without a finished-campaign save.
- `AssetRegistryHelpers.GetAssetRegistry` gives the registry, and
  `GetAssetsByClass(World, bSearchSubClasses)` returns `FAssetData` (0x60 bytes,
  read once). `/Engine/` and `/Script/` packages are dropped.
- Names that look like persistent maps (`_P`, `persistent`, `_main`,
  `openworld`, `mainmenu`) are sorted first. When that filter would hide
  everything, all maps are shown.
- A map loads with `GameplayStatics.OpenLevel(WorldContextObject, LevelName, bAbsolute = true)`,
  using the registry's own `PackageName` FName.
- Load by name converts typed text with `KismetStringLibrary.Conv_StringToName`.
  The text must be ASCII letters, digits and `/ _ -`. Anything from a dot on is
  dropped, because FURL would read a dot as a network host.
- `SaveBlockSandbox` goes up before `OpenLevel` and stays up until the title
  menu. The title menu is detected as `BP_MainMenuPlayerController_C`, spawned in
  the running world.
- `Sandbox::Tick` follows the world by pointer, `InternalIndex`, package name and
  any non-live gap between them. The first world to arrive after a load becomes
  the sandbox, even when a failed load fell back to the title menu. If a minute
  passes with no world change, the request was dropped, and the block is lifted
  unless it was already up.
- A level the game moves on to, or a campaign save loaded from the sandbox's pause
  menu, keeps the block and gets a notice.
- Eject is refused while the block is up. Unticking the setting lifts it.

**Calls the game has not confirmed yet.** Every call above goes through
`RefCall::Call` (`src/sdk/reflect_call.h`). It resolves the UFunction on the
receiver's class and lays each parameter out by name, at the offset and size the
running game's reflection reports. It refuses, and logs "Reflected call
unavailable", on any name or size mismatch, so a wrong guess is a logged no-op
rather than a corrupt frame. These are the calls still to confirm on this build:
- `AHAICharacter.Suicide` finishing every robot type.
- The team candidates `AreFriendlyCharacters` accepts.
- `Controller.Possess` on AI characters, and `AddComponentByClass` in a shipping
  build.
- Which abilities `TryActivateAbilityByClass` starts outside their own behaviour
  tree.
- The asset registry being present in the cooked build.
- `OpenLevel` bringing up a playable pawn on a map entered cold.

The first `AtomicHeartMenu.log` from a test run answers most of them.
