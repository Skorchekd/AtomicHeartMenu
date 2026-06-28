# Hook Twin Movement Reversal Notes

Live append log for ReVa/local reverse-engineering of Atomic Heart Twin movement. Implementation is intentionally not edited by this process.


## 2026-06-26 06:32:21

Server bootstrap test. Local append endpoint is reachable.

- Repo under investigation: `C:\Users\Skorcht\Downloads\AtomicHeartMenu`
- Live note file: `%TEMP%\ahm_reversal\hook_twin_reversal.md`
- Server: `http://127.0.0.1:8765`
- Endpoints: `GET /health`, `GET /tail?n=80`, `POST /append`

## 2026-06-26 06:32:38

ReVa inventory:

- Ghidra project contains one analyzed target: `/AtomicHeart-Win64-Shipping.exe`
- MD5: `c0355ed20421abf11a3ef5d6cc54138d`
- Next: search decompilation/comments/bookmarks for Twin, Mercuna, FollowLocation, ForceNavigationType, movement-mode, AI controller target/focus keys.

## 2026-06-26 06:33:30

ReVa confirmation: Twin controller move override

- Program: `/AtomicHeart-Win64-Shipping.exe`
- Twin mixed controller move override: `0x141D4A6D0` / RVA `0x1D4A6D0`
- Generic AIController move stage: `0x14486E2E0` / RVA `0x486E2E0`
- `FUN_141D4A6D0` calls `FUN_14486E2E0` on line 16 before running Twin-specific route decoration.
- ReVa comments already mark `0x141D4A781` as the Hook Twin MoveTo fault: null route-entry deref at `[routeEntry + 0x10]` when route segment count is zero.
- Conclusion: correct walking path is not Mercuna. It is AIController/Recast route creation through the generic stage, with Twin-specific postprocess firewalled/bypassed only for the bad empty-route tail.

## 2026-06-26 06:34:05

ReVa confirmation: Twin mixed-navigation transition

- Native transition routine: `0x141D493A0` / RVA `0x1D493A0`.
- Fields confirmed in decompile:
  - `guard + 0x1FA0`: Mercuna 3D movement component
  - `guard + 0x1FA8`: Mercuna navigation component
  - `guard + 0x1FC2`: current navigation type
  - `guard + 0x1F98`: automatic navigation selection
  - `guard + 0x1FC3`: forced-navigation flag
- Transition behavior:
  - Writes current nav type at `+0x1FC2`.
  - Activates/deactivates both navigation components via vtable calls.
  - Touches controller path-following component at controller `+0x2F8`.
  - Calls `0x1410E9750` on `+0x1FA8` during mode changes.
- Automatic selector: `0x141D49010` chooses mode from character movement `+0x2A8 + 0x188` (`MOVE_Flying` => nav type 2, else type 1) only when `+0x1F98 != 0` and `+0x1FC3 == 0`.
- `ForceNavigationType` exec wrapper: `0x1422539C0`; it sets `+0x1FC3 = 1` then dispatches through `0x141D49270` into `0x141D493A0`.
- Implementation implication: use real transition or automatic selector. Do not write only `+0x1FC2`. For follow walking, mode 1 is correct. For allowed flight, restore automatic or explicitly use mode 2 only for a deliberate flight-follow backend.

## 2026-06-26 06:45:07

ReVa search limitation note

- Broad `search_decompilation` for blackboard names timed out after ~300s.
- `get_strings` regex for blackboard key/function names also timed out after ~300s on this full shipping binary.
- Do not block the workflow on string enumeration. Use the Dumper/SDK offsets already in `src/sdk/offsets.h` for blackboard key-name fields and validate movement through the controller/mixed-nav decompiles instead.

## 2026-06-26 06:45:59

ReVa confirmation: reflected `AIController.MoveToActor` dispatch

- `AIController.MoveToActor` exec wrapper: `0x1448FC910` / RVA `0x48FC910`.
- Lower-half decompile lines 232-259 show wrapper building the request flags/object fields, then dispatching through controller vtable `+0x790`:
  - line 248: request flag packing from reflected booleans
  - line 259: `(**(code **)(*controller + 0x790))(controller, result, request, 0)`
  - line 280: writes status byte back to reflected return value.
- For Twins, that vtable slot is the mixed override `0x141D4A6D0`, which first calls generic `0x14486E2E0` and then can fault in the Twin tail.
- Implementation implication: use the reflected wrapper or the existing `MoveControllerToActor(...)` helper with `usePathfinding=true`, but make sure `AiMovementHooks::RegisterController` is installed for the Twin so `+0x790` is firewalled/bypassed. Do not route walking through Mercuna.

