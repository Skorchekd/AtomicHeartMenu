# Scanning vs. Hooking — diagnosis, what changed, what was left alone

This document is the result of reading the captured diagnostics snapshots in
`...\Binaries\Win64\AtomicHeartMenu_diagnostics\` together with the runtime log
(`AtomicHeartMenu.log`) and the full object map dump (`AtomicHeartMenu_gobjects.tsv`),
and using that evidence to make the menu lean on cached/native resolution instead of
brute-force memory scanning.

**TL;DR** — The menu's expensive part was never "finding objects by walking GObjects."
It was that the safety helper `Mem::IsReadable()` did a **`VirtualQuery` kernel syscall
on every single guarded read**, and a cold scan does *millions* of guarded reads. That
one function was the bottleneck behind every "slow scan" line in the log. It is now
cached. ProcessEvent is, and already was, a **real MinHook detour** — that part of the
menu's description is accurate.

---

## 1. What the snapshots/logs actually proved

### The ProcessEvent hook is real (not a false claim)
`Features::InstallProcessEventHook()` installs a genuine MinHook detour on the
`UObject::ProcessEvent` vtable slot (index `0x44`). The captured `trace.jsonl`
(e.g. `togglelogs1/trace.jsonl`) is that detour firing — it logged thousands of real
`AHAIController.SetBlackboardTargetAlly` / `Blackboard.SetValueAsVector` dispatches with
live object pointers and decoded params. The hook is used for three things today:

- **Game-thread dispatch** — drain the deferred task queue at a proven-safe callsite
  (outermost, non-AI object) so spawns / AI mutation never run on the render thread.
- **No-save guard** — swallow `SaveProgress` / `SavePersistentData` / `CheckpointObject.SaveProgress`
  while a Horde arena is live (pure pointer compares).
- **Diagnostics trace** — the `trace.jsonl` capture above.

So "hooking ProcessEvents" is accurate. What was *not* hook-driven — and what the user
correctly flagged as "scanning from memory" — is **object/function resolution** and the
**AI/ESP actor sweep**. Those used GObjects iteration, and iteration was being throttled
by per-access syscalls.

### The real cost was `VirtualQuery`, not iteration
From `AtomicHeartMenu.log` (single session, this build):

| Operation | Objects walked | Time (before) | Dominated by |
|---|---:|---:|---|
| `FindObject` (cold, per function) | 92,271 | **656–906 ms each** | `VirtualQuery` |
| Object name index build (1×, background) | 92,271 | **3,985 ms** | `VirtualQuery` |
| FName pool index build (1×, background) | 675,536 entries | **1,625 ms** | page walk + decode |
| Full object map dump (button) | 281,979 rows | **55,375 ms** | `VirtualQuery` |
| AI refresh (every ~5 s, worker thread) | ~9,960 | 15–16 ms | `VirtualQuery` |

Prewarm alone fired ~6 of those cold `FindObject` scans back-to-back at startup
(LaunchCharacter, FullUpgrade, Promote/CompleteAllActiveQuests, MoveToActor,
SimpleMoveToActor) ⇒ multiple seconds of worker-thread churn before caches warmed.

`Mem::IsReadable()` ran a `VirtualQuery` every call, and `GetObjectByIndex` + `GetName`
+ `GetFullName` call it several times per object. 92k objects × ~5 calls ≈ **half a
million syscalls per cold scan** — which is precisely the ~700 ms measured.

### GObjects indices for `/Script` functions are stable across launches
The four full snapshots were captured hours apart in different sessions. Every
`/Script` (engine-native) UFunction kept an **identical GObjects index** in all of them
(`MoveToActor` = 14173, `SetBlackboardTargetAlly` = 14193, …) and the `process_event_target`
RVA was rock-stable at **`0x274bc80`**. That is what makes index hints safe to hard-code
(validated before use) and what the menu already does for ~120 known objects.

---

## 2. What was changed (high confidence, no feature behavior change)

### (A) `Mem::IsReadable` region cache — `src/core/memory.cpp`  ★ the big one
A per-thread cache of recently-seen **committed + readable** memory runs
(`[BaseAddress, BaseAddress+RegionSize)` straight from the `MEMORY_BASIC_INFORMATION`).
A query whose whole range falls inside a cached run returns `true` with a couple of
compares — no syscall. Cache misses behave **byte-for-byte identically** to the old
function.

- **Positive-only**: never caches a "not readable" answer, so it can never wrongly
  reject a live address beyond the single call that saw it.
- **Short TTL (250 ms)** + **`/EHa` backstop**: the only risk is the game freeing a run
  we cached as readable inside the TTL; that fault is already the menu's normal
  wrong-offset failure mode and is caught by the `/EHa` `try/catch` firewalls (TickImpl
  + per-object scan guards) → skipped object, never a crash.
- **Thread-local**: no locks; render / worker / index threads each keep their own view.

Expected effect: cold `FindObject` and the index/dump scans drop by **>10×** (the syscall
storm becomes a handful of queries against ~32 hot heap segments). The per-frame property
reads in `TickImpl` also stop syscalling.

### (B) Six index hints added — `src/sdk/ue4.cpp` (`kObjectIndexHints`)
The exact `/Script` functions the log showed Prewarm cold-scanning are now hinted with
their snapshot/TSV-verified indices, so they resolve by **direct index lookup** instead of
a full sweep — even on a stone-cold cache before the name index exists:

```
Engine.Character.LaunchCharacter                       0x33EF (13295)
AIModule.AIController.MoveToActor                       0x375D (14173)
AtomicHeart.BaseWeapon.FullUpgrade                     0x3E6D (15981)
AtomicHeart.DebugSubsystem.CompleteAllActiveQuests     0x407E (16510)
AtomicHeart.DebugSubsystem.PromoteAllActiveQuests      0x40A9 (16553)
AIModule.AIBlueprintHelperLibrary.SimpleMoveToActor    0x55B4 (21940)
```

Each is still validated by full-name match before use (`TryObjectIndexHint`), so a game
patch that shifts indices simply falls back to the (now fast) scan — never a misresolve.

### (C) `FindObject` scan loop hardened — `src/sdk/ue4.cpp`
Wrapped the per-iteration body in `try/catch` so a slot freed during a scan (the region
cache's small staleness window) skips that one object instead of aborting the whole
resolve. This matches the per-object guards already present in `BuildNameIndexThread`,
`WriteObjectMapDump`, `RefreshGlobalAiDiscovery`, and `RefreshAiActors`.

---

## 3. What was deliberately NOT changed (held back on purpose)

> Per the brief: only convert to a native hook where it can be done confidently without
> crashing or perf regressions. These did not meet that bar, or were already optimal.

### ✗ Do NOT replace the ProcessEvent vtable read with the hard-coded RVA `0x274bc80`
The RVA is stable for *this* build, but reading `vt[0x44]` off a live anchor object is
**more robust** — it survives a game patch that moves the function, costs one read at
install time, and is not on any hot path. Hard-coding the RVA would trade real
patch-resilience for zero measurable gain. Keep the vtable read.

### ✗ Do NOT (yet) replace the AI/ESP GObjects sweep with a ProcessEvent-fed live registry
The most "advanced" version of this request is to stop sweeping GObjects for AI actors and
instead **harvest live actor pointers from the ProcessEvent hook** (every AI dispatches
UFunctions through it). It is genuinely attractive and the trace proves the data is right
there. It was held back because:

- **Dead-actor removal is the hard half.** There is no destructor hook; a harvested set
  needs liveness validation on read, which reintroduces the guarded-read cost the cache
  just removed.
- With the region cache, the sweep that motivated this is now ~15 ms → low-single-digit ms
  on a worker thread every ~5 s. The payoff shrank below the risk.

It is the right *next* step if profiling still shows the sweep mattering — see §4 for how
to drive it from a fresh capture. Until then: not worth the crash surface.

### ✗ Globals (GObjects/GNames/GWorld) — already optimal
`USE_STATIC_OFFSETS = true`: these are hard-coded RVAs from the Dumper-7 dump, resolved
once at `O(1)`. Nothing to scan, nothing to hook.

### ✗ Player chain (World→GameInstance→LocalPlayers[0]→PlayerController→Pawn) — already optimal
Pure guarded pointer-walks via the offsets in `offsets.h`. No scan. (And now syscall-free
per frame thanks to the cache.)

---

## 4. How to use the memory-snapshot function to drive future hooking

The diagnostics capture is your "what is real right now" oracle. Workflow:

### Capture
- **Debug tab → "Write full snapshot"** → one `snapshot_full.json` bundle
  (`engine`, `player`, `weapon`, `world_snapshot`, `ai`, `function_pointers`).
- **Debug tab → "Live diagnostic capture (togglelogsN)"** → rolling `snapshot_NNNN.json`
  frames **plus `trace.jsonl`** (the ProcessEvent firehose) while the toggle is on.
- **"Set trace target" + a name substring** (e.g. `Larisa`, `Twins`) → narrows
  `trace.jsonl` to every UFunction fired on that actor + its controller, with raw params.
- **"Dump targeted actor (full reflection)"** → complete field set of any actor by name,
  **plus detour data**: vtables, every component vtable (incl. Mercuna nav), and
  module+RVA for each — i.e. the addresses you would feed a native detour.
- **"Dump objects"** → `AtomicHeartMenu_gobjects.tsv` (`index ⇥ ptr ⇥ class ⇥ name ⇥ full_name`).

### Turn a capture into a hook / a scan-elimination
1. **Confirm stability before trusting a number.** Diff the same field across ≥2 snapshots
   from **different game sessions** (as in §1). Stable across sessions ⇒ safe to hard-code.
   - `function_pointers[*].index` stable ⇒ add an **index hint** to `kObjectIndexHints`
     (kills the cold scan, like §2-B). Grab the index from the TSV:
     `grep "Function /Script/<Pkg>.<Class>.<Fn>$" AtomicHeartMenu_gobjects.tsv`.
   - `process_event_target.rva` (or any function RVA from the targeted dump) stable ⇒
     candidate for a **sig** in `offsets.h` (`USE_STATIC_OFFSETS` path) or a MinHook detour.
2. **To force a behavior natively (e.g. follow/aggro), read `trace.jsonl`** for the exact
   UFunction the game itself calls and its params, then either call that UFunction via
   `ProcessEvent` (current approach — cheap, already game-thread-safe) or detour it. The
   trace already revealed the follow path: `SetBlackboardFollowLocation` +
   `SetValueAsVector(FollowLocation/CurrentWaypoint)` + `ForceFollowLocation=true`.
3. **Always keep the validated fallback.** Every hard-coded index/RVA must be re-checked
   at runtime (full-name match for objects; sanity-checked bytes for a sig) so a game
   patch degrades to a scan, never a misresolve into garbage. This is the existing
   `TryObjectIndexHint` contract — preserve it for anything new.

### Refreshing the hints after a game update
1. Inject, open the Debug tab, hit **"Dump objects"**.
2. For each hinted entry, re-grep its index in the new TSV; update the hex in
   `kObjectIndexHints` if it moved. Wrong/stale hints are harmless (validated) but slow
   (they force the fallback scan), so refreshing keeps startup instant.

---

## 5. Stability fix — the menu-open / random freeze (game thread hangs, audio keeps playing)

**Symptom (from the log):** the game thread (the one logging `SquadFollow(GT)` /
`SquadCombat(GT)` / `Twin nav` — thread T7780) goes permanently silent after the menu is
opened, while the worker thread (T15620, `AI refresh`) keeps logging and audio keeps
playing. Not a crash — a hang. It also happened occasionally at random, and predates the
optimization changes above.

**Root cause:** the game thread spinning forever inside the engine's own AI code after we
mutate AI/blackboard state *while the engine is mid-AI-iteration* — the exact failure the
ProcessEvent pump's `PeDispatchingOnAi(obj)` guard exists to prevent. The bug: the
**per-frame squad follow drive** (`DriveSquadVelocityGameThread`, called directly from
`hkProcessEvent`) writes blackboard keys via ProcessEvent on the AI controller
(`SetControllerTargetAlly` / `SetControllerFollowLocation` / `SetController…`), but unlike
the queued combat-injection drain it was gated **only** by "outermost + game thread +
squad>0" — it was **missing the `!PeDispatchingOnAi(obj)` gate**. So whenever the engine
dispatched a behaviour-tree task/service/decorator event at top level (a callsite where it
is actively iterating AI state) and our 20 Hz follow tick fired in the same window, the
blackboard write corrupted that iteration → infinite spin on the game thread → hard freeze.
The captured `trace.jsonl` shows exactly this stream of `SetBlackboardTargetAlly` /
`Blackboard.SetValueAsVector` calls on `BP_TwinsController_C_0`. Opening the menu shifts
frame pacing and the mix of dispatched UFunctions, which is why it raised the odds of
landing in the unsafe window (and why it felt "menu-triggered").

**Fix (`src/features/features.cpp`, `hkProcessEvent`):** add the same `!PeDispatchingOnAi(obj)`
gate to the follow drive that the combat drain already uses. `PeDispatchingOnAi` is
evaluated only after the 20 Hz throttle is ready (so it stays cheap), and if the current
callsite is unsafe the drive simply waits for the next safe one — these occur many times
per frame, so follow responsiveness is unchanged. After this, **every** AI-ProcessEvent
injection on the game thread (follow drive + the queued `DrainAiGameThreadWork`) is gated;
the render/menu side only ever does raw guarded reads (`ReadActorLocationFast`, cached
fields, `GetName`), never an AI ProcessEvent, so the menu cannot trigger the spin either.

> Audited and ruled out while finding this: the DX12 Present hook holds no lock across its
> D3D calls and its hot-path fence wait is non-blocking (timeout 0); `Mem::IsReadable`'s new
> cache is thread-local; `AiNearbyList` / `BuildEspFrame` / `RefreshAiRenderLocations` are
> raw-read only; the squad/combat drivers copy their rosters under a brief lock and release
> before doing work (no nested `g_aiMutex`/`g_squadMutex` ordering, so no AB-BA deadlock).

**Still worth watching (not changed — would need its own testing):** ImGui's Win32 message
handler runs on the game thread (WndProc) while `ImGui::NewFrame`/`Render` run on the render
thread. ImGui is single-threaded; this is the most likely remaining source of the *random
crash-on-open* (as opposed to the hang fixed here). It usually crashes rather than hangs, and
fixing it means marshalling input to the render thread — a bigger change held back for now.

## 6. File-by-file change list

| File | Change | Risk |
|---|---|---|
| `src/core/memory.cpp` | Per-thread VirtualQuery region cache in `IsReadable` (positive-only, 250 ms TTL) | Low — miss path identical; AV backstop unchanged |
| `src/sdk/ue4.cpp` | +6 validated `/Script` index hints; `try/catch` per-slot guard in `FindObject` | Low — hints validated; guard only adds safety |
| `src/features/features.cpp` | Gate the per-frame squad follow drive behind `!PeDispatchingOnAi(obj)` (freeze fix) | Low — same proven guard as the combat drain; closes a corruption window |
| `OPTIMIZATION_HOOKS.md` | This document | None |

No offsets, no hook wiring, and no ProcessEvent vtable/RVA were modified. The freeze fix
*tightens* an existing safety gate rather than adding new behavior. Build is unchanged
(`/EHa` already required and present).
