# Hook Bodyguard native movement reversal

## Scope and invariants

This applies only to the experimental bodyguards in **Hook Diagnostics**. The normal AI/Squad tab keeps its existing behavior. The game executable is read-only on disk; all detours are runtime MinHook patches and all targets fail closed.

The Hook roster never enters the legacy squad blackboard, `Pawn.AddMovementInput`, raw velocity, or periodic Mercuna flush paths. Combat temporarily owns native movement and follow reacquires it afterward.

## ReVa map

### Mercuna request entry

| Item | RVA / slot | Verified behavior |
|---|---:|---|
| `UMercunaNavigationComponent.MoveToLocation` exec wrapper | `0x110D860` | Parses reflected parameters and has one direct helper call at wrapper `+0x275`. |
| Native location request builder `FUN_1410e8060` | `0x10E8060` | Signature used by the implementation: `(nav, FVector*, endDistance, speed, partial)`. Builds the opaque request and dispatches the component virtual Move method. |
| Mercuna Move request | vtable `+0x410` | Receives the internal request plus request-ID output. All location/actor request construction converges here. |
| Mercuna Stop | vtable `+0x418` | Cancels/stops the active component move. |
| Mercuna Cancel | vtable `+0x430` | Cancel path used by the component and tasks. |

The resolver starts from the exact reflected `MoveToLocation` UFunction, reads its native exec pointer at `UFunction +0xE0`, validates the wrapper prologue, decodes the `E8 rel32` at `+0x275`, and validates the target prologue. The implementation never invents or reconstructs `FMercunaMoveRequest`.

The three virtual targets are read from a live `UMercunaNavigationComponent` vtable and must all be executable before ownership is accepted. If any detour fails, that component is not used by the native backend.

Native subclasses may expose different vtable addresses while inheriting the same Move/Stop/Cancel function targets. Registration aliases those vtables to the already-installed target detours and records a single installation owner, preventing both `MH_ERROR_ALREADY_CREATED` failures and duplicate removal during shutdown.

### Generic AIController fallback

| Item | RVA / slot | Verified behavior |
|---|---:|---|
| `AAIController.MoveToActor` exec wrapper | `0x48FC910` | Builds an `FAIMoveRequest`, then dispatches vtable `+0x790`. |
| `AAIController.MoveToLocation` exec wrapper | `0x48FC330` | Builds the location request and dispatches the same vtable `+0x790`. |
| Controller move request | vtable `+0x790` | Returns `FPathFollowingRequestResult`; status byte is at result `+4`. |
| `AController.StopMovement` exec wrapper | `0x45AD7D0` | Tail-jumps to vtable `+0x728`. |
| Path-following status | controller `+0x2F8`, component `+0x1D8` | `AAIController.GetMoveStatus` reads this byte directly. `3` is Moving. |

For a guard without Mercuna, the controller receives one direct native `MoveToActor` request. Runtime vtable detours refuse external Move and Stop calls while follow owns the controller. A refused external Move returns a zeroed `FPathFollowingRequestResult` (`Failed`) instead of leaving the caller with uninitialized output. The existing ProcessEvent hook also refuses reflected Move/Stop replacements. Internal calls use a thread-local gate and pass to the original.

The direct request uses the ReVa-mapped 0x40-byte `FAIMoveRequest` layout. The wrapper maps use-pathfinding to flag bit 2, allow-partial-path to bit 3, stop-on-overlap to bit 5, and can-strafe to bit 7. Hook follow uses `0x7F`: native pathfinding on, partial paths on, stop-on-overlap on, and strafing off so locomotion faces its travel direction.

### Twin mixed navigation

Dumped metadata and ReVa agree on these `AAIMixedNavigationCharacter` fields:

| Field | Offset |
|---|---:|
| automatic navigation selection | `0x1F98` |
| Mercuna 3D movement component | `0x1FA0` |
| Mercuna navigation component | `0x1FA8` |
| current navigation type | `0x1FC2` |
| forced-navigation flag | `0x1FC3` |

`ForceNavigationType` exec wrapper is at RVA `0x22539C0`. Its ReVa-confirmed call at wrapper `+0xC2` reaches dispatcher `FUN_141d49270`; the dispatcher call at `+0xDE` reaches the real transition routine `FUN_141d493a0` (RVA `0x1D493A0`). The final target is accepted only after both calls and its prologue pass sanity checks.