## 2026-06-26 16:04:02

ReVa comments/bookmarks evidence

`search_comments("Hook Twin")` returned two relevant comments:

- `0x141D4A6E8` (`CALL 0x14486E2E0`): "Hook Bodyguard Twin fix: call target FUN_14486e2e0 is the generic AIController move stage. Registered Hook Twins dispatch directly here to skip the unsafe Twin-only route decoration tail."
- `0x141D4A781` (`MOV RDX, qword ptr [RSI + 0x10]`): "Confirmed Hook Twin MoveTo fault: dereferences null route entry at [RSI+0x10] when route segment count is zero. Bypassed only for registered Hook Bodyguard Twins."

This is decisive: proper fix is not Mercuna actor-follow. It is a controller-move detour policy for registered Hook Twins: call the generic stage `0x14486E2E0` directly or use original with SEH then generic fallback, and do not let the unsafe tail kill/freeze movement.

## 2026-06-26 16:04:43

Implementation handoff: delete current Hook Twin Mercuna follow block

Current bad block:

- File: `src/features/features.cpp`
- Function: `DriveHookBodyguardsNativeGameThread(...)`
- Section label: `// ---- TWIN (mixed-nav): Hook Diagnostics clone of the proven normal Twin mover ----`
- Current log prefix: `HookTwinMercuna(GT)` / `HookTwinMercunaBB`
- Problem: it drives `MercunaMoveToPlayer(...)` and `MercunaStop(...)` as the primary follow mover. Testing confirms this creates partial movement/glide/flying behavior and no reliable walking animation. ReVa says this is the wrong subsystem for ground follow.

Replacement workflow for Hook Twins:

1. Detect mixed-nav Twin as now with `IsMixedNavCharacter(guard)`.
2. Get controller with `GetAiController(guard)` and validate `ControllerPathFollowingReady(ctrl, guard)`.
3. Install `AiMovementHooks::RegisterController(guard, ctrl)` for the Twin, but do **not** set controller owned/blocking true for Twins.
   - Required: `AiMovementHooks::SetControllerOwned(guard, ctrl, false)`.
   - Required: `SetHookControllerOwned(guard, false)`.
   - Reason: the hook is a firewall/bypass for `+0x790`, not an ownership blocker. Blocking the engine's own per-tick route updates can freeze her.
4. Force/maintain ground for pure walking follow:
   - Use `ForceGroundNavIfMixed(guard)` / `AiMovementHooks::ForceMixedNavigation(guard, 1)` when entering follow or after combat release.
   - Write `Uses3DNavigation=false`, `CanReach2D=true`, `CanReach3D=false` while ground-follow owns her.
   - Do not repeatedly flip ground/flight every pump. Re-pin only on transition/recovery.
5. Unpin schedule every tick during follow:
   - `UnpinTwinSchedule(guard)` so `AICh_Schedule @ 0x1788` cannot hold her in idle/schedule branch.
6. Write the BT follow keys:
   - `SetControllerForceFollow(ctrl, s.moving)`.
   - `SetControllerFollowLocation(ctrl, goal)` and raw vector key `AICtrl_Key_FollowLocation @ 0x378`.
   - `AICtrl_Key_CurrentWaypoint @ 0x368 = goal`.
   - `AICtrl_Key_CanReachFollowLoc @ 0x3A8 = true`.
   - `SetControllerFollowSpeed(ctrl)` on transition into moving.
7. Set the locomotion gate while moving:
   - `SetControllerAggressive(ctrl, true)` while `s.moving` and no active combat target.
   - Keep `TargetEnemy`/cached target clear during pure follow so this opens locomotion without attack target/montage.
   - Clear it on hold: `SetControllerAggressive(ctrl, false)`, `ClearAiAggressiveLatch(guard)`, `StopCharacterAggressive(guard)`.
8. Issue the actual route through AIController/Recast:
   - Prefer existing helper `MoveControllerToActor(ctrl, guard, player, kHookFollowStopM*kUnitsPerMetre, true)` because ReVa confirms reflected `MoveToActor` dispatches vtable `+0x790` with pathfinding flags.
   - Reissue only on state transition, status not Moving, goal drift, or measured stall. Do not spam every 300 ms.
   - `DirectControllerMoveStatus(ctrl)` should read `3` while moving.
9. Repair/verify consumer binding before/after issuing route:
   - Use existing `EnsureHookTwinGroundPathChain(guard, ctrl, reactivate, out)`.
   - Log `pathFollower`, `boundMovement`, `characterMovement`, `navData`, `movementMode`, `velocity`, `repairedBinding`, `reactivated`.
   - Required success condition: path follower `MovementComp` equals CharacterMovement and movement mode is `MOVE_Walking`.
