// SPDX-License-Identifier: GPL-3.0-or-later
//
// Atomic Heart Menu - internal mod menu for single-player Atomic Heart.
// Copyright (C) 2026 Skorchekd
//
// This program is free software: you can redistribute it and/or modify it under
// the terms of the GNU General Public License as published by the Free Software
// Foundation, either version 3 of the License, or (at your option) any later
// version. Distributed WITHOUT ANY WARRANTY. See the LICENSE file for details.
//
// Additional terms (GPLv3 Section 7): you must preserve attribution to the author
// (Skorchekd) and to Dumper-7 (Encryqed), MinHook (Tsuda Kageyu), and Dear ImGui
// (ocornut). See LICENSE and NOTICE. Forks must stay GPL-3.0-or-later and open.
#pragma once
#include "../sdk/ue4.h"
#include <cstddef>
#include <vector>
#include <string>

// Central feature state + per-frame application. The render hook calls
// Features::Tick() once per presented frame (after the SDK is ready).
namespace Features
{
    struct WeaponEntry
    {
        const char* label;
        const char* objectName;
    };

    struct State
    {
        // Engine-generic (work on any UE4 game once globals resolve)
        bool  godMode      = false;   // zero incoming damage and keep health topped
        bool  showCoords   = false;   // read pawn world location
        bool  teleportSaved= false;   // teleport to saved point (button-driven)

        // Movement (uses CharacterMovementComponent; offsets in offsets.h)
        bool  flyHack      = false;
        bool  noclip       = false;   // free-fly + actor collision off (pass through walls)
        bool  flyStreamingAssist = true;
        bool  speedHack    = false;
        float speedMult    = 2.0f;
        bool  superJump    = false;   // scale JumpZVelocity
        float jumpMult     = 2.5f;
        bool  lowGravity   = false;   // scale GravityScale
        float gravityScale = 0.4f;

        // Resources (UAHBaseCharacterAttributeSet)
        bool  infiniteStamina = false;
        bool  infiniteEnergy  = false;
        bool  infiniteAir     = false;

        // Combat
        bool  oneHitKill   = false;
        bool  infiniteAmmo = false;

        // World
        bool  timeDilation = false;   // UGameplayStatics::SetGlobalTimeDilation
        float timeScale    = 1.0f;
        bool  bulletTime   = false;   // matrix mode: world slows, you stay fast
        float bulletTimeScale = 0.30f;

        // Spectacle / fun
        bool  customScale  = false;   // resize the player (giant / tiny)
        float playerScale  = 1.0f;

        // Camera
        bool  customFov      = false;      // override camera FieldOfView (stretched FOV)
        float fovValue       = 90.0f;      // degrees

        // Visuals / ESP (drawn from the render hook via ImGui draw lists)
        bool  espEnabled     = false;
        bool  espBox         = true;
        bool  espFilled      = true;       // filled "chams" box (solid fill seen through walls)
        float espFillAlpha   = 0.25f;      // 0..1 fill opacity
        bool  espCornerBox   = false;
        bool  espSnapline    = false;
        bool  espHealthbar   = true;
        bool  espDistance    = true;
        bool  espRainbow     = true;       // animate ESP/crosshair colour through the spectrum
        float espColor[3]    = { 0.20f, 0.85f, 1.00f };
        float espMaxDistance = 300.0f;     // metres
        bool  crosshair      = false;
        float crosshairColor[3] = { 0.10f, 1.00f, 0.40f };

        // --- Render hijack (engine render-object overrides, not GPU draw hooks) ---
        // Chams: recolor enemy character models by overriding their mesh material
        // parameters + emissive, optionally flagging custom-depth for see-through.
        bool  chamsEnabled      = false;
        bool  chamsRainbow      = true;       // cycle the model colour through the spectrum
        float chamsColor[3]     = { 1.00f, 0.10f, 0.10f };
        float chamsEmissive     = 6.0f;       // glow strength (makes the chams POP)
        bool  chamsThroughWalls = true;       // also set custom depth/stencil (best-effort x-ray)

        // World/sky tint: RGB every light in the level (the sun tints the whole
        // sky + scene). Disable resets lights to white (exact originals come back
        // on area reload).
        bool  worldTint         = false;
        bool  worldTintRainbow  = true;
        float worldTintColor[3] = { 0.35f, 0.10f, 1.00f };
        float worldTintCycle    = 0.35f;      // rainbow cycle speed