The transition routine changes current mode, activates/deactivates both navigation components, updates the controller path-following component, updates movement capability, and broadcasts the navigation-type change. Hook Twin follow calls this real transition with mode `1` and pins automatic=0/forced=1 while it owns ground movement. Combat and release restore automatic=1/forced=0. Mode `2` remains mapped for a future vertical-traversal backend, but the current follow fix does not claim 3D traversal.

ReVa also confirms the automatic selector at RVA `0x1D49010`: it only changes mode when `automaticNavigation` is nonzero and `forcedNavigation` is zero, deriving mode 2 from movement mode `MOVE_Flying` and mode 1 otherwise. That behavior explains why leaving a spawned Twin automatic could preserve flight/3D state while the Hook code was driving ground follow keys. `FUN_1410e9750` is called on the same `0x1FA8` Mercuna navigation component during mode changes.


## 2026-06-26 validated Hook Twin status

Live testing after the controller/Recast rewrite confirms the Hook Diagnostics Twin movement path is correct: she follows, walks with real locomotion, closes distance, and keeps the expected AI/combat behavior. Do not disturb this movement path while fixing combat edge cases.

The remaining post-test issue is target lifecycle: after a robot dies, the Twin can keep standing over the corpse and attacking it. The fix is combat teardown only. Dead targets are now rejected by `Char_bIsDead` as well as health, and duplicate stale-target clears perform a throttled full stand-down instead of only clearing the raw target pointer. Movement, route issuance, nav mode, and the direct-generic controller detour are intentionally unchanged.

Native Twin attacks may still fail to damage robots even while the AI and montage behavior are correct. Treat that as a future external damage attribution problem, not a reason to rework follow/combat movement. A later system should apply damage only when the Hook Twin is confirmed to be attacking/contacting a live robot target.
## Command state machine

Each Hook guard has one command state:

1. Start beyond 3.5 m; hold inside 2.5 m.
2. Ordinary robots issue one owned native `AAIController::MoveToActor`. Hook Twins issue reflected `AIController.MoveToActor` through the registered direct-generic `+0x790` controller detour; controller ownership stays false so native BT/controller ticks are not blocked.
3. Sample real actor displacement every 500 ms.
4. On no progress, robots stop/replan. Hook Twins repair/verify the path-following chain and stop/reissue the reflected Recast route after a short delay; no raw velocity, movement-input pulse, teleport, or Mercuna fallback is used.
5. On combat entry, the per-frame injector seeds target + IsAggressive (the recipe that walks her); the approach drive owns one controller `MoveToActor` toward the enemy on Recast. Inside 4.5 m, stop and release movement ownership so native turning, root motion, abilities, and attack montages execute without interception.
6. On combat exit, stop the enemy-approach request before acquiring a new player-follow command.
7. On hold, release, mode disable, or actor removal, stop once and clear ownership.

### Twin ground movement: AIController + Recast, gated by blackboard IsAggressive (snapshot-confirmed)

This corrects an earlier flight-primary attempt. The Twins (`AAIMixedNavigationCharacter`) **walk on the ground using the standard UE path stack — `AIController` + `RecastNavMesh` + `AHPathFollowingComponent`** (driven by their BehaviorTree), exactly like a normal robot. **Mercuna is their flight (3D) system only and is NOT involved in ground movement.** Our Hook code had been driving the Twin through the Mercuna nav component, the wrong subsystem, so ground never translated her and only flight glided her ("she glides / walks away / never reaches me").

Decisive evidence is two read-only diagnostic snapshots under `AtomicHeartMenu_diagnostics`:

| Field | `twinmovingandattackingusafterrelease` (walks) | `twinnotmoving` (stuck) |
|---|---|---|
| path subsystem | AIController + RecastNavMesh-Medium | same |
| `movement_mode` | 1 (MOVE_Walking) | 1 |
| `move_status` | 3 (Moving) | 3 (Moving) |
| path length to goal | ~4-5 m (valid) | ~4-6 m (valid) |
| blackboard `IsAggressive` | **True** | **False** |
| `TargetEnemy` | player | None |
| `b_always_aggressive` (char flag) | true | true |

Both have a live Recast path of similar length and `move_status=3`, yet only the **aggressive** one actually translates. Because `b_always_aggressive` is true in both, it is the **blackboard `IsAggressive` key** (set via `SetBlackboardIsAggressive`) that gates whether her CharacterMovement follows the path. The proven-walking config (released chase) is `TargetEnemy` + `IsAggressive` + AIController/Recast move.