10. Stalls:
   - If path status is Moving but `Move_Velocity` and measured displacement stay zero, do **not** switch to Mercuna as primary.
   - First recovery: `EnsureHookTwinGroundPathChain(..., true, ...)`, reissue reflected MoveToActor, keep aggressive true.
   - Second recovery: stop movement once, clear/rewrite keys, reissue route after a short delay.
   - Flight should be a separate explicit fallback only after confirmed Recast path cannot close a vertical gap, and should have different logs. Do not mix it with normal ground follow.

Expected log names after replacement:

- Rename from `HookTwinMercuna(GT)` to `HookTwinRecast(GT)` or `HookTwinController(GT)`.
- Include: `dist`, `moving`, `status`, `mode`, `navMode`, `vel`, `routeIssued`, `moveRet`, `force`, `aggr`, `reach`, `use3d`, `can2d`, `can3d`, `pathFollower`, `boundMovement`, `cmc`, `repaired`, `reactivated`, `ctrlMoveCalls`, `genericBypass`, `faults`.
- Success looks like: `status=3`, `mode=1`, `navMode=1`, `vel>0`, `aggr readback true while moving`, distance decreasing, no Mercuna move counter involved.

## 2026-06-26 16:05:09

Required controller-detour policy change

Current `ai_movement_hooks.cpp::HkControllerMove` behavior for Twin override:

- If registered and `generic` is resolved, it calls the original Twin override first.
- Only if `MoveResultAccepted(...)` is false does it call the generic fallback.

Problem:

- ReVa comment at `0x141D4A6E8` says registered Hook Twins should dispatch directly to `FUN_14486E2E0` to skip the unsafe Twin route-decoration tail.
- The original Twin override can return an accepted result/status while still leaving the movement consumer/path decoration in a bad state. That matches observed logs: route accepted / status Moving / velocity 0.

Handoff requirement:

- For registered Hook Twins with `bypassTwinPostprocess=true` and `genericMove != nullptr`, call `genericMove` directly and return its result.
- Do not call `originalMove` first for registered Hook Twins.
- Keep SEH around the generic call too.
- Keep original path for unregistered/non-hook Twins so the rest of the game is untouched.
- Keep `owned=false` for Hook Twins so external/native per-tick BT/controller calls are not blocked; the detour only redirects them away from the Twin tail.

Suggested counter semantics:

- `twinGenericBypasses++` whenever a registered Hook Twin dispatches generic directly.
- `twinGenericFallbacks` should mean actual fallback after original failed; with direct-bypass policy it should stay mostly zero.
- Add a `twinDirectGenericAccepted` or reuse `twinGenericFallbackAccepted` only if logs make the meaning clear.

This is likely the missing piece from the earlier controller/Recast attempt. The route was accepted by the original Twin override, so fallback did not run; then the Twin tail left the follow visually dead.

## 2026-06-26 16:05:42

Combat handoff: Twin approach should use same controller/Recast path, not Mercuna

Current bad block:

- File: `src/features/features.cpp`
- Function: `DriveHookCombatApproach(...)`
- Current Twin log prefix: `TwinCombatMercuna`
- Problem: mixed Twin combat approach uses `MercunaMoveToPlayer(guard, target, 350.0f, 650.0f)`. This is the same wrong subsystem as follow and can produce glide/flying/animation weirdness.

Replacement workflow:

1. Combat selection stays in `InjectHookBodyguard(...)`.
2. `InjectAttack(guard, threat, ...)` remains the combat brain seed:
   - raw target field
   - aggressive flags
   - `SetAiTargetEnemy`
   - `SetControllerTargetEnemy`
   - `SetControllerAggressive(true)`
   - `ForceCharacterAggressive` throttled
3. In `DriveHookCombatApproach(...)`, for mixed Twins:
   - Register controller with `AiMovementHooks::RegisterController(guard, ctrl)`.
   - Keep controller ownership false: hook redirects/firewalls `+0x790` but does not block native BT/controller updates.
   - Clear follow keys while in combat: `ForceFollowLocation=false`; do not keep writing player FollowLocation.
   - Use reflected `MoveControllerToActor(ctrl, guard, target, 350.0f, true)` or direct controller helper once the detour direct-generic policy is fixed.
   - Maintain `IsAggressive=true` and target enemy during approach.
   - At `kHookFirstHitContactM`, stop/release movement once so native turning/root motion/abilities/attack montage own the final hit.