        // World AI controls. Radius is in metres; commands are queued and
        // drained over frames so large groups do not stall Present.
        bool  aiFreezeNearby = false;
        float aiRadius       = 80.0f;
        int   aiSpawnModel   = 0;

        // Squad / control model. Recruiting is now EXPLICIT (a managed roster), not
        // an auto-recruit toggle -- see the squad API below. Members always follow +
        // protect you.
        bool  aiInvincibleAllies = true;  // keep squad members topped to full health
        bool  aiAllowTeleport    = false; // OFF by default: the squad WALKS to you; only snap-teleport if a member is stuck
        float aiFollowStopM      = 1.0f;  // squad stops this far from you (customizable; clean, no circling)
        // Squad combat injection (SetCharacterAggressive state machine). OFF by default
        // because it CRASHES on non-combat NPCs (e.g. Larisa) that have no combat AI --
        // turn it on only for a squad of real combat enemies (robots). When off, the
        // squad still follows + stays friendly (fully crash-safe).
        bool  aiSquadAggressive  = true;  // squad fights threats for you (now crash-safe: combat-capability gated)
        bool  aiFightEachOther   = false; // crowd-control: make non-squad enemies brawl each other

        // RGB / recolor the player's equipped weapon via original-material MIDs,
        // with a forced visible parent only for texture-locked slots.
        bool  weaponRgb       = false;
        bool  weaponRgbRainbow= true;
        float weaponRgbColor[3] = { 1.0f, 0.2f, 0.05f };

        // Misc/debug helpers
        bool  instantPuzzleResolve = false;
        bool  debugLiveDump = false; // Debug tab: stream togglelogsN snapshots + ProcessEvent trace

        // Saved teleport point
        UE::FVector savedLocation{};
        bool        hasSaved = false;
        UE::FVector flyStartLocation{};
        bool        hasFlyStart = false;
    };

    State& Get();

    // One ESP target already projected to screen space (render-thread output).
    struct EspEntry
    {
        float x, y, w, h;       // screen box: top-left corner + size
        float feetX, feetY;     // bottom-centre, for snaplines
        float headX, headY;     // top-centre, for the "controlled" arrow
        float distance;         // metres from camera
        float healthFrac;       // 0..1, or <0 when unknown
        bool  inSquad;          // under our control -> glow + arrow
        bool  selected;         // UI-selected -> stronger highlight
    };

    // One row of the "AI under our control / nearby" list shown in the menu.
    struct AiListEntry
    {
        unsigned long long id;  // (uintptr_t)actor -- stable handle for select/dispatch
        float distanceM;
        float healthFrac;       // <0 unknown
        bool  selected;
        bool  inSquad;
        std::string name;
    };

    void Prewarm();           // resolve UFunctions on the worker thread (off the render path)
    void WorkerTick();        // worker-thread heartbeat: discover puzzles + refresh ESP actors
    void Tick();              // called every frame from the Present hook

    // Authoritatively pin the GAME thread id. Call from the WndProc hook: UE4 pumps
    // window messages on the game thread, so the WndProc thread IS the game thread.
    // This stops the ProcessEvent pump from ever latching a loading/audio worker
    // (which would run a spawn off the game thread -> the AnimInstance token-stream
    // fatal). Authoritative + immediate, so the very first spawn lands correctly.
    void NoteGameThread();

    // Render-thread: project the cached enemy list for the given screen size.
    // Returns a reference to an internal buffer (single-threaded render use).
    const std::vector<EspEntry>& BuildEspFrame(float screenW, float screenH);
    void FullHeal();          // top the player's health attribute now
    void SavePosition();      // capture current pawn location
    void TeleportToSaved();   // move pawn to saved location
    void ReturnToFlyStart();  // recovery point captured when fly is enabled
    void RefillAmmoNow();     // force inventory + current weapon ammo refill
    void SolveCurrentPuzzle(); // instant lock/puzzle resolve + QTE win pass
    void UnlockCurrentLock();  // call Atomic Heart debug instant lock unlock
    void WinCurrentQTE();      // call Atomic Heart debug QTE win
    void SkipObjective();      // advance every active quest one step (skip current objective)
    void CompleteActiveQuests(); // mark every active quest fully complete
    bool GiveWeapon(int index, bool equip); // grant one weapon data asset
    int  GiveAllWeapons(bool equipLast);    // grant every listed weapon
    int  AiCachedCount();
    int  AiPendingCount();
    int  AiQueueKillNearby();
    int  AiQueuePassiveNearby(bool passive);
    int  AiQueueFollowPlayer();
    int  AiQueueFightEachOther();
    int  AiQueueReleaseNearby();
    int  AiQueueKillAll();        // wipe every cached enemy in the level
    int  AiQueueLaunchAll();      // ragdoll-launch every cached enemy skyward
    bool AiSpawnBodyguard();      // clone + spawn the nearest live enemy as a guard (streamed)
    bool AiSpawnModel(int index); // spawn the dropdown-selected LIVE model as a guard (streamed)
    int  AiSpawnedAllyCount();    // how many spawned allies are currently alive
    int  AiSpawnQueueCount();     // how many spawns are still waiting in the stream