The policy is therefore: re-assert `SetBlackboardIsAggressive(true)` while she moves (her BT recomputes that key from perception and closes it to `false` when no enemy is present), and drive her with the reflected `AIController.MoveToActor`. She carries no `TargetEnemy` during pure follow, so this opens locomotion without an attack montage, and team-0 + forced friendship already zero any damage. Aggression is cleared on the hold transition so she settles peacefully. Hook Twins are excluded from both legacy Twin loops and the legacy grounding heartbeat.

### Twin route issuance through the crash-firewalled reflected wrapper

The Twin controller's vtable `+0x790` is **not** the generic `RequestPathMove`; it is a mixed-navigation override (`FUN_141d4a6d0`, RVA `0x1D4A6D0`). **Every** way of issuing a direct AIController MoveTo to her routes through it (hand-built `FAIMoveRequest` AND reflected `AIController.MoveToActor`, pathfinding on or off), and it has a latent null-deref:

```
lVar4 = TWeakObjectPtr::Get(controller + 0x4a0)   // FUN_14277c190; the active path/route object
if (route segment count [lVar4 + 0x130] == 0)      // EMPTY route
    // skips setting the route entry, leaving it null...
... deref [routeEntry + 0x10]                       // -> ACCESS_VIOLATION at +0xB1 (read addr 0x10)
```

The engine itself drives `+0x790` while her route is being (re)built, so the empty-route case fires intermittently and, uncaught, slowly degrades her movement (the recurring `+0x1D4A781` faults). Two-part fix:

1. **Crash firewall (`ai_movement_hooks.cpp`).** `SehControllerMove`/`SehControllerStop` wrap the original `+0x790`/`+0x728` dispatch in `__try/__except` and swallow the empty-route fault (returning `Failed`). `ExceptionGuard` is a vectored handler that returns `EXCEPTION_CONTINUE_SEARCH`, so the fault reaches our frame `__except`. We install the controller detour (`RegisterController`) **purely for this firewall** — covering the engine's own calls too — but never `SetControllerOwned(true)`/block her (blocking that per-tick update is what froze her). `DumpStatus` reports the firewalled fault count.

2. **Drive her BehaviorTree state and issue the real reflected MoveToActor request through the firewall.** Unpin her pedestrian schedule (null `AICh_Schedule` @ `0x1788` every tick; stash once in `g_stashedSchedule` for restore), pin the real mixed-navigation transition to mode 1 while follow owns her, and write the keys her BT reads: `SetControllerFollowLocation` / `ForceFollowLocation=true` / `CanReachFollowLocation=true` / `SetFollowLocationSpeed`, plus `Uses3DNavigation=false`, `CanReach2D=true`, `CanReach3D=false`, and the `IsAggressive` locomotion gate. The reflected request intentionally reaches `+0x790`: its generic first stage creates the Recast route, while the installed detour contains any fault in the Twin-specific post-processing. The path-following component then drives her real walk animation and arrival deceleration. `FollowLocation` is aimed at a standoff point `kHookFollowStopM` (2.5 m) short of the player so she finishes early instead of ramming; re-chase past `kHookFollowStartM` (3.5 m). Combat and release restore automatic mixed-navigation selection.

The spawned Twin does not start a route from blackboard writes alone: the path-following status remains `Idle`. Hook follow therefore calls the reflected `AIController.MoveToActor` wrapper with pathfinding enabled. ReVa confirms that the mixed override first invokes the generic AIController/Recast route builder and only then enters the fault-prone Twin-specific post-processing; the installed `+0x790` SEH detour contains that fault. Measured stalls now stop and reissue this route. No raw velocity, movement-input pulse, or teleport is used.

**Combat** uses the same rule: clear `ForceFollowLocation`, set `IsAggressive`, and let her BT chase the `InjectAttack`-seeded `TargetEnemy` + attack natively (get out of her way). The clean follow/combat key separation — follow keys off during combat, target cleared during follow — fixes the "stuck between combat and following / twitching" behaviour. Robots keep the owned hand-built `ControllerMoveToActor` path (their `+0x790` is the clean generic one). Mercuna stays resolved only for a future vertical-traversal fallback.

Hook Diagnostics also exposes a dedicated spawn menu: a recommended loaded-runtime-class dropdown plus a searchable character/boss list using the existing guarded game-thread spawn queue. Every resulting actor is registered directly into the Hook roster.

This removes the previous queue flood: no 300/900 ms MoveTo spam and no periodic four-second Stop flush.