4. Dead target clear remains necessary but should be duplicate-throttled as already patched:
   - clear raw cached target every time if present
   - throttle expensive `SetTargetEnemy(nullptr)`/`SetBlackboardTargetEnemy(nullptr)`
   - log only first clear per stale target window

Expected combat logs after replacement:

- Rename `TwinCombatMercuna` to `TwinCombatRecast` / `TwinCombatController`.
- Include: `target`, `dist`, `status`, `routeIssued`, `moveRet`, `aggr`, `targetBB`, `mode`, `navMode`, `vel`, `pathFollower`, `cmc`, `faults`, `genericBypass`.
- Success is not just route accepted; success is distance closing and `vel>0` until melee release.

## 2026-06-26 16:06:15

ReVa confirmation: Mercuna function inside mixed-nav transition is mode/component state, not ground follow

- `0x141D493A0` calls `0x1410E9750` on the mixed character's `+0x1FA8` Mercuna navigation component.
- `0x1410E9750(nav, bool)` changes internal Mercuna component state and can call the component cancel vtable `+0x430` when disabling.
- It also updates component movement parameters/derived values around offsets `0x1C0..0x1D8` and writes state at component offset `300` (`0x12C`).
- This supports a clean separation:
  - Ground follow: controller/Recast/CharacterMovement, nav type 1.
  - Flight fallback: explicit mixed nav type 2 / Mercuna component state, only after confirmed ground path failure.
- Do not call `MercunaMoveToActor` every few hundred ms as the primary walking follow. That bypasses/competes with the Recast + CharacterMovement path and produces gliding/flying symptoms.

## 2026-06-26 16:06:36

File-location note

- Attempted to mirror live Markdown to `C:\Users\Skorcht\hook_twin_reversal.md`, but sandbox denied write access there without approval.
- Source of truth remains: `C:\Users\Skorcht\AppData\Local\Temp\ahm_reversal\hook_twin_reversal.md`.
- Server remains: `http://127.0.0.1:8765` with `GET /tail?n=120` and `POST /append`.

## 2026-06-26 16:06:57

Repo state at reversal pass

- Working tree was clean when checked after this reversal pass.
- Current commit: `4bb8bfa` (`Experimental hook Twin flight and target clear fixes`).
- No source edits were made during this ReVa/reversal-only pass.
- The notes in this file are intended to supersede the current Mercuna-based Hook Twin movement code for the next implementation agent.

## 2026-06-26 16:07:14

Tail summary / immediate next patch

Do:

- Replace Hook Twin follow and combat approach with controller/Recast walking.
- Install/register controller hooks for Hook Twins, but leave ownership/blocking false.
- Change registered Hook Twin `+0x790` detour from original-first fallback to direct generic-stage dispatch (`0x14486E2E0`) to skip unsafe Twin tail.
- Pin nav type 1 for ground follow using the real mixed-nav transition, then use reflected `AIController.MoveToActor(..., bUsePathfinding=true)` through the detour.
- Keep `IsAggressive=true` only while moving/combating as a locomotion gate, with target enemy null during pure follow and real target only during combat.
- Verify path follower is bound to CharacterMovement via `EnsureHookTwinGroundPathChain` and log it every few seconds.
- Treat flight as explicit fallback only; separate logs and no Mercuna spam during normal walking.

Do not:

- Do not keep `MercunaMoveToPlayer` as primary follow/combat approach for Twins.
- Do not raw-write velocity, AddMovementInput, or teleport as the fix.
- Do not reissue MoveTo every 300 ms while status is already Moving; use state transition/status/stall based reissue.
- Do not call the original Twin `+0x790` tail first for registered Hook Twins; an accepted result there can still leave movement visually dead.
- Do not clear/reseed dead targets every tick; duplicate-throttle stale target cleanup.

Primary code targets:

- `src/hooks/ai_movement_hooks.cpp::HkControllerMove`
- `src/features/features.cpp::DriveHookBodyguardsNativeGameThread` mixed Twin branch
- `src/features/features.cpp::DriveHookCombatApproach` mixed Twin branch
- Existing helpers to reuse: `MoveControllerToActor`, `EnsureHookTwinGroundPathChain`, `ForceGroundNavIfMixed`, `UnpinTwinSchedule`, `SetControllerAggressive`, `SetControllerForceFollow`, `SetControllerFollowLocation`, `SetControllerBoolKeyAt`, `DirectControllerMoveStatus`.

## 2026-06-26 post-test validation

Runtime test result from Hook Diagnostics: the Hook Twin controller/Recast path is now the validated movement solution. She follows, walks, closes gaps, and her AI/combat selection works. Do not replace this movement path with Mercuna, raw velocity, AddMovementInput, teleport, or flight-primary logic.

