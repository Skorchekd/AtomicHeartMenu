# Atomic Heart - Internal Mod Menu

An **internal** (injected DLL) trainer / mod menu for **Atomic Heart** (single-player,
Unreal Engine 4.26/4.27). It hooks the game's **DirectX 12** swapchain to draw a
[Dear ImGui](https://github.com/ocornut/imgui) overlay and uses a small UE4 runtime
SDK to read/write game objects and call any `UFunction` via `ProcessEvent` - the
mechanism that gives you "do anything" control.

> Single-player only. Atomic Heart has no multiplayer; this affects no other players.

---

## Project status and honest disclaimer

Read this before judging the repo.

- **This is a beta.** It is a work in progress, not a finished or polished release.
- **The current layout is rough.** The menu organization and the code structure are
  honestly kind of a mess right now and will keep changing. Do not expect clean
  architecture yet.
- **Written by an amateur developer.** Expect large bugs, half-finished features,
  dead code, and leftover artifacts from AI coding assistants. Use at your own risk
  and read the code before you trust it.
- **Single-player only.** Atomic Heart has no multiplayer, so nothing here affects
  other players. This is an unofficial, non-commercial fan tool.
- **The docs are AI assisted.** This README and `PROJECT_AND_SDK.md` were written
  with AI help because of the author's restrictive English. The AI turns rough notes
  into readable docs, so wording may sometimes describe intent more cleanly than the
  code actually delivers. Trust the code over the prose where they disagree.

### Credits and dependencies

- **Dear ImGui** by Omar Cornut (ocornut) for the overlay UI: https://github.com/ocornut/imgui
- **MinHook** by Tsuda Kageyu for function hooking: https://github.com/TsudaKageyu/minhook
- **Dumper-7** by Encryqed, used to dump the game SDK and offsets: https://github.com/Encryqed/Dumper-7
- **Atomic Heart** by Mundfish, the game itself. This project is not affiliated with,
  endorsed by, or supported by Mundfish.

The vendored Dear ImGui and MinHook sources keep their own upstream licenses.

### License

This project is licensed under the **GNU General Public License v3.0 or later
(GPL-3.0-or-later)**, with additional attribution terms under GPLv3 Section 7.
See [`LICENSE`](LICENSE) for the full license text and [`NOTICE`](NOTICE) for the
additional terms and the third-party credits.

In short:
- It stays open source. Use, study, modify, and share it freely.
- **Copyleft / share-alike**: any fork, modification, or redistribution must stay
  licensed under the same GPL-3.0-or-later terms and remain open source. You may
  not close it off or relicense it under more restrictive terms.
- **Attribution is required**: you must keep credit to the original author
  (Skorchekd) and to Dumper-7 (Encryqed), MinHook (Tsuda Kageyu), and Dear ImGui
  (ocornut). The vendored libraries stay under their own permissive licenses,
  which are GPL-compatible.

> Note: the dumped game SDK (`dumped-sdk/`) is **not** included in this repo. It is
> large, build-specific, and only needed as reference when adding new features. The
> menu builds and runs without it. Regenerate it by running Dumper-7 against your own
> copy of the game (see "Resolving the offsets" below).

---

## Detected on your machine

| | |
|---|---|
| Process | `AtomicHeart-Win64-Shipping.exe` (the launcher `AtomicHeart.exe` is separate) |
| Architecture | x64, base `0x140000000` |
| Render backend | **DirectX 12** (confirmed: `D3D12Core.dll` + `d3d11on12.dll` + DLSS loaded) |
| Toolchain present | VS 2022 Community, CMake, Git |

That's why the render hook targets DX12, not DX11.

---

## Architecture

```
src/
  dllmain.cpp          Entry: spawns worker thread, installs hooks, eject on END key
  core/
    globals.h          Shared atomics (running / menuOpen / sdkReady) + module info
    log.{h,cpp}        File + console logging  -> AtomicHeartMenu.log next to the exe
  hooks/
    dx12_hook.{h,cpp}  DX12 Present / ResizeBuffers / ExecuteCommandLists hooks (MinHook)
                       + ImGui DX12 backend, RTV/SRV heaps, command-queue capture
    wndproc_hook.{h,cpp}  Window subclass: feeds input to ImGui, INSERT toggles menu
  menu/
    menu.{h,cpp}       The ImGui window (themed: Player / Weapons / AI·Squad / World / Visuals / Render / Misc / Debug tabs)
  features/
    features.{h,cpp}   Per-frame cheat application (godmode, fly, speed, teleport...)
  sdk/
    ue4.{h,cpp}        Minimal UE4 SDK: FName/UObject/GObjects/GWorld + ProcessEvent
    scanner.{h,cpp}    AOB / signature scanner for resolving engine globals
    offsets.h          *** ALL build-specific offsets & signatures live here ***
tools/
    injector.cpp       Minimal LoadLibrary injector -> bin\injector.exe
deps/
    imgui/  minhook/   Vendored dependencies (already cloned)
```

**Controls:** `INSERT` opens/closes the menu. `END` ejects the DLL cleanly.
With **Fly** or **Noclip** enabled and the menu closed, movement is Minecraft-style
free-fly: `W/A/S/D` move relative to camera yaw, `SPACE` goes up, and `SHIFT` goes
down. **Noclip** additionally turns the player's collision off so you pass straight
through walls/floors (Fly keeps collision on). The `Movement x` slider controls
walk, fly, and noclip speed. Fly/Noclip also capture a return point when enabled
and, with **Fly streaming assist** on, periodically invalidate Atomic Heart's
streaming state so moved-to areas can load instead of leaving the pawn in an
unloaded void.

Maintenance rule: after each code or behavior change, update this markdown
and `PROJECT_AND_SDK.md` in the same pass so feature notes do not go stale.

---

## Build

Easiest - double-click **`build.bat`** (or):

```bat
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

Output:
- `bin\AtomicHeartMenu.dll` - the menu
- `bin\injector.exe` - the injector

The DLL uses the **static CRT** (`/MT`), so the target needs no VC++ redistributable.

---

## Run

1. Launch Atomic Heart, get into the game world.
2. Inject (run as admin):
   ```bat
   bin\injector.exe AtomicHeart-Win64-Shipping.exe bin\AtomicHeartMenu.dll
   ```
   (or use any injector - Xenos, Process Hacker → Inject DLL, etc.)
3. Press **INSERT** in-game. You should see the menu and, at the top, an
   **SDK: resolved** banner with a live object count.

If it says **SDK: NOT resolved**, the signatures in `offsets.h` didn't match this
patch - see below. The menu still draws (proving the render hook works); only the
game-interaction features are gated on SDK resolution.

A live console opens on injection for real-time copy/paste logging. The same lines
are also written to `AtomicHeartMenu.log` next to the game exe - check it first
when something doesn't work.

Performance note: hook enabling is batched through MinHook's queue and logged with
timings. SDK function/object resolving runs from the worker thread, not the DX12
`Present` hook. `FindObject()` first uses dumped `GObjects` index hints for known
functions, weapons, ammo, and streaming helper objects, then only falls back to a
scan if the build no longer matches those hints. Weapon/ammo assets are not bulk
prewarmed at injection. Per-frame `ProcessEvent` calls are kept feature-gated:
actor location is read only for coordinates/fly, infinite-ammo polling is
throttled, and overlay fence waits skip drawing instead of blocking the game.
Feature commands use a no-scan lookup path; if a dumped asset index is unstable,
a background object-name index resolves it without freezing the render thread.

---

## What works out of the box vs. what needs *your* offsets

This is the important part. **The render/input/menu layer is complete and game-agnostic.**
The game-interaction layer depends on values that are unique to your exact game build.

### Current wired features
These are wired for the Dumper-7 SDK currently in `dumped-sdk/`:
- **God mode** - freezes incoming damage multiplier at `0`, tops health, clears `bIsDead`;
  disabling restores the captured incoming-damage and health attribute values.
  If an old save/session already has the cheat value persisted, disabled state
  normalizes the damage multiplier back to `1.0`
- **One-hit kill** - boosts outgoing damage multiplier; disabling restores the
  captured multiplier. If an old save/session already has the cheat value
  persisted, disabled state normalizes it back to `1.0`
- **Infinite stamina / energy / air** - tops GAS attribute values every frame;
  disabling restores the captured resource attribute values
- **Fly** - `MOVE_Flying` plus manual camera-relative free-fly movement and
  Atomic Heart streaming invalidation/update assist; disabling restores the
  captured movement mode and fly speed
- **Noclip** - same free-fly movement as Fly plus
  `Function /Script/Engine.Actor.SetActorEnableCollision(false)` so the player
  passes through level geometry; disabling re-enables collision and restores the
  captured movement mode and fly speed
- **Speed** - writes `MaxWalkSpeed` / `MaxFlySpeed`; disabling restores captured walk speed
- **Show coordinates** - `Function /Script/Engine.Actor.K2_GetActorLocation`
- **Save / Teleport to position** - `Function /Script/Engine.Actor.K2_SetActorLocation`
- **Infinite ammo** - fills known ammo data assets into the player inventory,
  raises the inventory infinite-ammo/capacity fields, and tops the current
  weapon/EBBarrel loaded ammo; disabling restores captured inventory ammo-gate
  fields, disables the inventory overweight bypass, and restores current-weapon/
  EBBarrel fields. **Refill ammo now** forces the same refill path once and does
  not own toggle restore state
- **Misc → Instant puzzle resolve** - toggles Atomic Heart's own
  `DebugSubsystem.SetInstantPuzzleResolve` path AND continuously auto-completes
  the two interactive puzzle families the debug path does not cover:
  `BPC_MiniGameBase_C` minigames (dials/grids, electric lockpick - via
  `MiniGame_SetComplete`/`SetGameComplete`) and `BP_LockComponent_C` door locks
  (the CodeLock button grid, ColorsLockPick, CoinLock, UniversalLock - via
  `Unlock()`). Discovery (the GObjects sweep) runs on the **worker thread**, never
  the Present hook, so it does not freeze the game
- **Misc → Solve / pass current puzzle** - pulses instant puzzle resolve, calls
  `DebugSubsystem.InstantLockUnlock` plus `DebugSubsystem.WinQTE`, and queues a
  one-shot minigame/lock completion pass (it pops within a frame or two; on a
  miss it also logs the live puzzle-class candidates for diagnosis)
- **Misc → Unlock current lock / Win active QTE** - one-shot buttons for lock
  bypasses and active QTE passes
- **Give weapon** - dropdown grants one base weapon via `AAHPlayerCharacter::InstantTakeWeapon`
  and equips it; **Give all** queues one listed base weapon per tick instead of
  granting all in one blocking click. The Kalash rifle is listed as
  **Kalash Rifle / AK-47** (`DA_Item_AK47`)
- **AI / Squad tab** - full control over the AI (the headline feature):
  - **AI control roster** - a live list of nearby AI. **Tick** units to select them
    (selected + squad units get an in-world **glow box + arrow overhead**), then
    **Recruit sel.** / **Recruit all nearby** to add them to your **squad**, or
    **Attack >** / **Kill sel.** to dispatch them. Recruiting is now **explicit** - the
    old "bodyguard mode / follow me" auto-recruit toggles are gone, replaced by this
    managed roster.
  - **Squad** members **walk-follow** you and fight your threats. Follow now layers
    the game's AHAI blackboard escort keys, a guarded SDK `AIController.MoveToActor`
    fallback with a stop radius, and a small `Pawn.AddMovementInput` nudge for quest
    NPCs whose BT only turns to face you. They no longer teleport by default - enable
    **Allow teleport leash** if you want a far/stuck member to snap to you.
    **Invincible squad** (on by default) keeps them alive.
  - **Spawn** - **Model dropdown** lists the **live loaded enemy/boss types** (incl.
    the Twins when loaded); **Clone nearest** copies what you stand by. Spawns are
    **streamed** (one ~every 300 ms) so a squad never freezes the game.
  - **Saved characters** - **Save** records a type; **Spawn** brings it back **anywhere,
    with no proximity needed** - the class is **loaded on demand** (the old version
    needed you near the NPC and did a 6-second scan that froze the game). Same-session
    respawns are instant.
  - **Release selected / Stand down squad** turn units back into **normal, killable
    enemies** (forced Hostile to you through the engine - fixes "stuck invincible").
  - **Zone respawn** - **Snapshot zone** records the current enemies; **Respawn zone**
    brings that whole set back. Plus **Enemies fight each other**, **Freeze nearby**,
    **KILL ALL** / **LAUNCH ALL**.
  - (Giving guns to the squad is not wired - that needs AI weapon-loadout functions
    that aren't verified yet.)
- **Visuals → RGB gun** - rainbow/recolor your equipped weapon via its mesh material
  parameters (same engine path as chams; no GPU pipeline hook).
- **Debug → Dump nearby volumes** - stand where the game teleports you (e.g. the
  lighthouse edge) and press it; it logs nearby volume/trigger classes to the log so
  the out-of-bounds teleporter can be identified and disabled precisely.
- **Debug → dump objects** - logs the first 200 `GObjects` from a background
  thread so the render hook does not block

Runtime function names include `/Script/...`; keep the function constants in
`offsets.h` in that format or `FindFunction()` will miss them.

### Resolving the offsets - the path to *full* control
1. Inject **[Dumper-7](https://github.com/Encryqed/Dumper-7)** into the game once.
   It prints the `GObjects`, `GNames`, and `ProcessEvent` addresses **and** dumps a
   full C++ SDK of every Atomic Heart class/function.
2. Plug the globals into `offsets.h`:
   - Either fix the `SIG_*` AOB patterns, **or**
   - set `USE_STATIC_OFFSETS = true` and paste the RVAs (`addr - 0x140000000`).
3. For each game-specific cheat, open the dumped SDK, find the class
   (e.g. the weapon / health component), copy its property offset or
   `BlueprintCallable` `UFunction`, and read/write or `ProcessEvent` it in
   `features.cpp`. With the dumped SDK header included you can also call functions
   type-safely instead of by string.

`ProcessEvent` + `GObjects` is the whole game: anything the game can do to itself,
you can trigger from here once the SDK resolves.

---

## Notes & caveats
- Cheats are applied from the **render thread** (inside the Present hook). This is
  fine for actor functions; if a particular function must run on the game thread,
  hook the game's tick instead (`AActor::Tick` / `UWorld::Tick` from the dump).
- `VFUNC_PROCESSEVENT` (vtable index) and the `SIG_*`/member offsets in `offsets.h`
  are the first things to re-check after a game patch.
- To ship without the debug console, change `Log::Init(true)` to `false` in
  `dllmain.cpp`.
```
