# AI bodyguard and friendship hooks

## Scope

This document covers the experimental bodyguard implementation in the **Hook Diagnostics** tab. It is separate from the normal AI/Squad tab. No resolver writes to or patches `AtomicHeart-Win64-Shipping.exe` on disk; all inspection is read-only and runtime hooks are installed in process memory through MinHook or the existing `ProcessEvent` detour.

## Confirmed crash chain

ReVa/Ghidra and the captured runtime crash agree on this chain:

1. `AHM_AIContextAllyTick_crashcaller` (`FUN_141b988e0`) reads the controller context target ally.
2. Call at RVA `0x1B9906B` invokes `GetContextTargetAlly` (`FUN_141cf8720`, RVA `0x1CF8720`).
3. Call at RVA `0x1B9907B` invokes `IsA_AHAICharacter` (`FUN_141bcbc70`, RVA `0x1BCBC70`).
4. A non-`AHAICharacter` ally is replaced with null.
5. The call at RVA `0x1B990A5` invokes `ContextAllyComp` (`FUN_141bc8ff0`, RVA `0x1BC8FF0`) with null `RCX`.
6. `ContextAllyComp` reads `[RCX+0x738]`, causing the access violation.

The historical bodyguard path triggered this by assigning the player to `TargetAlly`. The player is an `AHBaseCharacter`, not an `AHAICharacter`.

## Required crash firewall

`CrashGuard.ContextAllyComp` is the required hook. Its resolver is anchored by the unique `AIContextTick.callsite` signature at RVA `0x1B9908A` and decodes the `E8 rel32` call at `callsite + 27`. The resulting target must:

- be inside an executable PE section;
- begin with the ReVa-confirmed `[this+0x738]` helper bytes;
- match the target called by the unique AI context block.

Only after these checks does the MinHook detour install. The detour returns null when `this` is in the null page and otherwise calls the original. Rescans never tear down a working firewall, and the required guard cannot be disabled from the UI.

## Why standalone helper signatures were ambiguous

The compiler emitted several helpers with nearly identical prologues and object-validation logic. Searching the entire module also allowed byte sequences in data regions to inflate match counts. A prologue match alone therefore did not prove call-graph identity.

The scanner now treats code and data as separate domains:

- detour/code signatures scan executable PE sections only;
- whole-module search is an explicit scope reserved for data-aware work;
- a PE parse failure does not silently fall back to whole-module code scanning;
- decoded `E8`/`E9` targets must remain inside executable sections.

Executable-only scanning reduces false positives, but it does not make a generic prologue authoritative. The specific callsite relationship remains the stronger resolver.

## Resolver status

| Hook | Requirement | Resolver | Confirmed RVA | Status |
|---|---|---|---:|---|
| `AIContextTick.callsite` | required resolver | unique executable-section signature | `0x1B9908A` | resolved when unique and sanity checked |
| `CrashGuard.ContextAllyComp` | required firewall | `E8 rel32` at callsite `+27` | `0x1BC8FF0` | detour installs after sanity check |
| `GetContextTargetAlly` | optional helper | `E8 rel32` at callsite `-0x1F` | `0x1CF8720` | read-only call target after sanity check |
| `IsA_AHAICharacter` | optional validator | `E8 rel32` at callsite `-0x0F` | `0x1BCBC70` | read-only call target after sanity check |
| `CrashGuard.AllyCastBase` | optional defence-in-depth | ReVa-confirmed `E9 rel32` tail jump at `GetContextTargetAlly + 0x26` | `0x1D097B0` | detour installs after sanity check |
| `AIUtils.AreFriendlyCharacters` | optional friendship helper | exact SDK UFunction metadata; ReVa-confirmed native exec thunk | `0x225BDA0`* | intercepted through `ProcessEvent`, not a raw thunk detour |

`0x225BDA0` is the ReVa-confirmed native UFunction exec thunk reference. Runtime interception compares the unique reflected UFunction pointer, which is safer and narrower than detouring a generic native thunk.

If the anchor is missing, ambiguous, outside executable memory, or fails byte semantics, dependent hooks are marked missing, ambiguous, skipped, or failed-sanity and remain inactive.

## Safe bodyguard and friendship strategy

The Hook Diagnostics bodyguard path now follows these rules:

- it keeps a dedicated Hook Diagnostics roster, separate from general squad selection;
- Hook Diagnostics recruit/spawn/follow/attack/release buttons operate on that roster;
- the player may be a follow location, focus actor, move target, and protected actor;
- the player is never written to `AHAICharacter.SetTargetAlly` or `AHAIController.SetBlackboardTargetAlly`;
- non-null `TargetAlly` assignments are validated with the native `IsA_AHAICharacter` helper and fail closed;
- invalid attempts log `[AI] Skipping TargetAlly assignment: target is not AHAICharacter`;
- follow movement is isolated from the normal Squad/Twin loop and runs from the game-thread hook pump through the native movement ownership controller;
- Hook-owned Twins are excluded from `DriveTwinCombat`, `DriveTwinsFollowGameThread`, and legacy ground-nav repinning; only the Hook movement/combat state machines may command them;
- Hook Diagnostics guards use a fixed 2.0 m hold ring with 2.75 m restart hysteresis;
- recruitment performs team/target initialization only and cannot seed a legacy SDK follow request or legacy Twin mode pin;
- Mercuna guards use the ReVa-derived native `MoveToLocation` request builder and vtable-owned Move/Stop/Cancel dispatch; no reflected movement UFunction is called;
- guards without a Mercuna component receive one native `AAIController::MoveToActor` request; vtable `Move +0x790` and `Stop +0x728` plus reflected Move/Stop interception prevent the game from replacing it while follow owns movement;
- no Hook Bodyguard uses the normal squad blackboard drive, per-frame `Pawn.AddMovementInput`, raw velocity fallback, or periodic Mercuna flush;
- actual actor displacement is sampled every 500 ms; sustained zero displacement outside the ring performs one controlled stop, waits 150 ms, then issues one replacement request;
- moving guards clear controller focus so they face travel direction instead of walking backward toward the player;
- Twins use their native mixed-navigation transition routine: vertical separation or blocked 2D progress selects 3D, vertical convergence selects 2D, and combat releases navigation back to automatic selection;
- combat approach uses the same native movement backend until melee range, then releases ownership for native abilities and attack animations;
- threat perception scans the cached AI set in all directions, with no view-cone test;
- idle threats only qualify inside a 12 m player/guard protection perimeter; a robot actively targeting the player or a managed guard qualifies out to 35 m;
- dead, unreadable, and completed targets clear the native aggressive state immediately, with no stale combat hold;
- Hook Diagnostics guards receive a large outgoing damage multiplier; the first observed health delta, or first close melee contact fallback, completes the kill and records `[AI-HIT]`;
- threat selection and attack otherwise use the guard's native aggressive state, enemy target, controller behavior, abilities, and attack animations;
- the ownership lock blocks engine attempts to point managed guards back at the player;
- `AreFriendlyCharacters` is forced true only for a player/managed-bodyguard pair while experimental Hook Debug mode is active;
- every other friendship query calls the original game behavior.

The crash firewall remains active even if optional friendship or diagnostic helpers are unavailable.

## Hook Diagnostics dry run

The tab provides read-only checks for:

- selected guard pointer, name, and class;
- player pointer, name, and class;
- guard `AHAICharacter` status;
- player `AHBaseCharacter` and `AHAICharacter` status;
- whether the historical assignment would be unsafe;
- whether current code would attempt that unsafe assignment;
- whether the friendship override would apply;
- required crash-guard state.
- registered native movement component/controller, selected backend, ownership state, native helper/detour state, and current Twin navigation mode.

`Validate Bodyguard Pair` and `Validate TargetAlly Assignment` do not mutate game state. `Dump AI Hook Status` writes the full resolver state to the log. `Rescan Signatures` preserves active detours. `Clear Hook Logs` truncates the current menu log and writes a new marker.

## Manual build and test procedure

1. Build the DLL using the repository's normal Release build process.
2. Inject in a safe save and open **Hook Diagnostics**.
3. Confirm `CrashGuard.ContextAllyComp` is `active/detour`, resolver `callsite-derived`, RVA `0x1BC8FF0`, executable-only `yes`, sanity `pass`.
4. Confirm the callsite, `GetContextTargetAlly`, `IsA_AHAICharacter`, and `AllyCastBase` show the RVAs above. Do not continue if any derived target reports failed sanity.
5. Enable experimental hook bodyguards. Confirm the friendship row resolves through SDK/ReVa metadata.
6. Recruit or spawn one combat AI from this tab, select it, and run both validation buttons. Expected player result: `AHBaseCharacter=yes`, `AHAICharacter=no`, current unsafe assignment `NO`.
7. Walk away and confirm the guard starts after crossing about 2.75 m, continuously closes using native walk/run animation, and holds at about 2.0 m without oscillating.
8. Confirm `HookFollowNative(GT)` reports either `mercuna=1` or `controller=1`. The old Hook-path `input` and `velocity` counters must remain unchanged.
9. For a Mercuna guard, confirm native move/stop counters increment and external Move/Stop/Cancel blocks remain available. For a non-Mercuna guard, confirm the controller vtable detours become live.
10. Test stairs and a Twin across different elevations. Confirm the Twin changes to mode `2` for 3D traversal and returns to mode `1` after vertical convergence, without timer-based re-pinning.
11. Put an idle robot farther than 12 m away and confirm the guard keeps following. Then bring it inside the perimeter or make it target the player and confirm the guard engages from any facing direction.
12. Let the guard land one hit. Confirm the target dies and the log records `[AI-HIT] ... source=damage-delta` or `source=melee-contact`.
13. Confirm the guard immediately stops attacking the dead location and reacquires native follow.
14. Attack or provoke the guard and confirm it does not target the player. Watch friendship-force and ownership-block counters.
15. Release the Hook roster and verify the native command stops, Twin automatic navigation is restored, and original team/behavior restoration runs.
16. Review `AtomicHeartMenu.log` for resolver method, sanity result, native movement issue/block counters, `firstHitKills`, `staleTargetsCleared`, friendship overrides, and guard catches.

Do not treat unavailable optional hooks as a crash-firewall failure. Stop testing if the required firewall is inactive.

Native spawning is not claimed as resolved: see `mercuna_hook_bodyguard_movement.md` for the ReVa-mapped spawn chain and the packed spawn-parameter field that keeps direct spawning fail-closed.