Current remaining issue is target lifecycle only: after an enemy dies, the Twin can keep standing over it and attacking the dead target. The fix belongs in the Hook bodyguard combat teardown, not movement. Dead targets must be rejected using both `Char_bIsDead` and health, and the clear path must blank cached/blackboard target plus run a throttled full stand-down (`SetBlackboardIsAggressive(false)`, `StopCharacterAggressive`, aggressive latch clear) so native attack state releases the corpse.

Known future gap: native Twin attack montages can look correct but still fail to damage robots. That is not a follow/AI movement failure. If it persists, add a separate external damage attribution layer keyed off confirmed Hook Twin attack/contact against a live target.

## 2026-06-26 Hook roster lifecycle note

Post-test issue: deleting a Hook bodyguard and spawning a new one could leave follow broken because the Hook tab had no dedicated delete manager and old per-actor maps survived the actor destruction. Cleanup must cover `g_hookNativeFollow`, `g_inject`, `g_engagedUntilMs`, `g_lastThreatMs`, `g_twin`, movement hook registration, controller ownership, saved schedule/original team bookkeeping, and references from other Hook guards. New Hook registration clears stale state for the same pointer before adding the actor, so UE pointer reuse cannot inherit old follow/combat state.

## 2026-06-26 Hook Twin downed-pose root cause

Discord correlation matched the pose to the campaign first-Twin injured/downed state. ReVa confirmed the root path is the reflected death/load-death/fight-staging/QTE pipeline, not the working Hook follow mover.

Relevant ReVa chain:

- `AHCharacterEventUtils.SendDeathEvent` (`0x1421EB000`) builds a character event with byte `8` and dispatches through `FUN_141DCC500`.
- `AHCharacterEventUtils.SendLoadDeathStateEvent` (`0x1421E9B70`) builds a character event with byte `0xE` and dispatches through the same event bus.
- `GameplayEventSubsystem.SendCharacterDiedEvent` (`0x14230C090`) broadcasts the gameplay death event with dead character, hit params, killer, and killed-by data.
- `QTESubsystem.CacheDeathPose` (`0x142371720 -> FUN_141F824A0`) caches the pose into QTE subsystem state and marks the cache valid.
- `AHAICharacter.TryActivateFightStagingAbility`, `LoadDeadState`, `K2_OnDeath`, `K2_OnLoadDeathState`, `QTESubsystem.StartVersusQTE`, and `AIDeathAbility.DestroyOwnerCharacter` are the reflected ProcessEvent choke points around that chain.

Patch direction: keep the validated controller/Recast walking logic unchanged. In Hook Diagnostics mode only, ProcessEvent blocks those death/QTE dispatches when the affected actor is a managed Hook Twin (`IsHookBodyguard && IsMixedNavCharacter`). The block also clears `Char_bIsDead`, tops health, keeps tick/mesh tick enabled, and arms a short `CacheDeathPose` suppression window because that specific QTE call has no actor parameter. Test logs to watch: `[AI-DEATH] blocked Hook Twin death pipeline ...` and `[AI-DEATH] blocked Hook Twin QTE/death-pose ...` with stage names.

## 2026-06-27 - Hook Twin Campaign Downed Pose Root Cause Patch

Static ReVa and SDK reversal showed the pose is not a generic death bit. The relevant path is `AHAICharacter::TryActivateFightStagingAbility` -> `UAIFightStagingAbility` vtable `+0x550` (`FUN_141B93A50`, RVA `0x1B93A50`), which writes the selected fight-staging object at ability offsets `+0x690/+0x6A0/+0x6A1`. The campaign Chelomey/Twin audio theme also consumes `FAHCharacterEvent` damage context and health, so explosion/damage history can select a story fight-stage before `K2_OnDeath`/`LoadDeadState` fire.

Implementation direction: stop treating reflected `TryActivateFightStagingAbility` as the root fix. The hook now installs runtime MinHook detours on the native selector (`0x1B93A50`) and action-container factory (`0x1CA06E0`). For managed Hook Twins it logs selected fight-staging objects and the action containers spawned after selection. It blocks only selected objects whose class/name matches Twin/Chelomey downed/final/death/QTE/finale tokens, leaving normal movement and normal combat stages alone. Test logs to watch: `[AI-FSTAGE] Hook Twin fight-stage select`, `[AI-FSTAGE] Hook Twin action-container create`, and `[AI-FSTAGE] BLOCKED Hook Twin campaign/downed fight-stage select`.