    // Global model search: every loaded character TYPE (not just nearby), filtered by
    // a text query. Spawn any of them by name as a streamed bodyguard (combat-vs-
    // non-combat is auto-handled by the same injection used for Larisa/robots).
    std::vector<std::string> AiSearchModels(const char* query, int maxResults);
    bool AiSpawnModelByName(const char* prettyName);
    // Deep enemy sweep kill: catches enemies the level-walk cache misses (the
    // "uncatchable, unkillable, still attacking" edge case) via a full GObjects sweep.
    int  AiKillAllDeep();
    // Delete an actor from the world outright (K2_DestroyActor), by roster id.
    void AiDeleteActor(unsigned long long id);

    // Saved-character DB (persisted to AtomicHeartMenu_bodyguards.json).
    bool AiSaveNearestCharacter(const char* displayName); // capture nearest enemy class
    bool AiSpawnSavedCharacter(int index);                // re-spawn a saved character
    int  AiSavedCharacterCount();
    std::vector<std::string> AiSavedCharacterNames();
    bool AiDeleteSavedCharacter(int index);
    void AiClearSpawnedAllies();  // stand down + forget every spawned ally

    // ---- Squad / control roster (the AI-control core) ----------------------
    // The squad = every AI under our control (spawned + recruited). Members always
    // follow + protect you; recruiting is explicit (no auto-recruit toggle).
    int  AiSquadCount();
    std::vector<AiListEntry> AiNearbyList(int maxCount); // nearby AI for the menu list
    void AiToggleSelect(unsigned long long id);
    void AiSelectAllNearby();
    void AiClearSelection();
    int  AiSelectedCount();
    void AiRecruitSelected();     // selected -> squad
    void AiRecruitNearby();       // all nearby -> squad
    void AiSaveSelected();        // save selected units' classes to the model DB (json)
    void AiReleaseSelected();     // selected -> normal killable enemies
    void AiReleaseSquad();        // whole squad -> normal killable enemies
    void AiDispatchAttack();      // selected (or squad) attack your aim/nearest enemy
    void AiDispatchKill();        // selected (or squad) die
    // Zone respawn: snapshot the current enemies, respawn that set later.
    void AiSnapshotZone();
    void AiRespawnZone();
    int  AiZoneSnapshotCount();
    // Diagnostics: dump nearby volumes/triggers (to identify out-of-bounds teleporters).
    void DumpNearbyVolumes();
    // Diagnostics: one-shot full state bundle + live toggle log status.
    void DebugDumpGameSnapshot();
    // Discovery: full named-field reflection dump of every loaded actor whose
    // class/name/path contains nameSubstr (e.g. "Larisa"), incl. its controller +
    // movement component. Runs on a worker thread (never freezes the game).
    void DebugDumpTargetedActor(const char* nameSubstr);
    // Universal ProcessEvent trace: latch the nearest actor matching nameSubstr
    // (and its controller) so every UFunction dispatched on it is logged (function
    // full name + raw params) while live capture is on. Empty string clears it.
    void DebugSetTraceTarget(const char* nameSubstr);
    const char* DebugTraceTargetName();  // currently latched target's full name ("" if none)
    const char* DebugLastDumpDir();

    void MaxWeaponUpgrades();     // BaseWeapon::FullUpgrade on the current weapon
    void RunConsoleCommand(const char* command); // queue a console command to the game thread
    // Spawn model dropdown = the LIVE enemy classes currently loaded (deduped),
    // so every entry is guaranteed safe to spawn. Refreshed ~1/sec internally.
    const char* AiSpawnModelName(int index);
    int  AiSpawnModelCount();

    // Last-read pawn world location for the on-screen readout.
    UE::FVector LastLocation();

    const WeaponEntry* WeaponList();
    int WeaponCount();
}