## 2026-06-26 Hook roster deletion/cleanup manager

Hook Diagnostics now has an explicit delete-roster path. Deleting a Hook bodyguard must clear more than the actor: the runtime cleanup removes native follow state, combat injection state, engaged/threat holds, Twin state, movement-hook registration, controller ownership, saved schedule/original-team bookkeeping when appropriate, and any references from other Hook guards. New Hook registration also clears stale state for the same pointer first, covering UE pointer reuse after `K2_DestroyActor`.

Use the Hook tab's `Delete hook roster` control for destructive removal. General AI-list deletion now routes through the same cleanup before `K2_DestroyActor`, so deleting the old Hook bodyguard cannot poison the next spawned bodyguard's follow loop.
## Failure behavior

- Missing/invalid helper callsite: native Mercuna requests remain unavailable.
- Missing/non-executable vtable target: no detour and no command for that backend.
- Partial detour installation: installed members are removed and registration fails.
- Missing Mercuna component: use the guarded AIController backend.
- Missing controller/path-following component: no movement command is issued.
- Missing Twin transition resolver: the Twin keeps its current mode; no enum is forced blindly.
- Combat and released actors are never held by movement ownership.

## Deliberately withheld native paths

ReVa maps `AIBlueprintHelperLibrary.SpawnAIFromClass` to exec wrapper RVA `0x48FA290`. Its direct call at wrapper `+0x507` reaches the generic world spawn helper at RVA `0x413ED00`, and the returned pawn receives `SpawnDefaultController` through vtable `+0x710` when needed. The helper ABI is `(world, class, FTransform*, FActorSpawnParameters*)`.

Hook Diagnostics spawning still uses the reflected `SpawnAIFromClass` wrapper. The wrapper's 0x30-byte `FActorSpawnParameters` stack object contains a packed flag byte whose preserved high-nibble semantics are not yet uniquely proven. Reconstructing that bitfield would be an invented ABI and can create a partially initialized actor, so direct native spawn remains refused until that field is mapped. Recruitment and all post-registration movement remain fully isolated from the legacy movement path.

Mercuna's completion/delegate notification path is also not detoured. Follow completion and recovery use the native request result where available, path-following status for `AAIController`, and measured actor displacement for Mercuna. This avoids guessing delegate layouts while still preventing queue flooding and permanent idle lock.

## Manual test matrix

After the user builds:

1. Spawn/recruit one ordinary robot in Hook Diagnostics, walk 20 m, stop, turn, and repeat. It must maintain native locomotion and the 2 m ring without permanent idle.
2. Watch `HookFollowNative(GT)`. Exactly one backend should be active per guard.
3. For Mercuna, verify move issues occur on start/drift/recovery, not every tick. External Move/Stop/Cancel block counters may rise when the game attempts to override ownership.
4. For a non-Mercuna robot, verify controller Move/Stop vtable detours become live and direct block counters rise if the game cancels the request.
5. Stop moving for 10 seconds. The guard must hold without oscillation or repeated Stop calls.
6. Move across stairs and around obstacles. A progress timeout may cause one visible hitch and one controlled replan, not continuous twitching.
7. Test a Hook Twin on level ground, stairs, and around obstacles. Verify `HookTwinRecast(GT)` reports `navMode=1`, successful keys, direct generic hook counters, decreasing distance, and no Mercuna movement counter involvement.
8. Enter combat during follow. Verify movement ownership releases at melee range, abilities and attacks remain native, dead targets clear via `Char_bIsDead`/health plus stand-down, and follow reacquires immediately afterward.
9. Release the Hook roster and disable the mode. Verify active commands stop and Twin automatic navigation is restored.

## 2026-06-26 Controller vtable alias reuse

Some boss/controller classes share individual vtable functions without sharing the entire controller vtable. Vov reproduced this with `Controller.StopMovement(+0x728)`: the controller chain had already installed a detour for the shared stop target, but `RegisterController` only treated all four controller slots as reusable when every target matched. The result was repeated `MH_CreateHook=3` logs every registration attempt.

`ai_movement_hooks.cpp` now reuses controller detours per slot: Move, Stop, CommitPath, and BuildPath each look up an existing original trampoline by target address before calling MinHook. New controller classes only own the slots they actually install, and shutdown/removal only removes owned slots. Refusal logs are throttled so an unexpected third-party/already-created hook cannot flood the console while a guard is trying to register.
