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
#include "features.h"
#include "../sdk/offsets.h"
#include "../sdk/reflect.h"
#include "../sdk/scanner.h"
#include "../core/globals.h"
#include "../core/log.h"
#include "../core/memory.h"
#include "../hooks/native_hooks.h"
#include "../hooks/ai_movement_hooks.h"
#include <Windows.h>
#include <MinHook.h>
#include <cmath>
#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <string>
#include <mutex>
#include <vector>
#include <atomic>
#include <functional>
#include <fstream>
#include <iterator>
#include <sstream>
#include <iomanip>
#include <cstdio>
#include <cstring>
#include <cctype>

using namespace UE;
namespace AH = Offsets::AH;

// Defined at file scope below (after the anonymous namespace), but called from inside
// it (hkProcessEvent) -- declare it here so the anon-namespace call resolves to this
// one definition. Per-frame, game-thread squad walk (RequestDirectMove emulation).
void DriveSquadVelocityGameThread();

// Mercuna MoveToActor (the game's real pather for the normal/non-hook follower).
// The Hook Diagnostics Twin path below deliberately uses controller/Recast instead.
static bool MercunaMoveToPlayer(UObject* ai, UObject* target, float endDistU, float speed);
static bool MercunaStop(UObject* ai); // cancel her current Mercuna move (so she cleanly holds / stops stacking)
static UObject* GetMercunaNavComp(UObject* ai);
static void ReleaseHookNativeMovement(UObject* guard, bool stop);
static void ForgetHookBodyguardRuntimeState(UObject* guard, bool eraseReleaseBookkeeping);

namespace
{
    Features::State g_state;
    FVector         g_lastLoc{};

    // ---- Game-thread dispatch via a ProcessEvent hook ----------------------
    // Our cheats run inside the DX12 Present hook = the RENDER thread. That's
    // fine for property reads/writes and simple UFunction calls, but structural
    // engine work like SpawnActor and ALL AI behaviour-tree / blackboard / team
    // mutation MUST run on the GAME thread or it races the game's own actor / AI
    // iteration and corrupts it. So we hook UObject::ProcessEvent (one function,
    // shared by every class at vtable idx 0x44) purely to borrow the game thread:
    // when the game calls ProcessEvent we drain a small task queue there. The
    // detour is a couple of instructions when idle.
    //
    // WHY THE GAME KEPT FREEZING (and the guard below):
    // Borrowing the game thread is NOT enough on its own. ProcessEvent fires for
    // EVERY UFunction the engine dispatches -- including ones it dispatches from
    // *inside* the AI update: a behaviour-tree service/task, a montage notify, an
    // enemy's blueprint Tick. Draining our AI-injection tasks at one of those
    // callsites calls SetTargetEnemy / SwitchTeam / SetCharacterAggressive on the
    // AI while the engine is mid-iteration over that very state -> it corrupts the
    // state and the game thread spins forever inside engine AI code = the hard,
    // permanent freeze (continuous Fight / Bodyguard toggles made it reliable).
    //
    // A callsite is only safe to borrow when BOTH hold:
    //   1. OUTERMOST -- this is the first ProcessEvent on the thread's stack
    //      (t_peDepth == 0 on entry); we never drain while nested inside another
    //      UFunction dispatch.
    //   2. NON-AI -- the object being dispatched is not an enemy AI character or
    //      AI controller, so the engine is not currently executing AI logic.
    // Safe callsites occur many times every frame (player pawn, UMG, GameMode,
    // timers), so the tasks still run within a frame -- just never mid-AI-tick.
    typedef void (*ProcessEvent_t)(void*, void*, void*); // x64: this=RCX, fn=RDX, parms=R8
    ProcessEvent_t            oProcessEvent = nullptr;
    void*                     g_peTarget = nullptr;
    // Trampoline for AHAICharacter::TryActivateFightStagingAbility native detour.
    // Ghidra reversal at RVA 0x1B93A50: void(AHAICharacter*, void* params, bool flag).
    using TryFightStagingFn = void(__fastcall*)(void* thisptr, void* params, bool);
    TryFightStagingFn         g_oTryFightStaging = nullptr;

    // Trampoline for action-container factory (RVA 0x1CA06E0). Logs containers
    // spawned after fight-staging selection on Hook Twins.
    using ActionContainerFactoryFn = void*(__fastcall*)(void* thisptr, void* params);
    ActionContainerFactoryFn  g_oActionContainerFactory = nullptr;

    // Downed/final-stage token list. If the selected fight-staging object's
    // name contains any of these (case-insensitive), the native is blocked.
    // Normal movement / combat stages are allowed through.
    constexpr const char* kDownedTokens[] = {
        "twin", "chelomey", "downed", "final", "death", "qte", "finale",
    };

    static bool MatchesDownedToken(const std::string& name)
    {
        std::string lower = name;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char c){ return std::tolower(c); });
        for (const char* tok : kDownedTokens)
        {
            if (lower.find(tok) != std::string::npos)
                return true;
        }
        return false;
    }

    // Walk the character's outer chain + AbilitySystemComponent to find the
    // UAIFightStagingAbility instance and read the selected-object pointer
    // stored at offsets +0x690 / +0x6A0 / +0x6A1 (per ReVa reversal).
    static void ClearFightStagingAbilityObject(UObject* ab)
    {
        if (!ab || !Mem::IsReadable(ab, 0x6A2))
            return;
        uint8_t* b = reinterpret_cast<uint8_t*>(ab);
        *reinterpret_cast<void**>(b + 0x690) = nullptr; // selected object
        *reinterpret_cast<uint8_t*>(b + 0x6A0) = 0;     // selected/active flags from ReVa reversal
        *reinterpret_cast<uint8_t*>(b + 0x6A1) = 0;
    }

    static bool ClearFightStagingSelectedObject(UObject* character)
    {
        bool cleared = false;
        if (!character || !Mem::IsReadable(character, 0x30))
            return false;

        auto maybeClear = [&](UObject* ab)
        {
            if (!ab || !Mem::IsReadable(ab, 0x30) || !ab->Class())
                return;
            std::string name = ab->Class()->GetName();
            if (name.find("FightStaging") != std::string::npos)
            {
                ClearFightStagingAbilityObject(ab);
                cleared = true;
            }
        };

        if (Mem::IsReadable(character, 0x738 + sizeof(void*)))
        {
            UObject* asc = *reinterpret_cast<UObject**>(reinterpret_cast<uint8_t*>(character) + 0x738);
            if (asc && Mem::IsReadable(asc, 0x130))
            {
                int count = *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(asc) + 0x130);
                void** arr = *reinterpret_cast<void***>(reinterpret_cast<uint8_t*>(asc) + 0x128);
                if (arr && count > 0 && count < 1024 && Mem::IsReadable(arr, sizeof(void*) * (size_t)count))
                    for (int i = 0; i < count; ++i)
                        maybeClear(reinterpret_cast<UObject*>(arr[i]));
            }
        }

        UObject* o = character->Outer();
        for (int d = 0; o && d < 64; ++d)
        {
            maybeClear(o);
            o = o->Outer();
        }
        return cleared;
    }

    static void* GetFightStagingSelectedObject(UObject* character)
    {
        void* result = nullptr;
        // Helper: read a pointer from an offset if readable.
        auto tryRead = [](UObject* base, int offset) -> void*
        {
            if (!base || !Mem::IsReadable(base, offset + sizeof(void*)))
                return nullptr;
            return *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(base) + offset);
        };

        // 1. Check the character's AbilitySystemComponent (offset 0x738).
        if (Mem::IsReadable(character, 0x738 + sizeof(void*)))
        {
            UObject* asc = *reinterpret_cast<UObject**>(reinterpret_cast<uint8_t*>(character) + 0x738);
            if (asc)
            {
                // Walk ActiveAbilities (ASC ActiveAbilities array, offset 0x128,
                // count at 0x130). Up to 256 entries.
                if (Mem::IsReadable(asc, 0x130))
                {
                    int count = *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(asc) + 0x130);
                    if (count > 0 && count < 1024)
                    {
                        void** arr = *reinterpret_cast<void***>(reinterpret_cast<uint8_t*>(asc) + 0x128);
                        if (arr)
                        {
                            for (int i = 0; i < count; ++i)
                            {
                                UObject* ab = reinterpret_cast<UObject*>(arr[i]);
                                if (!ab || !ab->Class()) continue;
                                std::string abName = ab->Class()->GetName();
                                if (abName.find("FightStaging") != std::string::npos)
                                {
                                    // Read the selected-object pointer from the ability.
                                    // The note says offsets +0x690, +0x6A0, +0x6A1 are used.
                                    result = tryRead(ab, 0x690);
                                    if (!result) result = tryRead(ab, 0x6A0);
                                    if (!result) result = tryRead(ab, 0x6A1);
                                    break;
                                }
                            }
                        }
                    }
                }
            }
        }

        // Fallback: scan the character's outer chain for a UAIFightStagingAbility.
        if (!result)
        {
            UObject* o = character->Outer();
            for (int d = 0; o && d < 64; ++d)
            {
                if (!o->Class()) { o = o->Outer(); continue; }
                std::string cn = o->Class()->GetName();
                if (cn.find("FightStaging") != std::string::npos)
                {
                    result = tryRead(o, 0x690);
                    if (!result) result = tryRead(o, 0x6A0);
                    break;
                }
                o = o->Outer();
            }
        }

        return result;
    }
    std::atomic<unsigned long> g_renderThreadId{ 0 };
    std::atomic<unsigned long> g_gameThreadId{ 0 };
    std::mutex                g_gtQueueMutex;
    std::vector<std::function<void()>> g_gtQueue;
    std::atomic<bool>         g_gtHasWork{ false };
    thread_local int          t_peDepth = 0; // ProcessEvent nesting depth on this thread
    // Squad size mirror -- declared up here (not with the rest of the squad state lower
    // down) because hkProcessEvent gates the per-frame squad walk on it.
    std::atomic<int>          g_spawnedAllyCount{ 0 };
    // Idle-schedule stash for the squad-follow trick (game-thread only). The follow
    // drive nulls each member's pinned idle schedule so its BT runs the follow branch;
    // here we remember the original so the release path can put it back. Declared up
    // here so ApplyAiRelease (above the drive function) can restore it. See
    // DriveSquadVelocityGameThread.
    std::unordered_map<UObject*, UObject*> g_stashedSchedule;
    // Death/event tombstones: some robots stay readable with stale health after K2_OnDeath,
    // so health-only threat filtering can make Hook Twins keep attacking a corpse/spot.
    std::unordered_map<UObject*, ULONGLONG> g_aiDeathEventMs;
    std::unordered_map<UObject*, ULONGLONG> g_hookSuppressedThreatUntilMs;
    // Set by combat teardown, consumed by the native follow driver after
    // HookNativeFollowState exists. This stops any outstanding enemy MoveTo/path before
    // a follow route is allowed to reacquire the controller.
    std::unordered_set<UObject*> g_hookCombatRouteAbortPending;

    // ---- No-save guard (Horde Rounds arena) --------------------------------
    // While a horde run is active we must guarantee the game NEVER checkpoints --
    // otherwise the arena teleport / fight could be written over the player's real
    // progress. We swallow the game's save UFunctions right here in ProcessEvent:
    // g_blockSaves arms the guard; the three atomics hold the resolved save-fn
    // pointers (filled by the horde code on the game thread via CachedFn). The hook
    // only ever does a few cheap pointer compares, so it costs nothing when idle.
    std::atomic<bool>   g_blockSaves{ false };
    std::atomic<void*>  g_fnSaveProgress{ nullptr };
    std::atomic<void*>  g_fnSavePersistentData{ nullptr };
    std::atomic<void*>  g_fnCheckpointSaveProgress{ nullptr };

    // ---- AI ownership lock (native, ProcessEvent-level) --------------------
    // The reliable way to "own" an AI is not to keep RE-WRITING its team (the game
    // re-evaluates and flips it back -- the "fragile" behaviour), but to STOP the
    // game from ever flipping it: swallow, right here in ProcessEvent, the exact
    // UFunctions the engine would use to turn one of OUR squad units against us.
    // Same proven mechanism as the no-save guard above -- a few cheap pointer
    // compares, and only ever acts on squad members + only on the un-allying
    // direction (our own friendly/attack-real-enemy calls pass straight through).
    // This is class-agnostic, so it covers EVERY AI including the Twins.
    std::atomic<bool>     g_ownershipLock{ false };
    std::atomic<uint64_t> g_ownershipSwallows{ 0 };
    std::atomic<void*>    g_fnOwnSwitchTeamAttitude{ nullptr }; // AHAICharacter.SwitchTeamToMatchCharacterAttitude
    std::atomic<void*>    g_fnOwnSetTargetEnemy{ nullptr };     // AHAICharacter.SetTargetEnemy
    std::atomic<void*>    g_fnOwnSetBbTargetEnemy{ nullptr };   // AHAIController.SetBlackboardTargetEnemy
    // Hook Debug's experimental bodyguard mode. This is deliberately separate
    // from the normal AI/Squad tab. Its friendship override only affects the
    // player <-> managed-bodyguard pair and otherwise passes through untouched.
    std::atomic<bool>     g_hookBodyguardMode{ false };
    std::atomic<void*>    g_fnHookAreFriendly{ nullptr };
    std::atomic<uint64_t> g_friendshipForces{ 0 };
    std::atomic<uint64_t> g_unsafeTargetAllySkips{ 0 };
    std::atomic<uint64_t> g_hookDirectMoveInputs{ 0 };
    std::atomic<uint64_t> g_hookVelocityFallbacks{ 0 };
    std::atomic<uint64_t> g_hookMovementRecoveries{ 0 };
    std::atomic<uint64_t> g_hookFirstHitKills{ 0 };
    std::atomic<uint64_t> g_hookStaleTargetsCleared{ 0 };
    std::atomic<void*>    g_fnHookMoveToActor{ nullptr };
    std::atomic<void*>    g_fnHookMoveToLocation{ nullptr };
    std::atomic<void*>    g_fnHookStopMovement{ nullptr };
    std::atomic<void*>    g_fnHookSetCharacterAggressive{ nullptr };
    std::atomic<void*>    g_fnHookSetCharacterPassive{ nullptr };
    std::atomic<void*>    g_fnHookK2OnDeath{ nullptr };
    std::atomic<void*>    g_fnHookK2OnLoadDeathState{ nullptr };
    std::atomic<void*>    g_fnHookLoadDeadState{ nullptr };
    // Reflected UFunction pointer for AHAICharacter::TryActivateFightStagingAbility.
    // This is what ProcessEvent receives as `fn`.
    std::atomic<void*>    g_fnHookTryFightStaging{ nullptr };
    // Native helper detoured by MinHook. Ghidra shows RVA 0x1B93A50 is NOT the
    // reflected AHAICharacter bool function; it is a 3-arg fight-staging ability
    // selection writer: void(UAIFightStagingAbility*, void* selectedObj, bool).
    std::atomic<void*>    g_fnHookTryFightStagingNative{ nullptr };
    std::atomic<void*>    g_fnHookSendDeathEvent{ nullptr };
    std::atomic<void*>    g_fnHookSendLoadDeathStateEvent{ nullptr };
    std::atomic<void*>    g_fnHookSendCharacterDiedEvent{ nullptr };
    std::atomic<void*>    g_fnHookStartVersusQTE{ nullptr };
    std::atomic<void*>    g_fnHookCacheDeathPose{ nullptr };
    std::atomic<void*>    g_fnHookDestroyOwnerCharacter{ nullptr };
    std::atomic<uint64_t> g_hookTwinDeathPipelineBlocks{ 0 };
    std::atomic<uint64_t> g_hookTwinQtePipelineBlocks{ 0 };
    std::atomic<uint64_t> g_hookTwinFightStagingSelections{ 0 };
    std::atomic<uint64_t> g_hookTwinFightStagingBlocks{ 0 };
    std::atomic<uint64_t> g_hookTwinFightStagingContainerLogs{ 0 };
    std::atomic<bool>     g_hookTwinFightStagingHookAttempted{ false };
    std::atomic<bool>     g_hookTwinFightStagingSelectorHookLive{ false };
    std::atomic<bool>     g_hookTwinActionContainerHookLive{ false };
    std::atomic<bool>     g_hookTwinForensicsActive{ false };
    std::atomic<bool>     g_hookTwinForensicsWasOn{ false };
    std::atomic<bool>     g_hookTwinForensicsSnapshotInFlight{ false };
    std::ofstream         g_hookTwinForensicsFile;
    std::string           g_hookTwinForensicsDir;
    std::atomic<int>      g_hookTwinForensicsSeq{ 0 };
    std::atomic<unsigned long long> g_hookTwinForensicsLastSnapshotMs{ 0 };
    std::atomic<unsigned long long> g_hookTwinForensicsEvents{ 0 };
    std::atomic<unsigned long long> g_hookTwinDeathPoseBlockUntilMs{ 0 };
    std::atomic<void*>    g_hookTwinDeathPoseGuard{ nullptr };
    std::atomic<void*>    g_hookTwinRecentFightStagingGuard{ nullptr };
    std::atomic<unsigned long long> g_hookTwinRecentFightStagingUntilMs{ 0 };
    std::atomic<uint64_t> g_hookCombatTargetEvents{ 0 };
    std::atomic<uint64_t> g_hookCombatStateEvents{ 0 };
    std::atomic<uint64_t> g_hookControllerMovesBlocked{ 0 };
    thread_local bool     t_hookMovementInternal = false;
    std::mutex            g_hookControllerOwnedMutex;
    std::unordered_set<UObject*> g_hookControllerOwned;
    // Defined far below (needs the param structs + IsSquadMember); declared here so
    // hkProcessEvent can call it. Returns true => swallow this dispatch (block it).
    bool OwnershipShouldSwallow(void* obj, void* fn, void* params);
    bool IsHookBodyguard(UObject* ai);
    bool IsMixedNavCharacter(UObject* ai);
    bool ReadCharacterHealth(UObject* ch, float& cur, float& mx);
    bool SetCharacterHealthFull(UObject* ai);
    void ResolveHookTwinDeathPipelineFns();
    bool EnsureHookTwinFightStagingNativeHook();
    bool EnsureHookTwinActionContainerFactory();
    bool IsHookTwinBodyguard(UObject* ai);
    UObject* HookTwinOwnerFromDeathAbility(void* ability);
    bool HookTwinDeathPipelineShouldSwallow(void* obj, void* fn, void* params);
    void HookTwinForensicsProcessEvent(void* obj, void* fn, void* params);
    void UpdateHookTwinForensics();
    std::string SafeObjectFullName(UObject* o);
    void TrackAiDeathEventFromProcessEvent(void* obj, void* fn, void* params);

    bool HookFriendshipShouldForce(void* fn, void* params);
    bool HookMovementShouldSwallow(void* obj, void* fn);
    bool HookStaleCombatTargetShouldSwallow(void* obj, void* fn, void* params);
    void HookCombatTrace(void* obj, void* fn, void* params);

    void DiagnosticTraceProcessEvent(void* obj, void* fn, void* params);
    void RunConsoleCommandImpl(const std::string& cmd); // defined later; used by the spawn anim-safety fix
    void RefreshFlyStreaming(UObject* pawn, bool force); // defined later; horde restore re-streams the original area

    // True when the engine is dispatching a UFunction ON an enemy AI character or
    // AI controller -- i.e. it is mid-AI-logic, so this callsite is NOT safe to
    // borrow for our own AI ProcessEvent injection. Cheap: only ever reached on the
    // rare drain-candidate path (work pending + outermost + game thread), and the
    // class lookups are name-index hits cached in statics. If the AI classes are
    // not resolved yet (very early, no enemies streamed) we treat the object as
    // non-AI so spawn / other deferred work can still run.
    bool PeDispatchingOnAi(void* obj)
    {
        if (!Mem::IsReadable(obj, 0x30))
            return false;
        static UClass* aiChar = nullptr;
        static UClass* aiCtrl = nullptr;
        if (!Mem::IsReadable(aiChar, 0x30)) aiChar = FindObjectFast(AH::Cls_AICharacter);
        if (!Mem::IsReadable(aiCtrl, 0x30)) aiCtrl = FindObjectFast(AH::Cls_AIController);
        UObject* o = reinterpret_cast<UObject*>(obj);
        UClass* cls = o->Class();
        if (!Mem::IsReadable(cls, 0x30))
            return true; // unsure -> treat as AI and skip (the safe side)

        // O(1) per-CLASS memo. "Is this an AI/BT object?" depends only on the class,
        // so we answer it once per class and cache the result. This path is hot while
        // an AI feature is active (it runs on every outermost ProcessEvent while work
        // is pending), and the old code did a GetName() string alloc + 8 substring
        // scans EVERY call -> that overhead was a real source of stutter. thread_local
        // so the game thread fills its own table with no locks.
        thread_local std::unordered_map<UClass*, unsigned char> memo;
        auto it = memo.find(cls);
        if (it != memo.end())
            return it->second != 0;

        bool isAi = false;
        try
        {
            // Fast path: the AI pawn / controller itself.
            if ((aiChar && o->IsA(aiChar)) || (aiCtrl && o->IsA(aiCtrl)))
                isAi = true;
            else
            {
                // The behaviour tree dispatches service / task / decorator blueprint
                // events on their OWN component objects (not the AI pawn), at top-level
                // depth, while it is iterating AI state. Those are exactly the moments
                // we must not inject. Catch them by class-name family (memoised here).
                const std::string cn = cls->GetName();
                static const char* kAiMachinery[] = {
                    "BTService", "BTTask", "BTDecorator", "BehaviorTree",
                    "Blackboard", "AIPerception", "EnvQuery", "AIController" };
                for (const char* k : kAiMachinery)
                    if (cn.find(k) != std::string::npos) { isAi = true; break; }
            }
        }
        catch (...) { isAi = true; } // unsure -> treat as AI and skip (the safe side)
        memo[cls] = isAi ? 1u : 0u;
        return isAi;
    }

    void hkProcessEvent(void* obj, void* fn, void* params)
    {
        const bool outermost = (t_peDepth == 0);
        ++t_peDepth;
        // Always restore the depth, even if oProcessEvent unwinds.
        struct DepthGuard { ~DepthGuard() { --t_peDepth; } } depthGuard;

        // No-save guard: while a Horde Rounds arena is active, swallow the game's
        // save UFunctions so it can NEVER checkpoint over the player's real progress.
        // Pure pointer compares; we return WITHOUT calling the original (the saves are
        // fire-and-forget triggers with no return value the engine consumes mid-frame).
        if (g_blockSaves.load(std::memory_order_relaxed) && fn)
        {
            if (fn == g_fnSaveProgress.load(std::memory_order_relaxed) ||
                fn == g_fnSavePersistentData.load(std::memory_order_relaxed) ||
                fn == g_fnCheckpointSaveProgress.load(std::memory_order_relaxed))
            {
                static ULONGLONG lastBlockLogMs = 0;
                ULONGLONG nowB = GetTickCount64();
                if (nowB - lastBlockLogMs > 1000) { lastBlockLogMs = nowB; LOG("Horde: swallowed a game save (arena active)"); }
                return; // depthGuard restores t_peDepth
            }
        }

        // Track real death events globally so stale cached corpses never remain threats.
        if (fn && obj)
        {
            try { TrackAiDeathEventFromProcessEvent(obj, fn, params); } catch (...) {}
        }

        // Hook Twin root-cause forensics. When armed, dump every relevant ProcessEvent
        // around managed Twins before any guard below can swallow or mutate it.
        if (g_hookTwinForensicsActive.load(std::memory_order_relaxed) && fn)
        {
            try { HookTwinForensicsProcessEvent(obj, fn, params); } catch (...) {}
        }

        // Hook Twin death/QTE pipeline guard. ReVa traced the downed pose to the
        // reflected death/load-death/fight-staging/QTE chain, not to follow movement.
        // Swallow those dispatches only when the affected pawn is a managed Hook Twin.
        if (g_hookBodyguardMode.load(std::memory_order_relaxed) && fn)
        {
            bool swallow = false;
            try { swallow = HookTwinDeathPipelineShouldSwallow(obj, fn, params); } catch (...) { swallow = false; }
            if (swallow)
                return;
        }

        // If a latent Twin/gameplay ability tries to re-assign a killed/suppressed robot
        // as TargetEnemy after our stand-down, refuse it before it reaches the AI script.
        if (g_hookBodyguardMode.load(std::memory_order_relaxed) && fn && params)
        {
            bool swallowStale = false;
            try { swallowStale = HookStaleCombatTargetShouldSwallow(obj, fn, params); } catch (...) { swallowStale = false; }
            if (swallowStale)
                return;
        }

        // Hook Bodyguard combat-chain observer. This is read-only and records the game's
        // target/aggressive/passive transitions before any ownership policy can swallow one.
        if (g_hookBodyguardMode.load(std::memory_order_relaxed) && fn && params)
        {
            try { HookCombatTrace(obj, fn, params); } catch (...) {}
        }

        // AI ownership lock: while armed, block the game from turning one of OUR squad
        // units against us. Cheap pointer compares first (OwnershipShouldSwallow rejects
        // immediately unless fn is one of the 3 cached un-allying UFunctions), so this
        // costs nothing on the hot path. Swallow = return WITHOUT calling the original,
        // exactly like the no-save guard. Wrapped so a bad param read can never escalate.
        if (g_ownershipLock.load(std::memory_order_relaxed) && fn && obj)
        {
            bool swallow = false;
            try { swallow = OwnershipShouldSwallow(obj, fn, params); } catch (...) { swallow = false; }
            if (swallow)
            {
                uint64_t n = g_ownershipSwallows.fetch_add(1, std::memory_order_relaxed) + 1;
                static ULONGLONG lastOwnLogMs = 0;
                ULONGLONG nowO = GetTickCount64();
                if (nowO - lastOwnLogMs > 1000) { lastOwnLogMs = nowO; LOG("Ownership: blocked the game from un-allying a squad unit (total %llu)", (unsigned long long)n); }
                return; // depthGuard restores t_peDepth
            }
        }

        // Hook Bodyguard movement ownership. Only the exact controller Move/Stop
        // UFunctions are considered, only for a controller possessing a dedicated
        // Hook roster pawn, and only while our native follow command is active.
        if (g_hookBodyguardMode.load(std::memory_order_relaxed) && fn && obj)
        {
            bool swallow = false;
            try { swallow = HookMovementShouldSwallow(obj, fn); } catch (...) { swallow = false; }
            if (swallow)
            {
                uint64_t n = g_hookControllerMovesBlocked.fetch_add(1, std::memory_order_relaxed) + 1;
                static ULONGLONG lastLog = 0; ULONGLONG now = GetTickCount64();
                if (now - lastLog > 1500)
                { lastLog = now; LOG("[AI-MOVE] blocked external AIController Move/Stop replacement (total=%llu)", (unsigned long long)n); }
                return;
            }
        }

        // Hook Debug experimental friendship override. The reflected
        // AIUtils.AreFriendlyCharacters UFunction is uniquely resolved by SDK
        // metadata; ReVa confirms its native exec thunk at RVA 0x225BDA0. We
        // intercept only this dispatch and only for player <-> managed guard.
        if (g_hookBodyguardMode.load(std::memory_order_relaxed) && fn && params)
        {
            bool force = false;
            try { force = HookFriendshipShouldForce(fn, params); } catch (...) { force = false; }
            if (force)
                return; // params.ReturnValue was set true; narrow override complete
        }

        // Drain deferred game-thread work only at a proven-safe callsite (see the
        // header comment): outermost dispatch, on the game thread, not on an AI
        // object. Nested calls (including the ProcessEvent our own tasks make) and
        // AI-object dispatches fall straight through to the original function.
        try { DiagnosticTraceProcessEvent(obj, fn, params); } catch (...) {}

        unsigned long tid = GetCurrentThreadId();

        // Per-frame squad FOLLOW, on the GAME thread. We DON'T move the body ourselves
        // (no drag/teleport/velocity write -- those all looked like "dogshit gliding").
        // Instead we trick each member's OWN AI: every tick we overwrite the follow
        // blackboard keys its behaviour tree reads (FollowLocation/CurrentWaypoint =
        // YOUR position, ForceFollowLocation=true) and UNPIN it from its idle schedule,
        // so its own locomotion (real walk/run animation, Mercuna avoidance) carries it
        // to you. This MUST run on the game thread: it dispatches ProcessEvent (blackboard
        // setters), which is game-thread-only in UE4. Outermost-only + ~20 Hz throttled
        // (the BT re-reads the keys continuously, so this rate is plenty and stays light).
        {
            unsigned long gtid = g_gameThreadId.load();
            if (outermost && gtid != 0 && tid == gtid && g_spawnedAllyCount.load() > 0)
            {
                static ULONGLONG lastDriveMs = 0;
                ULONGLONG nowMs = GetTickCount64();
                // Two gates, BOTH required (the second one was missing and is the fix
                // for the hard menu-open / random freeze):
                //   * ~20 Hz throttle -- the follow keys are level-triggered, no need to spam.
                //   * NOT mid-AI-dispatch -- DriveSquadVelocityGameThread writes blackboard
                //     keys via ProcessEvent on the AI controller (SetController*). If that
                //     lands while the engine is itself dispatching AI logic at this top-level
                //     callsite (a BT task/service/decorator event -- see PeDispatchingOnAi),
                //     it mutates the very state the engine is iterating and the game thread
                //     spins forever inside engine AI code = the permanent freeze. The queue
                //     drain below already refuses those callsites; the per-frame follow MUST
                //     refuse them too. PeDispatchingOnAi is only evaluated once the throttle
                //     is ready, so it stays cheap; if this callsite is unsafe we simply wait
                //     for the next safe one (they occur many times per frame).
                if (nowMs - lastDriveMs >= 50 && !PeDispatchingOnAi(obj))
                {
                    lastDriveMs = nowMs;
                    try { DriveSquadVelocityGameThread(); } catch (...) {}
                }
            }
        }

        if (outermost && g_gtHasWork.load() && tid != g_renderThreadId.load() &&
            !PeDispatchingOnAi(obj))
        {
            // Latch onto the first safe non-render thread we see (the game thread:
            // UFunction dispatch is game-thread-only in UE4) and never switch.
            unsigned long expected = 0;
            g_gameThreadId.compare_exchange_strong(expected, tid);
            if (tid == g_gameThreadId.load())
            {
                // Drain at most a few tasks per safe callsite so a pile-up never does
                // all the heavy work (e.g. several spawns) in ONE frame -> the game
                // stays smooth. Safe callsites occur many times per frame, so anything
                // left over still runs within the same frame, just spread out.
                constexpr size_t kMaxTasksPerBorrow = 3;
                std::vector<std::function<void()>> tasks;
                {
                    std::lock_guard<std::mutex> lk(g_gtQueueMutex);
                    size_t take = (std::min)(g_gtQueue.size(), kMaxTasksPerBorrow);
                    tasks.assign(std::make_move_iterator(g_gtQueue.begin()),
                                 std::make_move_iterator(g_gtQueue.begin() + take));
                    g_gtQueue.erase(g_gtQueue.begin(), g_gtQueue.begin() + take);
                    g_gtHasWork = !g_gtQueue.empty();
                }
                // Tasks call ProcessEvent themselves; those re-enter this hook at
                // t_peDepth > 0 and are correctly skipped by the outermost check.
                for (auto& task : tasks) { try { task(); } catch (...) {} }
            }
        }
        oProcessEvent(obj, fn, params);
    }

    bool InstallProcessEventHook()
    {
        static std::mutex installMutex;
        std::lock_guard<std::mutex> lk(installMutex);
        if (oProcessEvent)
            return true;

        UObject* anchor = GetWorld();
        if (!Mem::IsReadable(anchor, sizeof(void*)))
            anchor = GetLocalPawn();
        if (!Mem::IsReadable(anchor, sizeof(void*)))
        {
            LOG("ProcessEvent hook: no anchor object yet");
            return false;
        }

        void** vt = *reinterpret_cast<void***>(anchor);
        if (!Mem::IsReadable(vt, (Offsets::VFUNC_PROCESSEVENT + 1) * sizeof(void*)))
        {
            LOG("ProcessEvent hook: vtable unreadable");
            return false;
        }
        void* target = vt[Offsets::VFUNC_PROCESSEVENT];
        if (!Mem::IsReadable(target, 1))
        {
            LOG("ProcessEvent hook: target not executable");
            return false;
        }

        MH_STATUS c = MH_CreateHook(target, reinterpret_cast<void*>(&hkProcessEvent), reinterpret_cast<void**>(&oProcessEvent));
        if (c != MH_OK && c != MH_ERROR_ALREADY_CREATED)
        {
            LOG("ProcessEvent hook: MH_CreateHook failed %d", c);
            return false;
        }
        MH_STATUS e = MH_EnableHook(target);
        if (e != MH_OK && e != MH_ERROR_ENABLED)
        {
            LOG("ProcessEvent hook: MH_EnableHook failed %d", e);
            return false;
        }
        g_peTarget = target;
        LOG("ProcessEvent hooked @ %p (game-thread dispatch ready)", target);
        return true;
    }

    void QueueGameThread(std::function<void()> fn)
    {
        std::lock_guard<std::mutex> lk(g_gtQueueMutex);
        g_gtQueue.push_back(std::move(fn));
        g_gtHasWork = true;
    }

    constexpr float kNormalDamageMultiplier = 1.0f;
    constexpr float kGodDamageMultiplier = 0.0f;
    constexpr float kOneHitDamageMultiplier = 1000.0f;

    struct GiveAllState
    {
        bool active = false;
        bool equipLast = false;
        int index = 0;
        int ok = 0;
        ULONGLONG lastStepMs = 0;
    };

    GiveAllState g_giveAll;

    struct AttrBackup
    {
        uint8_t* set = nullptr;
        int attrOff = 0;
        bool valid = false;
        float base = 0.0f;
        float current = 0.0f;
    };

    struct MovementBackup
    {
        uint8_t* mv = nullptr;
        bool walkValid = false;
        bool flyValid = false;
        bool modeValid = false;
        float walkSpeed = 0.0f;
        float flySpeed = 0.0f;
        uint8_t mode = 0;
    };

    struct InventoryBackup
    {
        UObject* inventory = nullptr;
        bool ammoCountValid = false;
        bool infiniteAmmoCountValid = false;
        int32_t ammoCount = 0;
        int32_t infiniteAmmoCount = 0;
    };

    struct WeaponAmmoBackup
    {
        UObject* weapon = nullptr;
        void* barrel = nullptr;
        bool ammoSizeValid = false;
        bool startAmmoValid = false;
        int32_t ammoSize = 0;
        int32_t startAmmo = 0;
        bool shootingBlockedValid = false;
        bool cycleUnlimitedValid = false;
        bool cycleCountValid = false;
        bool cyclePosValid = false;
        bool loadNextValid = false;
        bool chamberedValid = false;
        bool shootingBlocked = false;
        bool cycleUnlimited = false;
        int32_t cycleCount = 0;
        int32_t cyclePos = 0;
        bool loadNext = false;
        void* chambered = nullptr;
    };

    // FOV override capture/restore. Namespace-scope so HasPawnFeatureWork can
    // keep TickImpl alive for one more frame to restore after the toggle clears.
    struct FovBackup
    {
        bool  active = false;
        bool  fpValid = false;
        bool  tpValid = false;
        float fp = 0.0f;
        float tp = 0.0f;
    };
    FovBackup g_fovBackup;

    AttrBackup g_incomingDamageBackup;
    AttrBackup g_healthBackup;
    AttrBackup g_instigatedDamageBackup;
    AttrBackup g_staminaBackup;
    AttrBackup g_energyBackup;
    AttrBackup g_airBackup;
    MovementBackup g_movementBackup;
    InventoryBackup g_inventoryBackup;
    WeaponAmmoBackup g_weaponAmmoBackup;

    constexpr Features::WeaponEntry kWeapons[] =
    {
        { "Swede / Shved",          "DA_Item_Shved.DA_Item_Shved" },
        { "Zvezdochka",            "DA_Item_Zvezdochka.DA_Item_Zvezdochka" },
        { "Zvezdochka DLC1",       "DA_Item_Zvezdochka_DLC1.DA_Item_Zvezdochka_DLC1" },
        { "KS-23 Shotgun",         "DA_Item_ShotgunKS23.DA_Item_ShotgunKS23" },
        { "KS-23 Shotgun DLC1",    "DA_Item_ShotgunKS23_DLC1.DA_Item_ShotgunKS23_DLC1" },
        { "PM",                    "DA_Item_PM.DA_Item_PM" },
        { "PM DLC1",               "DA_Item_PM_DLC1.DA_Item_PM_DLC1" },
        { "Kalash Rifle / AK-47",  "DA_Item_AK47.DA_Item_AK47" },
        { "Base Pistol",           "DA_Item_BasePistol.DA_Item_BasePistol" },
        { "Electro",               "DA_Item_Electro.DA_Item_Electro" },
        { "Dominator",             "DA_Item_Dominator.DA_Item_Dominator" },
        { "Railgun",               "DA_Item_Railgun.DA_Item_Railgun" },
        { "Plasmagun",             "DA_Item_Plasmagun.DA_Item_Plasmagun" },
        { "Krepysh",               "DA_Item_Krepysh.DA_Item_Krepysh" },
        { "Machinegun",            "DA_Item_Machinegun.DA_Item_Machinegun" },
        { "PTRD",                  "DA_Item_PTRD.DA_Item_PTRD" },
        { "Bidonist",              "DA_Item_Bidonist.DA_Item_Bidonist" },
        { "Flamethrower",          "DA_Item_Flamethrower.DA_Item_Flamethrower" },
        { "Shprits",               "DA_Item_Shprits.DA_Item_Shprits" },
        { "Signalka",              "DA_Item_Signalka.DA_Item_Signalka" },
        { "Axe",                   "DA_Item_Axe.DA_Item_Axe" },
        { "Pashtet",               "DA_Item_Pashtet.DA_Item_Pashtet" },
        { "Snowball / Snejok",     "DA_Item_Snejok.DA_Item_Snejok" },
        { "Fox / Lisa",            "DA_Item_Lisa.DA_Item_Lisa" },
        { "Abzac",                 "DA_Item_Abzac.DA_Item_Abzac" },
        { "Bober",                 "DA_Item_Bober.DA_Item_Bober" },
        { "Dikobraz",              "DA_Item_Dikobraz.DA_Item_Dikobraz" },
        { "Hipar",                 "DA_Item_Hipar.DA_Item_Hipar" },
        { "Hirurg",                "DA_Item_Hirurg.DA_Item_Hirurg" },
        { "Karusel",               "DA_Item_Karusel.DA_Item_Karusel" },
        { "Kilka",                 "DA_Item_Kilka.DA_Item_Kilka" },
        { "Lapta",                 "DA_Item_Lapta.DA_Item_Lapta" },
        { "Molot",                 "DA_Item_Molot.DA_Item_Molot" },
        { "Neptun",                "DA_Item_Neptun.DA_Item_Neptun" },
        { "Petuh",                 "DA_Item_Petuh.DA_Item_Petuh" },
        { "Zinger",                "DA_Item_Zinger.DA_Item_Zinger" },
        { "Klusha",                "DA_Item_Klusha.DA_Item_Klusha" },
    };

    struct AmmoEntry
    {
        const char* label;
        const char* objectName;
    };

    constexpr AmmoEntry kAmmo[] =
    {
        { "AK47",       "DA_Item_AK47Ammo.DA_Item_AK47Ammo" },
        { "Machinegun", "DA_Item_MachinegunAmmo.DA_Item_MachinegunAmmo" },
        { "Medium",     "DA_Item_MediumAmmo.DA_Item_MediumAmmo" },
        { "Pistol",     "DA_Item_PistolAmmo.DA_Item_PistolAmmo" },
        { "Shotgun",    "DA_Item_ShotgunAmmo.DA_Item_ShotgunAmmo" },
        { "Strong",     "DA_Item_StrongAmmo.DA_Item_StrongAmmo" },
        { "Weak",       "DA_Item_WeakAmmo.DA_Item_WeakAmmo" },
        { "Plasmagun",  "DA_PlasmagunAmmo.DA_PlasmagunAmmo" },
    };

    // Cache resolved UFunction* by full name so we only walk GObjects once.
    UFunction* CachedFn(const char* fullName)
    {
        static std::unordered_map<std::string, UFunction*> cache;
        static std::mutex cacheMutex;
        std::lock_guard<std::mutex> lock(cacheMutex);
        auto it = cache.find(fullName);
        if (it != cache.end()) return it->second;
        UFunction* fn = FindFunction(fullName);
        cache[fullName] = fn;
        LOG("%s %s -> %p", fn ? "Resolved" : "MISSING", fullName, (void*)fn);
        return fn;
    }

    UObject* CachedObject(const char* name)
    {
        static std::unordered_map<std::string, UObject*> cache;
        static std::unordered_map<std::string, ULONGLONG> missLogMs;
        static std::mutex cacheMutex;
        std::lock_guard<std::mutex> lock(cacheMutex);
        auto it = cache.find(name);
        if (it != cache.end() && Mem::IsReadable(it->second, 0x30))
            return it->second;

        UObject* obj = FindObjectFast(name);
        if (obj)
        {
            cache[name] = obj;
            LOG("Resolved object %s -> %p", name, (void*)obj);
        }
        else
        {
            ULONGLONG nowMs = GetTickCount64();
            auto missIt = missLogMs.find(name);
            if (missIt == missLogMs.end() || nowMs - missIt->second > 5000)
            {
                missLogMs[name] = nowMs;
                LOG("Object not resolved yet %s -> %p", name, (void*)obj);
            }
        }
        return obj;
    }

    UFunction* CachedClassFn(const char* className, const char* shortName)
    {
        static std::unordered_map<std::string, UFunction*> cache;
        static std::unordered_map<std::string, ULONGLONG> missLogMs;
        static std::mutex cacheMutex;

        std::string key = std::string(className) + "::" + shortName;
        std::lock_guard<std::mutex> lock(cacheMutex);
        auto it = cache.find(key);
        if (it != cache.end() && Mem::IsReadable(it->second, 0x30))
            return it->second;

        UClass* cls = FindObjectFast(className);
        UFunction* fn = cls ? FindFunctionInClass(cls, shortName) : nullptr;
        if (fn)
        {
            cache[key] = fn;
            LOG("Resolved class fn %s -> %p", key.c_str(), (void*)fn);
        }
        else
        {
            ULONGLONG nowMs = GetTickCount64();
            auto missIt = missLogMs.find(key);
            if (missIt == missLogMs.end() || nowMs - missIt->second > 5000)
            {
                missLogMs[key] = nowMs;
                LOG("Class fn not resolved yet %s cls=%p fn=%p", key.c_str(), (void*)cls, (void*)fn);
            }
        }
        return fn;
    }

    UFunction* CachedObjectClassFn(UObject* object, const char* shortName)
    {
        if (!Mem::IsReadable(object, 0x30) || !shortName || !*shortName)
            return nullptr;

        UClass* objectClass = object->Class();
        if (!Mem::IsReadable(objectClass, 0x30))
            return nullptr;

        static std::unordered_map<std::string, UFunction*> cache;
        static std::unordered_map<std::string, ULONGLONG> missLogMs;
        static std::mutex cacheMutex;

        std::string key = std::to_string((uintptr_t)objectClass) + "::" + shortName;
        std::lock_guard<std::mutex> lock(cacheMutex);
        auto it = cache.find(key);
        if (it != cache.end() && Mem::IsReadable(it->second, 0x30))
            return it->second;

        UFunction* fn = nullptr;
        UClass* cls = objectClass;
        for (int depth = 0; cls && depth < 64; ++depth)
        {
            if (!Mem::IsReadable(cls, Offsets::O_UStruct_SuperStruct + sizeof(void*)))
                break;
            fn = FindFunctionInClass(cls, shortName);
            if (fn)
                break;
            cls = *reinterpret_cast<UClass**>((uint8_t*)cls + Offsets::O_UStruct_SuperStruct);
        }

        if (fn)
        {
            cache[key] = fn;
            LOG("Resolved object class fn %s class=%p -> %p", shortName, (void*)objectClass, (void*)fn);
        }
        else
        {
            ULONGLONG nowMs = GetTickCount64();
            auto missIt = missLogMs.find(key);
            if (missIt == missLogMs.end() || nowMs - missIt->second > 5000)
            {
                missLogMs[key] = nowMs;
                LOG("Object class fn not resolved yet %s class=%p", shortName, (void*)objectClass);
            }
        }
        return fn;
    }

    // ---- ProcessEvent param structs ---------------------------------------
    struct P_GetActorLocation { FVector ReturnValue; };
    struct P_ObjectReturn { void* ReturnValue; };
    struct P_WorldContext { void* WorldContextObject; };
    struct P_BoolParam { bool bValue; };
    struct P_SetMovementMode { uint8_t NewMovementMode; uint8_t NewCustomMode; };
    struct P_SetActorLocation
    {
        FVector NewLocation;
        bool    bSweep;
        uint8_t _pad0[3];
        uint8_t OutSweepHitResult[0x88]; // FHitResult
        bool    bTeleport;
        bool    ReturnValue;
        uint8_t _pad1[2];
    };
    static_assert(sizeof(P_SetActorLocation) == 0x9C, "K2_SetActorLocation params must match Dumper-7");
    static_assert(sizeof(P_ObjectReturn) == 0x08, "object return params must match Dumper-7");
    static_assert(sizeof(P_WorldContext) == 0x08, "world context params must match Dumper-7");
    static_assert(sizeof(P_BoolParam) == 0x01, "single bool params must match Dumper-7");
    struct P_GetControlRotation { FRotator ReturnValue; };
    struct P_IntReturn { int32_t ReturnValue; };
    struct P_GetCurrentWeapon { void* ReturnValue; };
    struct P_AmmoCount
    {
        bool    bCountChambered;
        uint8_t _pad0[3];
        int32_t ReturnValue;
    };
    struct P_WeaponInventoryAmmoCount
    {
        void*   Weapon;
        int32_t ReturnValue;
        uint8_t _pad0[4];
    };
    struct P_WeaponDataAsset { void* WeaponItemDataAsset; };
    struct P_FindWeaponByDataAsset { void* WeaponItemDataAsset; void* ReturnValue; };
    struct P_TakeWeapon
    {
        void*   WeaponItemDataAsset;
        bool    bInstant;
        uint8_t _pad0[7];
    };
    struct P_InventoryItemCount
    {
        void*   ItemDataAsset;
        int32_t InCount;
        uint8_t _pad0[4];
    };
    struct P_InventoryGetItemsCount
    {
        void*   InItemDataAsset;
        int32_t ReturnValue;
        uint8_t _pad0[4];
    };
    struct P_BarrelSetAmmo
    {
        int32_t Count;
        bool    UnloadChambered;
        bool    CancelShooting;
        bool    ManualCharge;
        uint8_t _pad0;
        TArray<void*> NewAmmo;
    };
    static_assert(sizeof(P_AmmoCount) == 0x08, "GetAmmoCount params must match Dumper-7");
    static_assert(sizeof(P_WeaponInventoryAmmoCount) == 0x10, "GetWeaponInventoryAmmoCount params must match Dumper-7");
    static_assert(sizeof(P_WeaponDataAsset) == 0x08, "weapon data asset params must match Dumper-7");
    static_assert(sizeof(P_FindWeaponByDataAsset) == 0x10, "FindWeaponByDataAsset params must match Dumper-7");
    static_assert(sizeof(P_TakeWeapon) == 0x10, "TakeWeapon params must match Dumper-7");
    static_assert(sizeof(P_InventoryItemCount) == 0x10, "AddItemsToInventory params must match Dumper-7");
    static_assert(sizeof(P_InventoryGetItemsCount) == 0x10, "GetItemsCount params must match Dumper-7");
    static_assert(sizeof(P_BarrelSetAmmo) == 0x18, "EBBarrel.SetAmmo params must match Dumper-7");
    struct P_SetIgnoreLookInput { bool bNewLookInput; };
    struct P_SetIgnoreMoveInput { bool bNewMoveInput; };
    struct P_SetTargetAlly { void* TargetAlly; };
    struct P_SetTargetEnemy
    {
        void* TargetEnemy;
        bool  bIgnoreScriptedEnemies;
        bool  bForceUpdate;
        uint8_t _pad0[6];
    };
    struct P_SwitchTeamToMatchCharacterAttitude
    {
        void* OtherCharacter;
        uint8_t TargetTeamAttitude;
        uint8_t _pad0[7];
    };
    // FGenericTeamId is a single uint8 (verified in AIModule_structs.hpp). These
    // are the AHBaseCharacter team accessors -- the engine-correct team path.
    struct P_SetGenericTeamId { uint8_t TeamID; };
    struct P_GetGenericTeamId { uint8_t ReturnValue; };
    static_assert(sizeof(P_SetGenericTeamId) == 0x01, "AHBaseCharacter.SetGenericTeamId params must match Dumper-7");
    static_assert(sizeof(P_GetGenericTeamId) == 0x01, "AHBaseCharacter.GetGenericTeamId params must match Dumper-7");
    struct P_SetBlackboardFollowLocation { FVector Location; };
    struct P_SetBlackboardTargetAlly { void* NewAlly; };
    struct P_SetBlackboardTargetEnemy
    {
        void* NewTarget;
        bool  bForceUpdate;
        uint8_t _pad0[7];
    };
    struct P_SetFollowLocationSpeed
    {
        float Speed;
        float Duration;
        FRotator RotationRate;
    };
    struct P_MoveToActor
    {
        void* Goal;
        float AcceptanceRadius;
        bool  bStopOnOverlap;
        bool  bUsePathfinding;
        bool  bCanStrafe;
        uint8_t _pad0;
        void* FilterClass;
        bool  bAllowPartialPath;
        uint8_t ReturnValue;
        uint8_t _pad1[6];
    };
    struct P_SpawnAIFromClass
    {
        void* WorldContextObject;
        void* PawnClass;
        void* BehaviorTree;
        FVector Location;
        FRotator Rotation;
        bool bNoCollisionFail;
        uint8_t _pad0[7];
        void* Owner;
        void* ReturnValue;
    };
    static_assert(sizeof(P_SetTargetAlly) == 0x08, "AHAICharacter.SetTargetAlly params must match Dumper-7");
    static_assert(sizeof(P_SetTargetEnemy) == 0x10, "AHAICharacter.SetTargetEnemy params must match Dumper-7");
    static_assert(sizeof(P_SwitchTeamToMatchCharacterAttitude) == 0x10, "AHAICharacter.SwitchTeamToMatchCharacterAttitude params must match Dumper-7");
    static_assert(sizeof(P_SetBlackboardFollowLocation) == 0x0C, "AHAIController.SetBlackboardFollowLocation params must match Dumper-7");
    static_assert(sizeof(P_SetBlackboardTargetAlly) == 0x08, "AHAIController.SetBlackboardTargetAlly params must match Dumper-7");
    static_assert(sizeof(P_SetBlackboardTargetEnemy) == 0x10, "AHAIController.SetBlackboardTargetEnemy params must match Dumper-7");
    static_assert(sizeof(P_SetFollowLocationSpeed) == 0x14, "AHAIController.SetFollowLocationSpeed params must match Dumper-7");
    static_assert(sizeof(P_MoveToActor) == 0x20, "AIController.MoveToActor params must match Dumper-7");
    static_assert(sizeof(P_SpawnAIFromClass) == 0x48, "AIBlueprintHelperLibrary.SpawnAIFromClass params must match Dumper-7");

    // ---- Deep AI injection param structs (Dumper-7 verified) --------------
    struct P_SetCharacterAggressive
    {
        void*   InAICharacter;
        bool    bShouldBeActive;
        uint8_t _pad0[7];
        void*   InTargetEnemy;
        void*   InPassiveAnimation;
        void*   InAggressiveAnimation;
        float   InAggressiveAnimationPlayRate;
        uint8_t _pad1[4];
    };
    static_assert(sizeof(P_SetCharacterAggressive) == 0x30, "AIUtils.SetCharacterAggressive params must match Dumper-7");

    struct P_SetCharacterPassive { void* InAICharacter; void* InPassiveAnimation; };
    static_assert(sizeof(P_SetCharacterPassive) == 0x10, "AIUtils.SetCharacterPassive params must match Dumper-7");

    // Pawn.AddMovementInput(WorldDirection, ScaleValue, bForce) -- the LOW-LEVEL
    // locomotion driver: feeds the character movement component's input directly,
    // independent of the behaviour tree, so a squad member physically walks toward
    // you even when its BT won't path.
    struct P_AddMovementInput { FVector WorldDirection; float ScaleValue; bool bForce; uint8_t _pad0[3]; };
    static_assert(sizeof(P_AddMovementInput) == 0x14, "Pawn.AddMovementInput params must match Dumper-7");

    struct P_RestartLogic { void* CachedAIController; };
    static_assert(sizeof(P_RestartLogic) == 0x08, "AIUtils.RestartLogic params must match Dumper-7");

    struct P_AreFriendlyCharacters
    {
        void*   CharacterOne;
        void*   CharacterTwo;
        bool    CountNeutralAsFriendly;
        bool    ReturnValue;
        uint8_t _pad0[6];
    };
    static_assert(sizeof(P_AreFriendlyCharacters) == 0x18, "AIUtils.AreFriendlyCharacters params must match Dumper-7");
    struct P_HookDeathEventOwner { void* EventOwnerCharacter; };
    struct P_HookGameplayCharacterDied { void* DeadCharacter; };
    struct P_HookTryFightStaging { bool ReturnValue; };
    struct P_HookModifyIncomingDamage { float InDamage; float OutDamage; };
    struct P_HookProcessIncomingDamage
    {
        float DamageDone;
        uint8_t _pad0[0x13C]; // FHitParameters lives at +0x08; we don't need to parse it here.
        float ReturnValue;
        uint8_t _pad1[4];
    };
    static_assert(sizeof(P_HookProcessIncomingDamage) == 0x148, "AHBaseCharacter.ProcessIncomingDamage params must match Dumper-7");
    struct P_HookStartVersusQTE
    {
        void* InQTEData;
        void* InAssaulter;
        void* InVictim;
        TArray<void*> InAdditionalActors;
    };
    static_assert(sizeof(P_HookDeathEventOwner) == 0x08, "AHCharacterEventUtils death owner param must match SDK");
    static_assert(sizeof(P_HookGameplayCharacterDied) == 0x08, "GameplayEventSubsystem death owner param must match SDK prefix");
    static_assert(sizeof(P_HookStartVersusQTE) == 0x28, "QTESubsystem.StartVersusQTE params must match Dumper-7");

    struct P_SetActorHiddenInGame { bool bNewHidden; };
    struct P_SetActorTickEnabled  { bool bEnabled; };
    struct P_SetVisibility        { bool bNewVisibility; bool bPropagateToChildren; };
    struct P_SetHiddenInGame      { bool bNewHidden; bool bPropagateToChildren; };
    struct P_TeleportTo
    {
        FVector  DestLocation;
        FRotator DestRotation;
        bool     ReturnValue;
        uint8_t  _pad0[3];
    };

    bool KeyDown(int vk)
    {
        return (GetAsyncKeyState(vk) & 0x8000) != 0;
    }

    float ClampDeltaSeconds(float dt)
    {
        if (dt <= 0.0f) return 1.0f / 60.0f;
        if (dt > 0.05f) return 0.05f;
        return dt;
    }

    bool ReadActorLocation(UObject* actor, FVector& out)
    {
        UFunction* getLoc = CachedFn(AH::Fn_GetActorLocation);
        if (!actor || !getLoc) return false;

        P_GetActorLocation p{};
        actor->ProcessEvent(getLoc, &p);
        out = p.ReturnValue;
        return true;
    }

    UClass* ResolvePawnClass();
    bool PawnUsable(UObject* pawn);

    bool ReadActorLocationFast(UObject* actor, FVector& out)
    {
        uint8_t* base = reinterpret_cast<uint8_t*>(actor);
        if (!Mem::IsReadable(base + Offsets::O_Actor_RootComponent, sizeof(void*)))
            return false;

        uint8_t* root = *reinterpret_cast<uint8_t**>(base + Offsets::O_Actor_RootComponent);
        if (!Mem::IsReadable(root + Offsets::O_Scene_RelativeLocation, sizeof(FVector)))
            return false;

        out = *reinterpret_cast<FVector*>(root + Offsets::O_Scene_RelativeLocation);
        return std::isfinite(out.X) && std::isfinite(out.Y) && std::isfinite(out.Z);
    }

    // Follow is intentionally broader than combat. Combat/team calls stay gated
    // to AHAICharacter; generic follow only needs a real APawn with a readable
    // root/location so quest NPCs and scripted boss pawns are not pruned.
    bool FollowPawnUsable(UObject* pawn)
    {
        FVector loc{};
        if (PawnUsable(pawn))
            return ReadActorLocationFast(pawn, loc);

        // During the first few frames after injection/spawn, the async short-name
        // index may not have resolved Engine.Pawn yet. Do not prune squad members
        // in that window; just keep them until the class check becomes available.
        if (!Mem::IsReadable(ResolvePawnClass(), 0x30))
            return Mem::IsReadable(pawn, 0x2A0) && ReadActorLocationFast(pawn, loc);
        return false;
    }

    bool SetActorLocation(UObject* actor, const FVector& loc, bool logFailure)
    {
        UFunction* setLoc = CachedFn(AH::Fn_SetActorLocation);
        if (!actor || !setLoc)
        {
            if (logFailure)
                LOG("SetActorLocation failed: actor=%p setLoc=%p", (void*)actor, (void*)setLoc);
            return false;
        }

        P_SetActorLocation p{};
        p.NewLocation = loc;
        p.bSweep = false;
        p.bTeleport = true;
        actor->ProcessEvent(setLoc, &p);

        if (!p.ReturnValue && logFailure)
            LOG("SetActorLocation returned false for %.1f %.1f %.1f", loc.X, loc.Y, loc.Z);
        return p.ReturnValue;
    }

    // Noclip: toggle the pawn's collision so it passes through level geometry.
    // Paired with free-fly so the player doesn't fall once collision is off.
    bool SetActorCollisionEnabled(UObject* actor, bool enabled)
    {
        UFunction* fn = CachedFn(AH::Fn_SetActorEnableCollision);
        if (!actor || !fn)
        {
            LOG("SetActorCollisionEnabled failed: actor=%p fn=%p", (void*)actor, (void*)fn);
            return false;
        }

        P_BoolParam p{ enabled };
        actor->ProcessEvent(fn, &p);
        LOG("Noclip: actor collision %s", enabled ? "ENABLED" : "DISABLED");
        return true;
    }

    bool SetActorScale3D(UObject* actor, const FVector& scale)
    {
        UFunction* fn = CachedFn(AH::Fn_SetActorScale3D);
        if (!actor || !fn)
            return false;
        struct { FVector NewScale3D; } p{ scale };
        actor->ProcessEvent(fn, &p);
        return true;
    }

    bool GetControlRotation(FRotator& out)
    {
        UObject* pc = GetPlayerController();
        UFunction* getRot = CachedFn(AH::Fn_GetControlRotation);
        if (!pc || !getRot) return false;

        P_GetControlRotation p{};
        pc->ProcessEvent(getRot, &p);
        out = p.ReturnValue;
        return true;
    }

    UObject* ResolveWorldStreamingSubsystem()
    {
        static UObject* cached = nullptr;
        static bool loggedMissing = false;

        if (Mem::IsReadable(cached, 0x30))
            return cached;

        if (UObject* live = CachedObject("BP_WorldStreamingSubsystem_C_0"))
        {
            cached = live;
            loggedMissing = false;
            LOG("Resolved world streaming subsystem object -> %p", (void*)cached);
            return cached;
        }

        UObject* lib = CachedObject("SubsystemUtils AtomicHeart.Default__SubsystemUtils");
        UFunction* fn = CachedFn(AH::Fn_GetAHWorldStreamingSubsystem);
        if (lib && fn)
        {
            P_ObjectReturn p{};
            lib->ProcessEvent(fn, &p);
            cached = static_cast<UObject*>(p.ReturnValue);
            if (cached)
            {
                loggedMissing = false;
                LOG("Resolved world streaming subsystem via SubsystemUtils -> %p", (void*)cached);
                return cached;
            }
        }

        if (!loggedMissing)
        {
            LOG("World streaming subsystem unavailable.");
            loggedMissing = true;
        }
        return nullptr;
    }

    bool InvalidateStreaming(UObject* context)
    {
        UObject* lib = CachedObject("StreamingUtils AtomicHeart.Default__StreamingUtils");
        UFunction* fn = CachedFn(AH::Fn_InvalidateStreaming);
        UObject* worldContext = context ? context : GetWorld();
        if (!lib || !fn || !worldContext)
            return false;

        P_WorldContext p{ worldContext };
        lib->ProcessEvent(fn, &p);
        return true;
    }

    bool EnableLevelStreamingUpdate()
    {
        UObject* subsystem = ResolveWorldStreamingSubsystem();
        UFunction* fn = CachedFn(AH::Fn_EnableLevelStreaming);
        if (!subsystem || !fn)
            return false;

        P_BoolParam p{ true }; // bUpdateStreamingVolumes
        subsystem->ProcessEvent(fn, &p);
        return true;
    }

    UObject* ResolveDebugSubsystem()
    {
        static UObject* cached = nullptr;
        static bool loggedMissing = false;

        if (Mem::IsReadable(cached, 0x30))
            return cached;

        cached = CachedObject("DebugSubsystem_0");
        if (cached)
        {
            loggedMissing = false;
            LOG("Resolved debug subsystem -> %p", (void*)cached);
            return cached;
        }

        if (!loggedMissing)
        {
            LOG("Debug subsystem unavailable; puzzle helpers will retry.");
            loggedMissing = true;
        }
        return nullptr;
    }

    bool ApplyInstantPuzzleResolve(bool enabled, bool logResult)
    {
        UObject* subsystem = ResolveDebugSubsystem();
        UFunction* fn = CachedFn(AH::Fn_Debug_SetInstantPuzzleResolve);
        if (!subsystem || !fn)
        {
            if (logResult)
                LOG("InstantPuzzleResolve failed: subsystem=%p fn=%p", (void*)subsystem, (void*)fn);
            return false;
        }

        P_BoolParam p{ enabled };
        subsystem->ProcessEvent(fn, &p);
        if (logResult)
            LOG("InstantPuzzleResolve -> %s", enabled ? "ON" : "OFF");
        return true;
    }

    bool CallDebugNoParams(const char* functionName, const char* label)
    {
        UObject* subsystem = ResolveDebugSubsystem();
        UFunction* fn = CachedFn(functionName);
        if (!subsystem || !fn)
        {
            LOG("%s failed: subsystem=%p fn=%p", label, (void*)subsystem, (void*)fn);
            return false;
        }

        uint8_t noParams = 0;
        subsystem->ProcessEvent(fn, &noParams);
        LOG("%s called", label);
        return true;
    }

    void UpdateInstantPuzzleResolveToggle()
    {
        static bool applied = false;
        static bool lastAppliedValue = false;
        static ULONGLONG lastAttemptMs = 0;

        bool desired = Features::Get().instantPuzzleResolve;
        if (!desired && !applied)
            return;

        if (applied && desired == lastAppliedValue)
            return;

        ULONGLONG nowMs = GetTickCount64();
        if (lastAttemptMs && nowMs - lastAttemptMs < 1000)
            return;

        lastAttemptMs = nowMs;
        if (ApplyInstantPuzzleResolve(desired, true))
        {
            applied = true;
            lastAppliedValue = desired;
        }
    }

    // ---- Puzzle completion (minigames + locks) ----------------------------
    // The DebugSubsystem trio (InstantPuzzleResolve/InstantLockUnlock/WinQTE)
    // only covers the rotating lock + QTE puzzles. The interactive puzzles it
    // ignores fall into two families, both handled here by the same machinery:
    //   * MiniGames -> BPC_MiniGameBase_C components (tri-way dials, etc.),
    //                  completed via MiniGame_SetComplete / SetGameComplete.
    //   * Locks     -> BP_LockComponent_C components (the CodeLock button grid
    //                  from the screenshot, ColorsLockPick, CoinLock,
    //                  UniversalLock), opened via Unlock().
    //
    // Work is split across threads ON PURPOSE (README: "resolving runs from the
    // worker thread, not the Present hook"). The worker thread does the heavy,
    // read-only DISCOVERY (resolve each target class + its functions, IsA-sweep
    // GObjects for live instances) and the render thread fires ProcessEvent on
    // what was found. The first attempt scanned GObjects on the render thread per
    // click and froze the game for ~12s.
    struct PuzzleTarget
    {
        const char* classNeedle;  // FindObject needle for the component/actor UClass
        const char* fn1;          // short fn name (resolved via class Children)
        const char* fn2;          // optional second fn (nullptr if unused)
        int         flagOffset;   // dedupe flag offset (-1 = none)
        bool        skipWhenFlag; // poll mode skips an instance when flag == this
        bool        verboseOnly;  // only fire on button/enable-edge (for targets with no dedupe flag)
        const char* label;
    };

    const PuzzleTarget kPuzzleTargets[] =
    {
        // minigames: once IsGameComplete is already true there is nothing to do
        { AH::Cls_MiniGameBase,   AH::Fn_MiniGame_SetComplete,       AH::Fn_MiniGame_GameComplete,
          AH::MiniGame_IsGameComplete, true,  false, "minigame" },
        // simple door/container locks: once IsLocked is false the lock is open
        { AH::Cls_LockComponent,  AH::Fn_Lock_Unlock,                nullptr,
          AH::Lock_IsLocked,           false, false, "lock" },
        // multi-part UniversalLock actor (the CodeLock button-grid door). No
        // simple solved-bool, so fire only on an explicit press / enable-edge.
        { AH::Cls_UniversalLock,  AH::Fn_UniversalLock_CompleteAll,  AH::Fn_UniversalLock_CompleteAllEvent,
          -1,                          false, true,  "universallock" },
    };
    constexpr int kNumPuzzleTargets = (int)(sizeof(kPuzzleTargets) / sizeof(kPuzzleTargets[0]));

    struct ResolvedTarget
    {
        UClass* cls = nullptr;
        UFunction* fn1 = nullptr;
        UFunction* fn2 = nullptr;
        ULONGLONG lastSlowClassScanMs = 0;
    };
    struct PendingCompletion { UObject* obj; UFunction* fn1; UFunction* fn2; const char* label; };

    std::mutex                     g_puzzleMutex;
    std::vector<PendingCompletion> g_puzzlePending;           // discovered; render thread drains
    std::atomic<bool>              g_puzzleSolveOnce{ false }; // one-shot "Solve" button request
    constexpr int                  kPuzzleCompletionsPerFrame = 4;

    std::string LastObjectToken(const char* name)
    {
        std::string text = name ? name : "";
        size_t pos = text.find_last_of("./ ");
        if (pos == std::string::npos)
            return text;
        if (pos + 1 >= text.size())
            return {};
        return text.substr(pos + 1);
    }

    // Diagnostic (worker thread, one-shot). When we can't find a puzzle
    // component, dump every live puzzle-ish object + its class so we can see what
    // the puzzle in front of the player actually is. Heavy (GetName over all of
    // GObjects) -- only run on an explicit button press, never in the poll.
    void DumpPuzzleCandidates()
    {
        int n = NumObjects();
        std::unordered_map<std::string, int> classCounts;
        int logged = 0;
        LOG("PuzzleCandidate scan start (objects=%d)", n);
        for (int i = 0; i < n; ++i)
        {
            UObject* o = GetObjectByIndex(i);
            if (!Mem::IsReadable(o, 0x30)) continue;
            UObject* cls = o->Class();
            if (!Mem::IsReadable(cls, 0x30)) continue;

            std::string cn = cls->GetName();
            std::string on = o->GetName();
            // Case-sensitive on purpose: capitalized keywords avoid matching
            // unrelated names (e.g. "Lock" won't hit "Blocking").
            static const char* kKw[] = {
                "MiniGame", "Minigame", "Puzzle", "Lockpick", "Lock", "Hack",
                "Polibino", "Dial", "Keypad", "Switch", "Valve", "Terminal",
                "Circuit", "Fuse", "Socket", "Numpad", "Sequence", "Polymer",
                "Neuro", "Interact", "Combination", "Code", "Panel", "Wheel" };
            auto has = [](const std::string& s, const char* k){ return s.find(k) != std::string::npos; };
            auto puzzleKw = [&](const std::string& s){
                for (const char* k : kKw) if (has(s, k)) return true;
                return false; };
            if (!puzzleKw(cn) && !puzzleKw(on)) continue;
            if (on.rfind("Default__", 0) == 0) continue;          // skip CDOs
            if (has(on, "GEN_VARIABLE")) continue;                // skip templates

            classCounts[cn]++;
            if (logged < 60)
            {
                LOG("PuzzleCandidate: %s", o->GetFullName().c_str());
                ++logged;
            }
        }
        LOG("PuzzleCandidate distinct live classes (non-default):");
        for (const auto& kv : classCounts)
            LOG("   %s  x%d", kv.first.c_str(), kv.second);
        if (classCounts.empty())
            LOG("PuzzleCandidate: none found -- no MiniGame/Puzzle/Lockpick objects are live right now");
    }

    void ResolveMissingPuzzleClassesSlow(ResolvedTarget resolved[kNumPuzzleTargets])
    {
        std::string tokens[kNumPuzzleTargets];
        bool anyMissing = false;
        for (int t = 0; t < kNumPuzzleTargets; ++t)
        {
            if (!Mem::IsReadable(resolved[t].cls, 0x30))
            {
                tokens[t] = LastObjectToken(kPuzzleTargets[t].classNeedle);
                if (!tokens[t].empty())
                    anyMissing = true;
            }
        }
        if (!anyMissing)
            return;

        ULONGLONG startMs = GetTickCount64();
        int n = NumObjects();
        int candidates = 0;
        int resolvedCount = 0;
        for (int i = 0; i < n; ++i)
        {
            UObject* o = GetObjectByIndex(i);
            if (!Mem::IsReadable(o, 0x30))
                continue;

            std::string objectName = o->GetName();
            for (int t = 0; t < kNumPuzzleTargets; ++t)
            {
                ResolvedTarget& r = resolved[t];
                if (Mem::IsReadable(r.cls, 0x30) || objectName != tokens[t])
                    continue;

                ++candidates;
                if (o->GetFullName().find(kPuzzleTargets[t].classNeedle) != std::string::npos)
                {
                    r.cls = o;
                    r.fn1 = r.fn2 = nullptr;
                    ++resolvedCount;
                    LOG("Resolved puzzle target '%s' class by slow pass -> %p",
                        kPuzzleTargets[t].label, (void*)r.cls);
                    break;
                }
            }
        }

        ULONGLONG elapsedMs = GetTickCount64() - startMs;
        if (elapsedMs > 100)
        {
            LOG("Puzzle class slow pass: resolved=%d candidates=%d objects=%d time=%llums",
                resolvedCount, candidates, n, elapsedMs);
        }
    }

    // Worker thread only. Resolve classes through the fast name index first, with
    // a rare combined slow fallback for newly streamed puzzle packages. Then
    // a single GObjects sweep enqueues live, non-template, not-already-done
    // instances of any resolved target.
    //   verbose: explicit solve/enable-edge -> fuller counters, but no heavy dump.
    int DiscoverPuzzles(bool verbose)
    {
        static ResolvedTarget resolved[kNumPuzzleTargets];
        ULONGLONG nowMs = GetTickCount64();
        bool needSlowClassScan = false;

        int usable = 0;
        for (int t = 0; t < kNumPuzzleTargets; ++t)
        {
            ResolvedTarget& r = resolved[t];
            if (!Mem::IsReadable(r.cls, 0x30))
            {
                r.cls = FindObjectFast(kPuzzleTargets[t].classNeedle);
                if (!r.cls && (verbose || nowMs - r.lastSlowClassScanMs > 8000))
                {
                    r.lastSlowClassScanMs = nowMs;
                    needSlowClassScan = true;
                }
                r.fn1 = r.fn2 = nullptr;
                if (r.cls)
                    LOG("Resolved puzzle target '%s' class -> %p", kPuzzleTargets[t].label, (void*)r.cls);
            }
        }

        if (needSlowClassScan)
            ResolveMissingPuzzleClassesSlow(resolved);

        for (int t = 0; t < kNumPuzzleTargets; ++t)
        {
            ResolvedTarget& r = resolved[t];
            if (r.cls)
            {
                if (!r.fn1 && kPuzzleTargets[t].fn1) r.fn1 = FindFunctionInClass(r.cls, kPuzzleTargets[t].fn1);
                if (!r.fn2 && kPuzzleTargets[t].fn2) r.fn2 = FindFunctionInClass(r.cls, kPuzzleTargets[t].fn2);
                if (r.fn1 || r.fn2) ++usable;
            }
        }
        if (!usable)
            return 0; // no target package streamed in yet (player not near a puzzle)

        int n = NumObjects();
        int isaHits = 0, skippedTemplate = 0, skippedDone = 0;
        std::vector<PendingCompletion> found;
        for (int i = 0; i < n; ++i)
        {
            UObject* o = GetObjectByIndex(i);
            if (!Mem::IsReadable(o, 0x30))
                continue;

            for (int t = 0; t < kNumPuzzleTargets; ++t)
            {
                const PuzzleTarget& pt = kPuzzleTargets[t];
                ResolvedTarget& r = resolved[t];
                if (!r.cls || (!r.fn1 && !r.fn2))
                    continue;
                if (pt.verboseOnly && !verbose) // no-dedupe target: skip the steady poll
                    continue;
                if (!o->IsA(r.cls))
                    continue;
                ++isaHits;

                std::string name = o->GetName();
                if (name.rfind("Default__", 0) == 0 || name.find("GEN_VARIABLE") != std::string::npos)
                { ++skippedTemplate; break; }

                // Best-effort dedupe so continuous (poll) mode does not re-fire
                // forever. An explicit press (verbose) bypasses it: re-completing
                // is a harmless no-op and keeps the button working even if a
                // dedupe-flag offset is wrong for the build.
                if (!verbose && pt.flagOffset >= 0 &&
                    Mem::IsReadable(o, pt.flagOffset + 1) &&
                    *reinterpret_cast<bool*>((uint8_t*)o + pt.flagOffset) == pt.skipWhenFlag)
                { ++skippedDone; break; }

                found.push_back({ o, r.fn1, r.fn2, pt.label });
                break; // an object matches at most one target
            }
        }

        if (!found.empty())
        {
            std::lock_guard<std::mutex> lk(g_puzzleMutex);
            int loggedQueued = 0;
            int queuedNow = 0;
            for (const PendingCompletion& p : found)
            {
                bool duplicate = false;
                for (const PendingCompletion& q : g_puzzlePending)
                {
                    if (q.obj == p.obj && q.fn1 == p.fn1 && q.fn2 == p.fn2)
                    {
                        duplicate = true;
                        break;
                    }
                }
                if (duplicate)
                    continue;

                g_puzzlePending.push_back(p);
                ++queuedNow;
                if (loggedQueued < 6)
                {
                    LOG("  queue %-13s %s", p.label, p.obj->GetFullName().c_str());
                    ++loggedQueued;
                }
            }
            LOG("DiscoverPuzzles: queued %d new / %zu pending (isaHits=%d template=%d done=%d)",
                queuedNow, g_puzzlePending.size(), isaHits, skippedTemplate, skippedDone);
        }
        else
        {
            // Rate-limit so continuous mode doesn't spam.
            static ULONGLONG lastEmptyLogMs = 0;
            ULONGLONG nowMs = GetTickCount64();
            if (verbose || nowMs - lastEmptyLogMs > 3000)
            {
                lastEmptyLogMs = nowMs;
                LOG("DiscoverPuzzles: 0 queued (isaHits=%d template=%d done=%d)",
                    isaHits, skippedTemplate, skippedDone);
            }
            if (verbose)
                LOG("DiscoverPuzzles: diagnostic candidate dump skipped for stability");
        }
        return (int)found.size();
    }

    // Render thread only. Fires the completion functions on whatever the worker
    // discovered -- ProcessEvent stays on the render thread like every other
    // feature in this menu.
    void DrainPuzzleCompletions()
    {
        std::vector<PendingCompletion> batch;
        {
            std::lock_guard<std::mutex> lk(g_puzzleMutex);
            if (g_puzzlePending.empty())
                return;

            int take = (int)g_puzzlePending.size();
            if (take > kPuzzleCompletionsPerFrame)
                take = kPuzzleCompletionsPerFrame;
            batch.insert(batch.end(), g_puzzlePending.begin(), g_puzzlePending.begin() + take);
            g_puzzlePending.erase(g_puzzlePending.begin(), g_puzzlePending.begin() + take);
        }

        int done = 0;
        for (const PendingCompletion& p : batch)
        {
            if (!Mem::IsReadable(p.obj, 0x30))
                continue;
            try
            {
                if (p.fn1) p.obj->ProcessEvent(p.fn1, nullptr);
                if (p.fn2) p.obj->ProcessEvent(p.fn2, nullptr);
                ++done;
            }
            catch (...) { /* one bad instance must not abort the batch */ }
        }
        if (done)
            LOG("Puzzle completion fired on %d instance(s)", done);
    }

    // ---- AI cache + world-to-screen ESP -----------------------------------
    // A single worker scan feeds ESP and the world AI controls. Commands are
    // queued and drained in bounded batches from the render/game thread.
    constexpr float kUnitsPerMetre = 100.0f;
    constexpr int   kMaxCachedAiActors = 512;
    constexpr int   kMaxDeepAiTargets = 1024;
    constexpr int   kMaxAiCommandTargets = 96;
    constexpr int   kAiOpsPerFrame = 12;
    constexpr int   kAiAutoOpsPerPass = 8;
    // AI discovery walks the loaded levels' actor lists (a few thousand actors)
    // instead of sweeping every UObject (~360k), so a full rebuild is a few ms
    // and never hangs the worker thread or delays a command. The list is rebuilt
    // wholesale each refresh, so freshly spawned enemies appear immediately and
    // dead ones drop out.
    constexpr int       kMaxScannedLevels       = 4096;   // sanity cap on UWorld::Levels
    constexpr int       kMaxActorsPerLevel      = 100000; // sanity cap on ULevel::Actors
    constexpr int       kMaxScannedActors       = 20000;  // overall actor budget per rebuild
    constexpr ULONGLONG kAiDeferredCommandTtlMs = 15000;  // drop a queued command if discovery never finds targets

    struct AiCachedActor
    {
        UObject* actor = nullptr;
        int      index = -1;
        FVector  location{};
        float    distanceM = 0.0f;
        float    healthFrac = -1.0f;
    };

    enum class AiQueuedKind
    {
        None,
        Kill,
        PassiveOn,
        PassiveOff,
        Follow,
        FightEachOther,
        Release,
        Launch
    };

    struct AiQueuedOperation
    {
        uint64_t id = 0;
        AiQueuedKind kind = AiQueuedKind::None;
        std::vector<UObject*> targets;
        size_t index = 0;
        int applied = 0;
        UObject* player = nullptr;
        FVector playerLoc{};
    };

    std::mutex                 g_aiMutex;
    std::vector<AiCachedActor> g_aiActors;
    UClass*                    g_aiClass = nullptr;
    UClass*                    g_aiControllerClass = nullptr;
    UClass*                    g_pawnClass = nullptr;
    UClass*                    g_characterClass = nullptr;
    std::atomic<int>           g_aiCachedCount{ 0 };
    std::atomic<int>           g_aiPendingCount{ 0 };
    std::atomic<bool>          g_aiDiscoveryRequested{ false };
    // A World AI command pressed before discovery has populated the cache is
    // remembered here and fired automatically (render thread) once enemies are
    // found, so the buttons work on a single press instead of needing a retry.
    std::atomic<int>           g_aiDeferredKind{ 0 };          // (int)AiQueuedKind
    std::atomic<ULONGLONG>     g_aiDeferredMs{ 0 };
    // True while a game-thread AI pump task is queued/running. ALL AI ProcessEvent
    // work runs on the game thread (never the render thread -- see the pump note),
    // and only one pump is ever in flight so the queue can't pile up and stall.
    std::atomic<bool>          g_aiPumpInFlight{ false };

    // The loaded-level actor arrays are not authoritative for streamed enemies:
    // some live AHAICharacter instances are absent from every ULevel::Actors array.
    // Keep a worker-thread-only incremental GObjects discovery set and merge it into
    // every normal refresh. A full pass completes in under a second while an AI
    // feature is active, without putting a monolithic object sweep on the game thread.
    constexpr int                          kGlobalAiObjectsPerRefresh = 100000;
    size_t                                 g_globalAiScanCursor = 0;
    uint64_t                               g_globalAiScanGeneration = 0;
    std::unordered_map<UObject*, uint64_t> g_globalAiSeen;

    // THE SQUAD: every AI under our control -- both ones we spawned AND enemies we
    // recruited. All are driven as permanent bodyguards (fight nearby threats +
    // follow you) every pump, with no toggle: recruiting/spawning IS the control.
    // Guarded by g_squadMutex because the render thread reads it too (ESP highlight,
    // the menu roster, recruit/release). Capped so it can't grow without bound.
    std::mutex                 g_squadMutex;
    std::vector<UObject*>      g_spawnedAllies;          // = the squad (spawned + recruited)
    std::mutex                 g_hookBodyguardMutex;
    std::vector<UObject*>      g_hookBodyguards;         // Hook Diagnostics-owned subset
    // g_spawnedAllyCount (the size mirror) is declared near the top atomics so
    // hkProcessEvent can gate the per-frame squad walk on it.
    constexpr int              kMaxSpawnedAllies = 24;

    // UI selection set (render-thread only: menu writes, ESP + dispatch read, all in
    // the Present hook). Holds raw actor pointers; always re-validated before use.
    std::vector<UObject*>      g_selectedAi;

    bool IsSquadMember(UObject* ai)
    {
        std::lock_guard<std::mutex> lk(g_squadMutex);
        for (UObject* s : g_spawnedAllies) if (s == ai) return true;
        return false;
    }
    bool IsSelectedAi(UObject* ai)
    {
        for (UObject* s : g_selectedAi) if (s == ai) return true;
        return false;
    }
    bool IsHookBodyguard(UObject* ai)
    {
        std::lock_guard<std::mutex> lk(g_hookBodyguardMutex);
        for (UObject* guard : g_hookBodyguards) if (guard == ai) return true;
        return false;
    }
    void HookBodyguardAdd(UObject* ai)
    {
        if (!Mem::IsReadable(ai, 0x30)) return;
        {
            std::lock_guard<std::mutex> lk(g_hookBodyguardMutex);
            for (UObject* guard : g_hookBodyguards) if (guard == ai) return;
        }
        // Pointer reuse after K2_DestroyActor can otherwise inherit the previous
        // Hook guard's follow/combat maps. Clean old runtime state before registering.
        ForgetHookBodyguardRuntimeState(ai, false);
        std::lock_guard<std::mutex> lk(g_hookBodyguardMutex);
        for (UObject* guard : g_hookBodyguards) if (guard == ai) return;
        g_hookBodyguards.push_back(ai);
    }
    void HookBodyguardRemove(UObject* ai)
    {
        {
            std::lock_guard<std::mutex> lk(g_hookBodyguardMutex);
            for (size_t i = 0; i < g_hookBodyguards.size(); ++i)
                if (g_hookBodyguards[i] == ai) { g_hookBodyguards.erase(g_hookBodyguards.begin() + i); break; }
        }
        {
            std::lock_guard<std::mutex> lock(g_hookControllerOwnedMutex);
            g_hookControllerOwned.erase(ai);
        }
        AiMovementHooks::UnregisterGuard(ai);
    }
    std::vector<UObject*> HookBodyguardSnapshot()
    {
        std::lock_guard<std::mutex> lk(g_hookBodyguardMutex);
        return g_hookBodyguards;
    }
    UObject* HookDebugSelectedGuard()
    {
        for (UObject* selected : g_selectedAi)
            if (Mem::IsReadable(selected, 0x30) && IsHookBodyguard(selected))
                return selected;
        std::lock_guard<std::mutex> lk(g_hookBodyguardMutex);
        for (UObject* guard : g_hookBodyguards)
            if (Mem::IsReadable(guard, 0x30))
                return guard;
        return nullptr;
    }
    // Add to the squad (any thread; locked). Dedup + cap (drop oldest).
    void SquadAdd(UObject* ai)
    {
        if (!Mem::IsReadable(ai, 0x30)) return;
        UObject* evicted = nullptr;
        {
            std::lock_guard<std::mutex> lk(g_squadMutex);
            for (UObject* s : g_spawnedAllies) if (s == ai) return;
            if ((int)g_spawnedAllies.size() >= kMaxSpawnedAllies)
            {
                evicted = g_spawnedAllies.front();
                g_spawnedAllies.erase(g_spawnedAllies.begin());
            }
            g_spawnedAllies.push_back(ai);
            g_spawnedAllyCount = (int)g_spawnedAllies.size();
        }
        if (evicted)
            QueueGameThread([evicted]()
            {
                ReleaseHookNativeMovement(evicted, true);
                HookBodyguardRemove(evicted);
                ForgetHookBodyguardRuntimeState(evicted, true);
            });
    }
    // Remove from the squad (any thread; locked). Used when an actor is deleted from
    // the world so the follow drive stops poking a dead pointer.
    void SquadRemove(UObject* ai)
    {
        {
            std::lock_guard<std::mutex> lk(g_squadMutex);
            for (size_t i = 0; i < g_spawnedAllies.size(); ++i)
                if (g_spawnedAllies[i] == ai) { g_spawnedAllies.erase(g_spawnedAllies.begin() + i); break; }
            g_spawnedAllyCount = (int)g_spawnedAllies.size();
        }
        HookBodyguardRemove(ai);
    }

    // ---- AI ownership lock: the swallow decision (declared up by hkProcessEvent) ----
    // A controller -> its possessed pawn (the character). Guarded; null on any bad
    // read so the caller simply treats the object as "not one of ours".
    UObject* OwnershipControllerPawn(void* ctrl)
    {
        uint8_t* c = reinterpret_cast<uint8_t*>(ctrl);
        if (!Mem::IsReadable(c + Offsets::O_BaseController_Pawn, sizeof(void*)))
            return nullptr;
        return *reinterpret_cast<UObject**>(c + Offsets::O_BaseController_Pawn);
    }

    // Should this ProcessEvent dispatch be swallowed to keep a Hook Debug unit ours?
    // Only ever true for: (a) one of three cached "un-ally" UFunctions, dispatched
    // (b) on a Hook Diagnostics character or its controller, (c) in the
    // direction that would turn it against the player / another hook bodyguard. Our
    // OWN friendly / attack-real-enemy calls never match, so they pass through.
    bool OwnershipShouldSwallow(void* obj, void* fn, void* params)
    {
        void* fSwitch = g_fnOwnSwitchTeamAttitude.load(std::memory_order_relaxed);
        void* fEnemyC = g_fnOwnSetTargetEnemy.load(std::memory_order_relaxed);
        void* fEnemyB = g_fnOwnSetBbTargetEnemy.load(std::memory_order_relaxed);
        if (fn != fSwitch && fn != fEnemyC && fn != fEnemyB)
            return false;                       // cheap reject (the hot path)
        if (!Mem::IsReadable(params, 0x10))
            return false;

        UObject* o = reinterpret_cast<UObject*>(obj);
        bool owned = IsHookBodyguard(o);
        if (!owned)
        {
            UObject* pawn = OwnershipControllerPawn(obj); // obj may be the controller
            owned = pawn && IsHookBodyguard(pawn);
        }
        if (!owned)
            return false;                       // never touch AI we do not own

        UObject* player = UE::GetLocalPawn();
        auto isUs = [&](void* x) -> bool {
            return x && (x == player || IsHookBodyguard(reinterpret_cast<UObject*>(x)));
        };

        if (fn == fSwitch)
        {
            // Block the engine flipping our unit to a NON-friendly attitude toward us.
            // Our own SwitchTeamToMatchCharacterAttitude(player, Friendly=0) passes.
            auto* p = reinterpret_cast<P_SwitchTeamToMatchCharacterAttitude*>(params);
            return isUs(p->OtherCharacter) && p->TargetTeamAttitude != 0;
        }
        if (fn == fEnemyC)
        {
            // Never let our unit take US as its enemy (real-enemy targets pass through).
            auto* p = reinterpret_cast<P_SetTargetEnemy*>(params);
            return isUs(p->TargetEnemy);
        }
        // fn == fEnemyB
        auto* p = reinterpret_cast<P_SetBlackboardTargetEnemy*>(params);
        return isUs(p->NewTarget);
    }

    bool HookMovementShouldSwallow(void* obj, void* fn)
    {
        if (t_hookMovementInternal)
            return false;
        void* moveActor = g_fnHookMoveToActor.load(std::memory_order_relaxed);
        void* moveLocation = g_fnHookMoveToLocation.load(std::memory_order_relaxed);
        void* stop = g_fnHookStopMovement.load(std::memory_order_relaxed);
        if (fn != moveActor && fn != moveLocation && fn != stop)
            return false;
        UObject* pawn = OwnershipControllerPawn(obj);
        if (!pawn || !IsHookBodyguard(pawn))
            return false;
        std::lock_guard<std::mutex> lock(g_hookControllerOwnedMutex);
        return g_hookControllerOwned.find(pawn) != g_hookControllerOwned.end();
    }

    bool HookStaleCombatTargetShouldSwallow(void* obj, void* fn, void* params)
    {
        if (!g_hookBodyguardMode.load(std::memory_order_relaxed) || !fn || !params)
            return false;
        void* fCharTarget = g_fnOwnSetTargetEnemy.load(std::memory_order_relaxed);
        void* fBbTarget = g_fnOwnSetBbTargetEnemy.load(std::memory_order_relaxed);
        void* fAggressive = g_fnHookSetCharacterAggressive.load(std::memory_order_relaxed);
        if (fn != fCharTarget && fn != fBbTarget && fn != fAggressive)
            return false;

        UObject* guard = nullptr;
        UObject* target = nullptr;
        bool activeAggressive = false;
        if (fn == fCharTarget && Mem::IsReadable(params, sizeof(P_SetTargetEnemy)))
        {
            guard = reinterpret_cast<UObject*>(obj);
            target = reinterpret_cast<UObject*>(reinterpret_cast<P_SetTargetEnemy*>(params)->TargetEnemy);
        }
        else if (fn == fBbTarget && Mem::IsReadable(params, sizeof(P_SetBlackboardTargetEnemy)))
        {
            guard = OwnershipControllerPawn(obj);
            target = reinterpret_cast<UObject*>(reinterpret_cast<P_SetBlackboardTargetEnemy*>(params)->NewTarget);
        }
        else if (fn == fAggressive && Mem::IsReadable(params, sizeof(P_SetCharacterAggressive)))
        {
            auto* p = reinterpret_cast<P_SetCharacterAggressive*>(params);
            guard = reinterpret_cast<UObject*>(p->InAICharacter);
            target = reinterpret_cast<UObject*>(p->InTargetEnemy);
            activeAggressive = p->bShouldBeActive;
        }
        if (!guard || !IsHookBodyguard(guard) || !target)
            return false;

        ULONGLONG now = GetTickCount64();
        auto sup = g_hookSuppressedThreatUntilMs.find(target);
        bool suppressed = (sup != g_hookSuppressedThreatUntilMs.end() && sup->second > now);
        bool tombstoned = (g_aiDeathEventMs.find(target) != g_aiDeathEventMs.end());
        if (!suppressed && !tombstoned)
            return false;

        // This is the important part: do not let the game's latent AI/fight-staging
        // script rehydrate the just-killed robot back into TargetEnemy after our
        // stand-down. Swallow the stale target write / stale aggressive kick entirely;
        // the follow driver will issue a clean player-follow route next frame.
        if (IsHookTwinBodyguard(guard))
            ClearFightStagingSelectedObject(guard);
        static ULONGLONG lastLog = 0;
        if (now - lastLog > 500)
        {
            lastLog = now;
            LOG("[AI-HOOK] swallowed stale combat target dispatch guard=%p target=%p fn=%p active=%d suppressed=%d tombstoned=%d",
                (void*)guard, (void*)target, fn, activeAggressive ? 1 : 0, suppressed ? 1 : 0, tombstoned ? 1 : 0);
        }
        return true;
    }

    void HookCombatTrace(void* obj, void* fn, void* params)
    {
        void* fCharTarget = g_fnOwnSetTargetEnemy.load(std::memory_order_relaxed);
        void* fBbTarget = g_fnOwnSetBbTargetEnemy.load(std::memory_order_relaxed);
        void* fAggressive = g_fnHookSetCharacterAggressive.load(std::memory_order_relaxed);
        void* fPassive = g_fnHookSetCharacterPassive.load(std::memory_order_relaxed);
        if (fn != fCharTarget && fn != fBbTarget && fn != fAggressive && fn != fPassive) return;

        UObject* guard = nullptr;
        UObject* target = nullptr;
        int active = -1;
        if (fn == fCharTarget)
        {
            guard = reinterpret_cast<UObject*>(obj);
            target = reinterpret_cast<UObject*>(reinterpret_cast<P_SetTargetEnemy*>(params)->TargetEnemy);
        }
        else if (fn == fBbTarget)
        {
            guard = OwnershipControllerPawn(obj);
            target = reinterpret_cast<UObject*>(reinterpret_cast<P_SetBlackboardTargetEnemy*>(params)->NewTarget);
        }
        else if (fn == fAggressive)
        {
            auto* p = reinterpret_cast<P_SetCharacterAggressive*>(params);
            guard = reinterpret_cast<UObject*>(p->InAICharacter);
            target = reinterpret_cast<UObject*>(p->InTargetEnemy);
            active = p->bShouldBeActive ? 1 : 0;
        }
        else
        {
            auto* p = reinterpret_cast<P_SetCharacterPassive*>(params);
            guard = reinterpret_cast<UObject*>(p->InAICharacter);
            active = 0;
        }
        if (!guard || !IsHookBodyguard(guard)) return;

        uint64_t count = 0;
        const char* stage = nullptr;
        if (fn == fAggressive || fn == fPassive)
        {
            count = g_hookCombatStateEvents.fetch_add(1, std::memory_order_relaxed) + 1;
            stage = fn == fAggressive ? "SetCharacterAggressive" : "SetCharacterPassive";
        }
        else
        {
            count = g_hookCombatTargetEvents.fetch_add(1, std::memory_order_relaxed) + 1;
            stage = fn == fCharTarget ? "SetTargetEnemy" : "SetBlackboardTargetEnemy";
        }
        static ULONGLONG lastCombatTraceMs = 0;
        ULONGLONG now = GetTickCount64();
        if (now - lastCombatTraceMs >= 750)
        {
            lastCombatTraceMs = now;
            LOG("[AI-CHAIN] combat stage=%s guard=%p target=%p active=%d count=%llu",
                stage,(void*)guard,(void*)target,active,(unsigned long long)count);
        }
    }

    bool HookFriendshipShouldForce(void* fn, void* params)
    {
        if (!g_hookBodyguardMode.load(std::memory_order_relaxed) ||
            fn != g_fnHookAreFriendly.load(std::memory_order_relaxed) ||
            !Mem::IsReadable(params, sizeof(P_AreFriendlyCharacters)))
            return false;

        auto* p = reinterpret_cast<P_AreFriendlyCharacters*>(params);
        UObject* a = reinterpret_cast<UObject*>(p->CharacterOne);
        UObject* b = reinterpret_cast<UObject*>(p->CharacterTwo);
        UObject* player = GetLocalPawn();
        if (!Mem::IsReadable(player, 0x30))
            return false;

        UObject* guard = nullptr;
        if (a == player && b && IsHookBodyguard(b)) guard = b;
        if (b == player && a && IsHookBodyguard(a)) guard = a;
        if (!guard || !NativeHooks::IsAHAICharacter(guard))
            return false;

        p->ReturnValue = true;
        uint64_t n = g_friendshipForces.fetch_add(1, std::memory_order_relaxed) + 1;
        static ULONGLONG lastLogMs = 0;
        ULONGLONG now = GetTickCount64();
        if (now - lastLogMs > 1000)
        {
            lastLogMs = now;
            LOG("[AI-FRIENDSHIP] forcing friendly for player/bodyguard pair (total %llu)",
                (unsigned long long)n);
        }
        return true;
    }

    std::mutex        g_aiOpMutex;
    AiQueuedOperation g_aiOp;
    uint64_t          g_aiNextOpId = 1;

    // CRITICAL crash guard. AI actor pointers are cached across frames; an actor
    // can be destroyed and its memory reused before a command reaches it. Calling
    // an AHAICharacter-specific UFunction on a stale/wrong object dispatches into
    // game code, where UE4's own exception handler traps the access violation
    // BEFORE our catch(...) can -- so it hard-crashes. Re-validate the exact type
    // right before every AI ProcessEvent so we never make that bad call. IsA only
    // does guarded reads, so it can't crash even on freed memory.
    bool AiUsable(UObject* ai)
    {
        if (!Mem::IsReadable(ai, 0x30))
            return false;
        UClass* cls = g_aiClass;
        if (!Mem::IsReadable(cls, 0x30))
            return false; // class not resolved yet: don't risk a type-specific call
        return ai->IsA(cls);
    }

    UClass* ResolvePawnClass()
    {
        if (!Mem::IsReadable(g_pawnClass, 0x30))
            g_pawnClass = FindObjectFast(AH::Cls_Pawn);
        return Mem::IsReadable(g_pawnClass, 0x30) ? g_pawnClass : nullptr;
    }

    bool PawnUsable(UObject* pawn)
    {
        if (!Mem::IsReadable(pawn, 0x30))
            return false;
        UClass* cls = ResolvePawnClass();
        if (!Mem::IsReadable(cls, 0x30))
            return false;
        return pawn->IsA(cls);
    }

    UClass* ResolveCharacterClass()
    {
        if (!Mem::IsReadable(g_characterClass, 0x30))
            g_characterClass = FindObjectFast(AH::Cls_Character);
        return Mem::IsReadable(g_characterClass, 0x30) ? g_characterClass : nullptr;
    }

    bool CharacterUsable(UObject* ch)
    {
        if (!Mem::IsReadable(ch, 0x30))
            return false;
        UClass* cls = ResolveCharacterClass();
        if (!Mem::IsReadable(cls, 0x30))
            return false;
        return ch->IsA(cls);
    }

    UClass* ResolveAiControllerClass()
    {
        if (!Mem::IsReadable(g_aiControllerClass, 0x30))
            g_aiControllerClass = FindObjectFast(AH::Cls_AIController);
        return Mem::IsReadable(g_aiControllerClass, 0x30) ? g_aiControllerClass : nullptr;
    }

    bool AiControllerUsable(UObject* ctrl)
    {
        if (!Mem::IsReadable(ctrl, 0x30))
            return false;
        UClass* cls = ResolveAiControllerClass();
        if (!Mem::IsReadable(cls, 0x30))
            return false;
        return ctrl->IsA(cls);
    }

    bool ControllerOwnsPawn(UObject* ctrl, UObject* pawn)
    {
        if (!AiControllerUsable(ctrl) || !Mem::IsReadable(pawn, 0x30))
            return false;
        uint8_t* base = reinterpret_cast<uint8_t*>(ctrl);
        if (!Mem::IsReadable(base + Offsets::O_BaseController_Pawn, sizeof(void*)))
            return false;
        UObject* owned = *reinterpret_cast<UObject**>(base + Offsets::O_BaseController_Pawn);
        return owned == pawn && Mem::IsReadable(owned, 0x30);
    }

    bool ControllerPathFollowingReady(UObject* ctrl, UObject* pawn)
    {
        if (!ControllerOwnsPawn(ctrl, pawn))
            return false;
        uint8_t* base = reinterpret_cast<uint8_t*>(ctrl);
        if (!Mem::IsReadable(base + AH::AICtrl_PathFollowing, sizeof(void*)))
            return false;
        UObject* path = *reinterpret_cast<UObject**>(base + AH::AICtrl_PathFollowing);
        return Mem::IsReadable(path, 0x30);
    }

    bool ReadCameraPOV(FVector& loc, FRotator& rot, float& fov)
    {
        UObject* pc = GetPlayerController();
        uint8_t* pcb = reinterpret_cast<uint8_t*>(pc);
        if (!Mem::IsReadable(pcb + Offsets::O_PC_CameraManager, 8)) return false;
        uint8_t* cam = *reinterpret_cast<uint8_t**>(pcb + Offsets::O_PC_CameraManager);
        if (!Mem::IsReadable(cam + Offsets::O_CamMgr_POV_FOV, 4)) return false;
        loc = *reinterpret_cast<FVector*>(cam + Offsets::O_CamMgr_POV_Location);
        rot = *reinterpret_cast<FRotator*>(cam + Offsets::O_CamMgr_POV_Rotation);
        fov = *reinterpret_cast<float*>(cam + Offsets::O_CamMgr_POV_FOV);
        return fov > 1.0f && fov < 170.0f;
    }

    // Standard UE FRotationMatrix axes -> perspective projection.
    bool WorldToScreen(const FVector& world, const FVector& camLoc, const FRotator& camRot,
                       float fov, float w, float h, float& sx, float& sy)
    {
        const float d2r = 3.14159265358979f / 180.0f;
        float cp = cosf(camRot.Pitch * d2r), sp = sinf(camRot.Pitch * d2r);
        float cy = cosf(camRot.Yaw   * d2r), yawSin = sinf(camRot.Yaw   * d2r);
        float cr = cosf(camRot.Roll  * d2r), sr = sinf(camRot.Roll  * d2r);

        FVector fwd   = { cp * cy,                   cp * yawSin,                   sp };
        FVector right = { sr * sp * cy - cr * yawSin, sr * sp * yawSin + cr * cy, -sr * cp };
        FVector up    = { -(cr * sp * cy + sr * yawSin), cy * sr - cr * sp * yawSin, cr * cp };

        FVector d = { world.X - camLoc.X, world.Y - camLoc.Y, world.Z - camLoc.Z };
        float depth = d.X * fwd.X + d.Y * fwd.Y + d.Z * fwd.Z;
        if (depth < 1.0f) return false; // behind camera

        float rDot = d.X * right.X + d.Y * right.Y + d.Z * right.Z;
        float uDot = d.X * up.X    + d.Y * up.Y    + d.Z * up.Z;
        float halfW = w * 0.5f, halfH = h * 0.5f;
        float t = tanf(fov * d2r * 0.5f);
        if (t < 1e-4f) t = 1e-4f;
        float focal = halfW / t;
        sx = halfW + rDot * focal / depth;
        sy = halfH - uDot * focal / depth;
        return true;
    }

    bool ReadCharacterHealth(UObject* ch, float& cur, float& mx)
    {
        uint8_t* base = reinterpret_cast<uint8_t*>(ch);
        if (!Mem::IsReadable(base + AH::Char_AttributeSet, 8)) return false;
        uint8_t* set = *reinterpret_cast<uint8_t**>(base + AH::Char_AttributeSet);
        if (!Mem::IsReadable(set, AH::Set_Health + AH::Attr_CurrentValue + 4)) return false;
        cur = *reinterpret_cast<float*>(set + AH::Set_Health    + AH::Attr_CurrentValue);
        mx  = *reinterpret_cast<float*>(set + AH::Set_MaxHealth + AH::Attr_CurrentValue);
        return true;
    }

    float DistanceMetres(const FVector& a, const FVector& b)
    {
        const float dx = a.X - b.X;
        const float dy = a.Y - b.Y;
        const float dz = a.Z - b.Z;
        return sqrtf(dx * dx + dy * dy + dz * dz) / kUnitsPerMetre;
    }

    bool ReadLocalPawnLocationFast(FVector& loc, UObject** outPawn = nullptr)
    {
        UObject* pawn = GetLocalPawn();
        if (outPawn) *outPawn = pawn;
        if (!pawn)
            return false;
        if (ReadActorLocationFast(pawn, loc))
            return true;
        return ReadActorLocation(pawn, loc);
    }

    std::vector<AiCachedActor> CopyAiSnapshot()
    {
        std::lock_guard<std::mutex> lk(g_aiMutex);
        return g_aiActors;
    }

    void RequestAiDiscovery()
    {
        g_aiDiscoveryRequested = true;
    }

    void RefreshAiRenderLocations(const std::vector<AiCachedActor>& snapshot)
    {
        if (snapshot.empty())
            return;

        constexpr int kMaxRenderLocationUpdates = 48;
        static size_t cursor = 0;

        struct LocationUpdate { UObject* actor; FVector location; };
        std::vector<LocationUpdate> updates;
        updates.reserve((std::min)((int)snapshot.size(), kMaxRenderLocationUpdates));

        const size_t count = snapshot.size();
        const int take = (std::min)((int)count, kMaxRenderLocationUpdates);
        if (cursor >= count)
            cursor = 0;

        for (int i = 0; i < take; ++i)
        {
            const AiCachedActor& e = snapshot[(cursor + (size_t)i) % count];
            if (!Mem::IsReadable(e.actor, 0x30))
                continue;

            // Fast = raw RootComponent read, no ProcessEvent. Calling a UFunction
            // on a possibly-stale actor every frame from the render thread is a
            // crash risk; the raw read is guarded and cheap.
            FVector liveLoc{};
            if (ReadActorLocationFast(e.actor, liveLoc))
                updates.push_back({ e.actor, liveLoc });
        }
        cursor = (cursor + (size_t)take) % count;

        if (updates.empty())
            return;

        std::lock_guard<std::mutex> lk(g_aiMutex);
        for (const LocationUpdate& u : updates)
        {
            for (AiCachedActor& e : g_aiActors)
            {
                if (e.actor == u.actor)
                {
                    e.location = u.location;
                    break;
                }
            }
        }
    }

    // Every cached enemy in the level (not radius-limited) -- for the "all" ops.
    std::vector<UObject*> CollectAllCachedAi(int maxTargets)
    {
        std::vector<AiCachedActor> snapshot = CopyAiSnapshot();
        std::vector<UObject*> out;
        out.reserve(snapshot.size());
        for (const AiCachedActor& e : snapshot)
        {
            if ((int)out.size() >= maxTargets)
                break;
            if (AiUsable(e.actor))
                out.push_back(e.actor);
        }
        return out;
    }

    std::vector<UObject*> CollectNearbyAi(float radiusM, int maxTargets, bool requireTwo = false)
    {
        std::vector<AiCachedActor> snapshot = CopyAiSnapshot();
        if (snapshot.empty())
            return {};

        FVector playerLoc{};
        bool havePlayerLoc = ReadLocalPawnLocationFast(playerLoc);
        if (!havePlayerLoc)
            return {};

        if (radiusM < 5.0f) radiusM = 5.0f;
        struct Candidate { UObject* actor; float distanceM; };
        std::vector<Candidate> candidates;
        candidates.reserve(snapshot.size());

        for (const AiCachedActor& e : snapshot)
        {
            if (!AiUsable(e.actor)) // re-validate type now; the actor may have died since the scan
                continue;
            float d = DistanceMetres(e.location, playerLoc);
            if (d <= radiusM)
                candidates.push_back({ e.actor, d });
        }

        if (requireTwo && candidates.size() < 2)
            return {};

        std::sort(candidates.begin(), candidates.end(),
            [](const Candidate& a, const Candidate& b) { return a.distanceM < b.distanceM; });

        if ((int)candidates.size() > maxTargets)
            candidates.resize((size_t)maxTargets);

        std::vector<UObject*> out;
        out.reserve(candidates.size());
        for (const Candidate& c : candidates)
            out.push_back(c.actor);
        return out;
    }

    std::vector<UObject*> CollectNearbyNonSquadAi(float radiusM, int maxTargets, bool requireTwo = false)
    {
        std::vector<UObject*> nearby = CollectNearbyAi(radiusM, maxTargets, false);
        nearby.erase(std::remove_if(nearby.begin(), nearby.end(),
            [](UObject* ai) { return IsSquadMember(ai); }), nearby.end());
        if (requireTwo && nearby.size() < 2)
            return {};
        return nearby;
    }

    bool ProcessNoParams(UObject* target, UFunction* fn)
    {
        if (!Mem::IsReadable(target, 0x30) || !Mem::IsReadable(fn, 0x30))
            return false;
        uint8_t noParams = 0;
        target->ProcessEvent(fn, &noParams);
        return true;
    }

    bool SetAiPassive(UObject* ai, bool passive)
    {
        UFunction* fn = CachedClassFn(AH::Cls_AICharacter, "SetIsPassive");
        if (!AiUsable(ai) || !fn)
            return false;
        P_BoolParam p{ passive };
        ai->ProcessEvent(fn, &p);
        return true;
    }

    UObject* GetAiController(UObject* ai)
    {
        if (!PawnUsable(ai))
            return nullptr;

        if (AiUsable(ai))
        {
            UFunction* fn = CachedClassFn(AH::Cls_AICharacter, "GetAIController");
            if (Mem::IsReadable(fn, 0x30))
            {
                P_ObjectReturn p{};
                ai->ProcessEvent(fn, &p);
                UObject* ctrl = static_cast<UObject*>(p.ReturnValue);
                if (AiControllerUsable(ctrl))
                    return ctrl;
            }
        }

        // Some scripted/quest pawns either are not AHAICharacter or return null from
        // AHAICharacter::GetAIController while APawn::Controller is still valid. Use
        // the SDK-confirmed raw field as a no-dispatch fallback, and still validate
        // it as AIModule.AIController before any nav call.
        uint8_t* base = reinterpret_cast<uint8_t*>(ai);
        if (!Mem::IsReadable(base + Offsets::O_Pawn_Controller, sizeof(void*)))
            return nullptr;
        UObject* rawCtrl = *reinterpret_cast<UObject**>(base + Offsets::O_Pawn_Controller);
        return AiControllerUsable(rawCtrl) ? rawCtrl : nullptr;
    }

    // =======================================================================
    //  AI TEAM-ID MANAGEMENT  (engine FGenericTeamId via the character functions)
    // -----------------------------------------------------------------------
    //  WHY THIS EXISTS: the fight/guard injection converts enemies onto the
    //  PLAYER's team via SwitchTeamToMatchCharacterAttitude(player, Friendly).
    //  Friendly-fire is off, so a robot put on your team becomes UNKILLABLE by
    //  you -- and if the release path doesn't undo it THROUGH THE ENGINE, it stays
    //  invincible ("godmode leak on robots"). Read/Write here go through the
    //  character's own Get/SetGenericTeamId (IGenericTeamAgentInterface), which
    //  refreshes the attitude/perception solver -- a raw controller-byte poke did
    //  NOT, which was the leak. Release additionally force-switches the unit
    //  Hostile to the player (ApplyAiRelease) as the hard killability guarantee.
    //  The split design:
    //    * Fight = split into two groups: one keeps its real robot team, the other
    //      moves to a sentinel id (kFightTeamB). The two differ (their hits land)
    //      and NEITHER is the player's team (you can still kill everyone). No robot
    //      ever joins your team.
    //    * Every team change is recorded (g_origTeam) and RESTORED on release, so
    //      nothing is ever stranded on a team that makes it invincible.
    //  All writes are 1-byte, guarded by Mem::IsReadable -> crash-safe.
    // =======================================================================
    std::unordered_map<UObject*, uint8_t> g_origTeam; // game-thread/pump only
    // Fight mode owns only these actors. Squad membership and bodyguard allegiance
    // are separate state and must survive the fight toggle's falling edge.
    std::unordered_set<UObject*> g_fightParticipants;

    // Read/write the team id through the CHARACTER's own GetGenericTeamId /
    // SetGenericTeamId (the engine IGenericTeamAgentInterface path). This refreshes
    // the attitude/perception solver, unlike a raw controller-byte poke -- which is
    // exactly why released guards used to stay friendly and unkillable ("godmode
    // leak"). ProcessEvent => game-thread only (the AI pump), guarded by AiUsable.
    bool ReadAiTeamId(UObject* ai, uint8_t& outId)
    {
        UFunction* fn = CachedFn(AH::Fn_AHBaseCharacter_GetGenericTeamId);
        if (!AiUsable(ai) || !fn) return false;
        P_GetGenericTeamId p{};
        ai->ProcessEvent(fn, &p);
        outId = p.ReturnValue;
        return true;
    }

    bool WriteAiTeamId(UObject* ai, uint8_t id)
    {
        UFunction* fn = CachedFn(AH::Fn_AHBaseCharacter_SetGenericTeamId);
        if (!AiUsable(ai) || !fn) return false;
        P_SetGenericTeamId p{ id };
        ai->ProcessEvent(fn, &p);
        return true;
    }

    // Remember an actor's real team the first time we touch it; prune dead actors
    // if the map ever gets large so it can't grow without bound.
    void RememberOrigTeam(UObject* ai, uint8_t currentId)
    {
        if (g_origTeam.count(ai))
            return;
        if (g_origTeam.size() > 512)
        {
            for (auto it = g_origTeam.begin(); it != g_origTeam.end(); )
            {
                if (AiUsable(it->first)) ++it;
                else it = g_origTeam.erase(it);
            }
        }
        g_origTeam[ai] = currentId;
    }

    // Record the original team before a NATIVE team switch (read the live byte).
    void EnsureOriginalTeamRecorded(UObject* ai)
    {
        if (!ai || g_origTeam.count(ai)) return;
        uint8_t id = 0;
        if (ReadAiTeamId(ai, id))
            RememberOrigTeam(ai, id);
    }

    // Set the team id, recording the original once (so Release can undo it). The
    // read goes through ProcessEvent now, so only do it the first time per actor.
    bool SetAiTeamIdTracked(UObject* ai, uint8_t id)
    {
        if (!g_origTeam.count(ai))
        {
            uint8_t cur = 0;
            if (ReadAiTeamId(ai, cur))
                RememberOrigTeam(ai, cur);
        }
        return WriteAiTeamId(ai, id);
    }

    // Put an actor back on the team it had before we ever touched it.
    void RestoreOriginalTeam(UObject* ai)
    {
        auto it = g_origTeam.find(ai);
        if (it == g_origTeam.end())
            return;
        if (AiUsable(ai))
            WriteAiTeamId(ai, it->second);
        g_origTeam.erase(it);
    }

    // "Fight each other" team split. Group A is LEFT on its real robot team (a
    // genuine faction the game already treats as hostile to the player), and only
    // group B is moved to this sentinel id. Result:
    //   * A vs B: different ids -> NOT same-team -> hits are not friendly-fire
    //     zeroed -> real damage (the only thing the game zeroes is same-team).
    //   * A vs you / B vs you: neither is your team, so you can still kill EVERYONE
    //     -- no robot is ever parked on your team / made invincible (the old bug).
    // Picked high so it can't collide with a real faction or your (small) team id.
    constexpr uint8_t kFightTeamB = 231;

    // Dedicated ALLY/GUARD team. A squad bodyguard is parked here while it is
    // ENGAGING a threat. WHY a distinct combat team instead of the player's own
    // team: dump evidence shows the player is team 0, and team 0 is the game's
    // DOCILE/civilian faction (animals, pedestrians, Larisa, turrets all sit on it
    // and target NOTHING). SwitchTeamToMatchCharacterAttitude(player, Friendly) put
    // guards onto team 0 -- which silently stripped their combat AI (a team-0
    // character's perception ignores the team-1 robots). kGuardTeam is a normal
    // combat id (not 0, not the robots' team), so the robots' perception reads it as
    // hostile and the guard's OWN native combat AI hunts them -- the exact mechanism
    // that makes "fight each other" work, now applied to bodyguards. Distinct from
    // kFightTeamB so guards and free brawlers are never accidentally the same side.
    constexpr uint8_t kGuardTeam = 230;

    bool ValidateTargetAllyAssignment(UObject* target)
    {
        if (!target)
            return true; // clearing the field is always safe
        if (NativeHooks::IsAHAICharacter(target))
            return true;

        g_unsafeTargetAllySkips.fetch_add(1, std::memory_order_relaxed);
        static ULONGLONG lastLogMs = 0;
        ULONGLONG now = GetTickCount64();
        if (now - lastLogMs > 1000)
        {
            lastLogMs = now;
            LOG("[AI] Skipping TargetAlly assignment: target is not AHAICharacter");
        }
        return false;
    }

    bool SetAiTargetAlly(UObject* ai, UObject* ally)
    {
        UFunction* fn = CachedClassFn(AH::Cls_AICharacter, "SetTargetAlly");
        if (!AiUsable(ai) || !fn || !ValidateTargetAllyAssignment(ally))
            return false;
        P_SetTargetAlly p{ ally };
        ai->ProcessEvent(fn, &p);
        return true;
    }

    bool SetAiTargetEnemy(UObject* ai, UObject* enemy, bool force)
    {
        UFunction* fn = CachedClassFn(AH::Cls_AICharacter, "SetTargetEnemy");
        if (!AiUsable(ai) || !fn)
            return false;
        P_SetTargetEnemy p{ enemy, true, force, {} };
        ai->ProcessEvent(fn, &p);
        return true;
    }

    bool CharacterAbilityControl(UObject* ai, const char* fnName, uint8_t ability)
    {
        UFunction* fn = CachedObjectClassFn(ai, fnName);
        if (!AiUsable(ai) || !Mem::IsReadable(fn, 0x30))
            return false;
        struct { uint8_t Ability; } p{ ability };
        ai->ProcessEvent(fn, &p);
        return true;
    }

    void HardStopHookTwinCombatAbilities(UObject* guard, const char* reason)
    {
        if (!IsHookTwinBodyguard(guard))
            return;
        // ECharacterAbilities: MeleeAttack=13, WeaponSpecialAbility=46. End first,
        // then Cancel as belt/suspenders. This is used only after target death/stale
        // combat; normal active attacks are untouched.
        CharacterAbilityControl(guard, "EndAbility", 13);
        CharacterAbilityControl(guard, "CancelAbility", 13);
        CharacterAbilityControl(guard, "EndAbility", 46);
        CharacterAbilityControl(guard, "CancelAbility", 46);
        static ULONGLONG lastLog = 0; ULONGLONG now = GetTickCount64();
        if (now - lastLog > 750)
        {
            lastLog = now;
            LOG("[AI-HOOK] hard-stopped Hook Twin combat abilities guard=%p reason=%s", (void*)guard, reason ? reason : "stand-down");
        }
    }

    bool SwitchAiTeamFriendlyTo(UObject* ai, UObject* player)
    {
        UFunction* fn = CachedClassFn(AH::Cls_AICharacter, "SwitchTeamToMatchCharacterAttitude");
        if (!AiUsable(ai) || !player || !fn)
            return false;
        EnsureOriginalTeamRecorded(ai); // so Release can undo this friendly conversion
        P_SwitchTeamToMatchCharacterAttitude p{ player, 0, {} }; // AIModule.ETeamAttitude::Friendly
        ai->ProcessEvent(fn, &p);
        return true;
    }

    bool SetControllerBool(UObject* ctrl, const char* shortName, bool value)
    {
        UFunction* fn = CachedObjectClassFn(ctrl, shortName);
        if (!AiControllerUsable(ctrl) || !Mem::IsReadable(fn, 0x30))
            return false;
        P_BoolParam p{ value };
        ctrl->ProcessEvent(fn, &p);
        return true;
    }

    bool ControllerNoParams(UObject* ctrl, const char* shortName)
    {
        UFunction* fn = CachedObjectClassFn(ctrl, shortName);
        if (!AiControllerUsable(ctrl) || !Mem::IsReadable(fn, 0x30))
            return false;
        return ProcessNoParams(ctrl, fn);
    }

    // The AHAIController's SetBlackboard* setters write into its UBlackboardComponent.
    // If that component pointer is null/stale, the native setter faults INSIDE game
    // code where UE4's own SEH beats our catch(...) -> hard crash (this is the exact
    // class of controller call the old code blamed for the "Fight each other" freeze).
    // Gate every blackboard write on a readable Blackboard pointer so it's crash-safe.
    bool ControllerBlackboardReady(UObject* ctrl)
    {
        if (!AiControllerUsable(ctrl))
            return false;
        uint8_t* base = reinterpret_cast<uint8_t*>(ctrl);
        if (!Mem::IsReadable(base + AH::AICtrl_Blackboard, sizeof(void*)))
            return false;
        UObject* bb = *reinterpret_cast<UObject**>(base + AH::AICtrl_Blackboard);
        return Mem::IsReadable(bb, 0x30);
    }

    bool SetControllerFollowLocation(UObject* ctrl, const FVector& loc)
    {
        UFunction* fn = CachedObjectClassFn(ctrl, "SetBlackboardFollowLocation");
        if (!ControllerBlackboardReady(ctrl) || !fn)
            return false;
        P_SetBlackboardFollowLocation p{ loc };
        ctrl->ProcessEvent(fn, &p);
        return true;
    }

    bool SetControllerTargetAlly(UObject* ctrl, UObject* ally)
    {
        UFunction* fn = CachedObjectClassFn(ctrl, "SetBlackboardTargetAlly");
        if (!ControllerBlackboardReady(ctrl) || !fn || !ValidateTargetAllyAssignment(ally))
            return false;
        P_SetBlackboardTargetAlly p{ ally };
        ctrl->ProcessEvent(fn, &p);
        return true;
    }

    bool SetControllerTargetEnemy(UObject* ctrl, UObject* enemy, bool force)
    {
        UFunction* fn = CachedObjectClassFn(ctrl, "SetBlackboardTargetEnemy");
        if (!ControllerBlackboardReady(ctrl) || !fn)
            return false;
        P_SetBlackboardTargetEnemy p{ enemy, force, {} };
        ctrl->ProcessEvent(fn, &p);
        return true;
    }

    // Flip the IsAggressive blackboard key. THIS is what moves the AI's behaviour
    // tree out of "approach/investigate" and into the attack branch -> the attack
    // montage actually plays. Setting only the target enemy makes a guard walk to
    // the threat but never swing; this is the missing piece.
    bool SetControllerAggressive(UObject* ctrl, bool aggressive)
    {
        UFunction* fn = CachedObjectClassFn(ctrl, "SetBlackboardIsAggressive");
        if (!ControllerBlackboardReady(ctrl) || !fn)
            return false;
        P_BoolParam p{ aggressive };
        ctrl->ProcessEvent(fn, &p);
        return true;
    }

    bool SetControllerFollowSpeed(UObject* ctrl)
    {
        UFunction* fn = CachedObjectClassFn(ctrl, "SetFollowLocationSpeed");
        if (!ControllerBlackboardReady(ctrl) || !fn)
            return false;
        // Lower speed/rotation keeps Twins in their native walk-cycle range instead
        // of the stiff Recast jog/snap-turn look. Combat paths override max speed.
        P_SetFollowLocationSpeed p{ 260.0f, 1.25f, { 0.0f, 240.0f, 0.0f } };
        ctrl->ProcessEvent(fn, &p);
        return true;
    }

    // Set the AHAIController "ForceFollowLocation" BOOL blackboard key. THIS is the
    // missing piece for companion NPCs (Larisa): "FollowLocation" alone is ignored
    // while the NPC is on its idle/wait schedule (-> it only TURNS to face you,
    // "turns to me but doesn't walk"); "ForceFollowLocation"=true overrides the
    // schedule so the follow MoveTo task actually runs. No AHAIController wrapper
    // exists for this key, so we set it on the BlackboardComponent directly, reading
    // the exact key NAME from the controller's ForceFollowLocationKeyName field (no
    // guessed string). Fully crash-safe: blackboard-gated, no native nav call.
    bool SetControllerForceFollow(UObject* ctrl, bool value)
    {
        if (!ControllerBlackboardReady(ctrl))
            return false;
        uint8_t* base = reinterpret_cast<uint8_t*>(ctrl);
        UObject* bb = *reinterpret_cast<UObject**>(base + AH::AICtrl_Blackboard);
        if (!Mem::IsReadable(bb, 0x30) || !Mem::IsReadable(base + AH::AICtrl_Key_ForceFollowLoc, sizeof(FName)))
            return false;
        FName key = *reinterpret_cast<FName*>(base + AH::AICtrl_Key_ForceFollowLoc);
        if (key.ComparisonIndex <= 0)
            return false; // key name unset on this controller -> nothing to force
        UFunction* fn = CachedObjectClassFn(bb, "SetValueAsBool");
        if (!fn)
            return false;
        struct { FName KeyName; bool Value; uint8_t pad[3]; } p{ key, value, { 0, 0, 0 } };
        bb->ProcessEvent(fn, &p);
        return true;
    }

    // Generalised key writers: set ANY of the AHAIController's blackboard keys by the
    // OFFSET of its key-name field (offsets.h AICtrl_Key_*). This is how we "trick" a
    // companion NPC's own behaviour tree into walking to us -- we overwrite the exact
    // destination/flag keys its move task reads, so it walks there with REAL locomotion.
    bool SetControllerBoolKeyAt(UObject* ctrl, int keyNameOffset, bool value)
    {
        if (!ControllerBlackboardReady(ctrl)) return false;
        uint8_t* base = reinterpret_cast<uint8_t*>(ctrl);
        UObject* bb = *reinterpret_cast<UObject**>(base + AH::AICtrl_Blackboard);
        if (!Mem::IsReadable(bb, 0x30) || !Mem::IsReadable(base + keyNameOffset, sizeof(FName))) return false;
        FName key = *reinterpret_cast<FName*>(base + keyNameOffset);
        if (key.ComparisonIndex <= 0) return false;
        UFunction* fn = CachedObjectClassFn(bb, "SetValueAsBool");
        if (!fn) return false;
        struct { FName KeyName; bool Value; uint8_t pad[3]; } p{ key, value, { 0, 0, 0 } };
        bb->ProcessEvent(fn, &p);
        return true;
    }

    bool SetControllerVectorKeyAt(UObject* ctrl, int keyNameOffset, const FVector& v)
    {
        if (!ControllerBlackboardReady(ctrl)) return false;
        uint8_t* base = reinterpret_cast<uint8_t*>(ctrl);
        UObject* bb = *reinterpret_cast<UObject**>(base + AH::AICtrl_Blackboard);
        if (!Mem::IsReadable(bb, 0x30) || !Mem::IsReadable(base + keyNameOffset, sizeof(FName))) return false;
        FName key = *reinterpret_cast<FName*>(base + keyNameOffset);
        if (key.ComparisonIndex <= 0) return false;
        UFunction* fn = CachedObjectClassFn(bb, "SetValueAsVector");
        if (!fn) return false;
        struct { FName KeyName; FVector Value; } p{ key, v };
        bb->ProcessEvent(fn, &p);
        return true;
    }

    bool SetControllerFloatKeyAt(UObject* ctrl, int keyNameOffset, float value)
    {
        if (!ControllerBlackboardReady(ctrl)) return false;
        uint8_t* base = reinterpret_cast<uint8_t*>(ctrl);
        UObject* bb = *reinterpret_cast<UObject**>(base + AH::AICtrl_Blackboard);
        if (!Mem::IsReadable(bb, 0x30) || !Mem::IsReadable(base + keyNameOffset, sizeof(FName))) return false;
        FName key = *reinterpret_cast<FName*>(base + keyNameOffset);
        if (key.ComparisonIndex <= 0) return false;
        UFunction* fn = CachedObjectClassFn(bb, "SetValueAsFloat");
        if (!fn) return false;
        struct { FName KeyName; float Value; } p{ key, value };
        bb->ProcessEvent(fn, &p);
        return true;
    }

    bool SetControllerObjectKeyAt(UObject* ctrl, int keyNameOffset, UObject* value)
    {
        if (!ControllerBlackboardReady(ctrl)) return false;
        uint8_t* base = reinterpret_cast<uint8_t*>(ctrl);
        UObject* bb = *reinterpret_cast<UObject**>(base + AH::AICtrl_Blackboard);
        if (!Mem::IsReadable(bb, 0x30) || !Mem::IsReadable(base + keyNameOffset, sizeof(FName))) return false;
        FName key = *reinterpret_cast<FName*>(base + keyNameOffset);
        if (key.ComparisonIndex <= 0) return false;
        UFunction* fn = CachedObjectClassFn(bb, "SetValueAsObject");
        if (!fn) return false;
        struct { FName KeyName; void* ObjectValue; } p{ key, value };
        bb->ProcessEvent(fn, &p);
        return true;
    }

    bool ReadControllerBoolKeyAt(UObject* ctrl, int keyNameOffset, bool& out)
    {
        if (!ControllerBlackboardReady(ctrl)) return false;
        uint8_t* base = reinterpret_cast<uint8_t*>(ctrl);
        UObject* bb = *reinterpret_cast<UObject**>(base + AH::AICtrl_Blackboard);
        if (!Mem::IsReadable(bb, 0x30) || !Mem::IsReadable(base + keyNameOffset, sizeof(FName))) return false;
        FName key = *reinterpret_cast<FName*>(base + keyNameOffset);
        if (key.ComparisonIndex <= 0) return false;
        UFunction* fn = CachedObjectClassFn(bb, "GetValueAsBool");
        if (!fn) return false;
        struct { FName KeyName; bool Value; uint8_t pad[3]; } p{ key, false, {0,0,0} };
        bb->ProcessEvent(fn, &p);
        out = p.Value;
        return true;
    }
    // Issue the reflected AIController::MoveToActor request used by Hook followers.
    // Twins require pathfinding so the generic controller route builder creates an
    // active Recast path before their mixed-navigation override post-processes it.
    // ControllerPathFollowingReady validates the controller/pawn ownership chain first.
    bool MoveControllerToActor(UObject* ctrl, UObject* controlledPawn, UObject* goal,
                               float acceptanceRadius, bool usePathfinding = false)
    {
        if (!ControllerPathFollowingReady(ctrl, controlledPawn) || !Mem::IsReadable(goal, 0x30))
            return false;
        UFunction* fn = CachedFn(AH::Fn_AIController_MoveToActor);
        if (!Mem::IsReadable(fn, 0x30))
            fn = CachedObjectClassFn(ctrl, "MoveToActor");
        if (!Mem::IsReadable(fn, 0x30))
            return false;
        P_MoveToActor p{};
        p.Goal = goal;
        p.AcceptanceRadius = acceptanceRadius; // stop this far away -> no orbiting at point-blank
        p.bStopOnOverlap = true;
        p.bUsePathfinding = usePathfinding;
        p.bCanStrafe = false;
        p.FilterClass = nullptr;
        p.bAllowPartialPath = true;
        t_hookMovementInternal = true;
        AiMovementHooks::SetControllerCallInternal(true);
        try { ctrl->ProcessEvent(fn, &p); }
        catch (...) { AiMovementHooks::SetControllerCallInternal(false); t_hookMovementInternal = false; throw; }
        AiMovementHooks::SetControllerCallInternal(false);
        t_hookMovementInternal = false;
        return p.ReturnValue != 0; // Failed=0, AlreadyAtGoal=1, RequestSuccessful=2 (SDK)
    }

    [[maybe_unused]] bool StopHookControllerMovement(UObject* ctrl)
    {
        UFunction* fn = CachedObjectClassFn(ctrl, "StopMovement");
        if (!AiControllerUsable(ctrl) || !Mem::IsReadable(fn, 0x30)) return false;
        bool ok = false;
        try
        {
            t_hookMovementInternal = true;
            AiMovementHooks::SetControllerCallInternal(true);
            ok = ProcessNoParams(ctrl, fn);
            AiMovementHooks::SetControllerCallInternal(false);
            t_hookMovementInternal = false;
        }
        catch (...) { AiMovementHooks::SetControllerCallInternal(false); t_hookMovementInternal = false; ok = false; }
        return ok;
    }

    uint8_t DirectControllerMoveStatus(UObject* ctrl)
    {
        uint8_t* base = reinterpret_cast<uint8_t*>(ctrl);
        // ReVa: AAIController::GetMoveStatus reads PathFollowingComponent +0x1D8.
        if (!Mem::IsReadable(base + 0x2F8, sizeof(void*))) return 0xFF;
        uint8_t* path = *reinterpret_cast<uint8_t**>(base + 0x2F8);
        return Mem::IsReadable(path + 0x1D8, 1) ? path[0x1D8] : 0xFF;
    }

    // Poll the controller's path-following status (EPathFollowingStatus: 0 Idle,
    // 1 Waiting, 2 Paused, 3 Moving; 0xFF = couldn't read). The game itself polls this
    // (the Larisa trace showed GetMoveStatus called every move) and only (re)issues a
    // move when NOT already Moving -- re-issuing while a move is running is exactly the
    // "takes one step then halts" restart-stutter. GetMoveStatus is on the parent
    // AAIController, and CachedObjectClassFn walks the SuperStruct chain to find it.
    uint8_t ControllerMoveStatus(UObject* ctrl)
    {
        UFunction* fn = CachedObjectClassFn(ctrl, "GetMoveStatus");
        if (!AiControllerUsable(ctrl) || !Mem::IsReadable(fn, 0x30))
            return 0xFF;
        struct { uint8_t Ret; } p{ 0xFF };
        ctrl->ProcessEvent(fn, &p);
        return p.Ret;
    }

    bool FocusControllerOnActor(UObject* ctrl, UObject* goal)
    {
        UFunction* fn = CachedObjectClassFn(ctrl, "K2_SetFocus");
        if (!ctrl || !goal || !fn)
            return false;
        P_SetTargetAlly p{ goal };
        ctrl->ProcessEvent(fn, &p);
        return true;
    }

    // Drop the controller's focus actor so the pawn faces its TRAVEL direction again
    // (its AnimBP orients to velocity) instead of being pinned to stare at a focus
    // target. Used by the Twin follow so a moving Twin turns naturally like her native
    // locomotion, then re-focuses you when she stops. No-op + crash-safe if unresolved.
    bool ClearControllerFocus(UObject* ctrl)
    {
        UFunction* fn = CachedObjectClassFn(ctrl, "K2_ClearFocus");
        if (!ctrl || !fn)
            return false;
        return ProcessNoParams(ctrl, fn);
    }

    bool SetCharacterHealthZero(UObject* ai)
    {
        uint8_t* base = reinterpret_cast<uint8_t*>(ai);
        if (!Mem::IsReadable(base + AH::Char_AttributeSet, sizeof(void*)))
            return false;
        uint8_t* set = *reinterpret_cast<uint8_t**>(base + AH::Char_AttributeSet);
        if (!Mem::IsReadable(set + AH::Set_Health + AH::Attr_CurrentValue, sizeof(float)))
            return false;
        *reinterpret_cast<float*>(set + AH::Set_Health + AH::Attr_BaseValue) = 0.0f;
        *reinterpret_cast<float*>(set + AH::Set_Health + AH::Attr_CurrentValue) = 0.0f;
        return true;
    }

    // Top an ally's health to its max -- a single guarded write (no ProcessEvent),
    // so it's crash-safe and needs no restore bookkeeping. Re-asserted each pump,
    // this keeps your bodyguards/spawns ALIVE (they used to die in seconds, which
    // made the whole squad feel useless). Toggle off => simply stop topping them.
    bool SetCharacterHealthFull(UObject* ai)
    {
        uint8_t* base = reinterpret_cast<uint8_t*>(ai);
        if (!Mem::IsReadable(base + AH::Char_AttributeSet, sizeof(void*)))
            return false;
        uint8_t* set = *reinterpret_cast<uint8_t**>(base + AH::Char_AttributeSet);
        if (!Mem::IsReadable(set + AH::Set_MaxHealth + AH::Attr_CurrentValue, sizeof(float)))
            return false;
        float mx = *reinterpret_cast<float*>(set + AH::Set_MaxHealth + AH::Attr_CurrentValue);
        if (!(mx > 0.0f) || !std::isfinite(mx))
            return false;
        *reinterpret_cast<float*>(set + AH::Set_Health + AH::Attr_BaseValue)    = mx;
        *reinterpret_cast<float*>(set + AH::Set_Health + AH::Attr_CurrentValue) = mx;
        return true;
    }

    // Scale the damage a character INSTIGATES (deals). Single guarded float write (no
    // ProcessEvent, crash-safe). Re-asserted each pump on combat-capable squad members
    // so your bodyguards obliterate enemies -- same attribute the player one-hit uses.
    bool SetCharacterInstigatedDamage(UObject* ai, float mult)
    {
        uint8_t* base = reinterpret_cast<uint8_t*>(ai);
        if (!Mem::IsReadable(base + AH::Char_AttributeSet, sizeof(void*)))
            return false;
        uint8_t* set = *reinterpret_cast<uint8_t**>(base + AH::Char_AttributeSet);
        if (!Mem::IsReadable(set + AH::Set_InstigatedDmgMult + AH::Attr_CurrentValue, sizeof(float)))
            return false;
        *reinterpret_cast<float*>(set + AH::Set_InstigatedDmgMult + AH::Attr_BaseValue)    = mult;
        *reinterpret_cast<float*>(set + AH::Set_InstigatedDmgMult + AH::Attr_CurrentValue) = mult;
        return true;
    }

    // Stop an actor by scaling its own time dilation to ~0. This is a single
    // guarded float write -- no ProcessEvent -- so it cannot dispatch into game
    // code and cannot crash even if the actor pointer just went stale.
    bool SetActorTimeDilation(UObject* actor, float value)
    {
        uint8_t* base = reinterpret_cast<uint8_t*>(actor);
        if (!Mem::IsReadable(base + Offsets::O_Actor_CustomTimeDilation, sizeof(float)))
            return false;
        *reinterpret_cast<float*>(base + Offsets::O_Actor_CustomTimeDilation) = value;
        return true;
    }

    void EnsureFollowerCanMove(UObject* pawn)
    {
        if (!FollowPawnUsable(pawn))
            return;

        // Actor-level tick/time controls are generic AActor state and are safe for
        // quest pawns. This prevents a spawned follower from remaining frozen by a
        // previous menu operation or significance state.
        SetActorTimeDilation(pawn, 1.0f);
        if (UFunction* fn = CachedFn(AH::Fn_SetActorTickEnabled))
        {
            if (Mem::IsReadable(fn, 0x30))
            {
                P_SetActorTickEnabled p{ true };
                pawn->ProcessEvent(fn, &p);
            }
        }

        // CharacterMovement is SDK-confirmed on ACharacter at 0x2A8. Only touch it
        // after an Engine.Character IsA check; plain APawn subclasses do not have it.
        if (CharacterUsable(pawn))
        {
            uint8_t* b = reinterpret_cast<uint8_t*>(pawn);
            if (Mem::IsReadable(b + AH::Char_CharacterMovement, sizeof(void*)))
            {
                uint8_t* mv = *reinterpret_cast<uint8_t**>(b + AH::Char_CharacterMovement);
                if (Mem::IsReadable(mv + AH::Move_MaxWalkSpeed, sizeof(float)))
                {
                    // Followers walk via per-frame RequestDirectMove, capped at MaxWalkSpeed,
                    // so give them a brisk floor or they can't keep up when you jog/sprint.
                    float& speed = *reinterpret_cast<float*>(mv + AH::Move_MaxWalkSpeed);
                    if (!std::isfinite(speed) || speed < 600.0f)
                        speed = 600.0f;
                }
            }
        }

        if (AiUsable(pawn))
        {
            uint8_t* b = reinterpret_cast<uint8_t*>(pawn);
            if (Mem::IsReadable(b + AH::AICh_bActorTickEnabled, 1))
                *reinterpret_cast<bool*>(b + AH::AICh_bActorTickEnabled) = true;
            if (Mem::IsReadable(b + AH::AICh_bMeshTickEnabled, 1))
                *reinterpret_cast<bool*>(b + AH::AICh_bMeshTickEnabled) = true;
        }
    }

    // =======================================================================
    //  DEEP AGGRO INJECTION  --  make enemies *truly* fight / guard
    // -----------------------------------------------------------------------
    //  Atomic Heart never spawns enemy-vs-enemy combat, so a one-shot "set
    //  target" is reverted by the AI's own perception within a frame or two.
    //  The pipeline below covers every layer at once, re-asserted continuously:
    //    1. raw write of the cached aggro target field   (the data the AI reads)
    //    2. raw write of the passive/always-aggressive gates
    //    3. the game's own SetTargetEnemy(force)          (routes to blackboard)
    //    4. team split via the game's SwitchTeam...Hostile (so hits actually do
    //       damage -- bypasses friendly-fire zeroing without touching damage)
    //    5. UAIUtils.SetCharacterAggressive               (native attack-state
    //       machine driver: the "force the attack transition" step)
    //  Raw writes are guarded by Mem::IsReadable (cannot crash on a stale actor);
    //  the ProcessEvent calls are all the game's OWN setters (which the engine
    //  itself calls), guarded by AiUsable -- we deliberately avoid the raw
    //  AIModule nav / behaviour-tree start/stop calls that crashed earlier.
    // =======================================================================

    // Raw write of AAHAICharacter::CachedTargetEnemy -- the field the engine
    // reads to know who this AI is fighting. No ProcessEvent, so crash-safe.
    bool WriteAiTargetField(UObject* ai, UObject* enemy)
    {
        uint8_t* base = reinterpret_cast<uint8_t*>(ai);
        if (!Mem::IsReadable(base + AH::AICh_CachedTargetEnemy, sizeof(void*)))
            return false;
        *reinterpret_cast<void**>(base + AH::AICh_CachedTargetEnemy) = enemy;
        return true;
    }

    // Flip the character-level gates that decide "will I attack at all". Raw.
    void WriteAiAggressiveFlags(UObject* ai, bool aggressive)
    {
        uint8_t* base = reinterpret_cast<uint8_t*>(ai);
        if (Mem::IsReadable(base + AH::AICh_bIsPassive, 1))
            *reinterpret_cast<bool*>(base + AH::AICh_bIsPassive) = !aggressive;
        if (Mem::IsReadable(base + AH::AICh_bPassiveButWithSenses, 1))
            *reinterpret_cast<bool*>(base + AH::AICh_bPassiveButWithSenses) = false;
        if (aggressive && Mem::IsReadable(base + AH::AICh_bIsAlwaysAggressive, 1))
            *reinterpret_cast<bool*>(base + AH::AICh_bIsAlwaysAggressive) = true;
    }

    // Hook Debug idle state: keep the pawn awake and perceptive without leaving the
    // game's "always aggressive" latch set. That latch was the reason a managed guard
    // immediately re-entered combat against distant robots after we cleared its target.
    void ClearAiAggressiveLatch(UObject* ai)
    {
        uint8_t* base = reinterpret_cast<uint8_t*>(ai);
        if (Mem::IsReadable(base + AH::AICh_bIsPassive, 1))
            *reinterpret_cast<bool*>(base + AH::AICh_bIsPassive) = false;
        if (Mem::IsReadable(base + AH::AICh_bPassiveButWithSenses, 1))
            *reinterpret_cast<bool*>(base + AH::AICh_bPassiveButWithSenses) = false;
        if (Mem::IsReadable(base + AH::AICh_bIsAlwaysAggressive, 1))
            *reinterpret_cast<bool*>(base + AH::AICh_bIsAlwaysAggressive) = false;
    }

    UObject* ReadAiTargetField(UObject* ai)
    {
        uint8_t* base = reinterpret_cast<uint8_t*>(ai);
        if (!Mem::IsReadable(base + AH::AICh_CachedTargetEnemy, sizeof(void*)))
            return nullptr;
        return *reinterpret_cast<UObject**>(base + AH::AICh_CachedTargetEnemy);
    }

    void MarkAiDeathTombstone(UObject* actor, const char* reason)
    {
        if (!actor || !Mem::IsReadable(actor, 0x30) || IsHookBodyguard(actor))
            return;
        ULONGLONG now = GetTickCount64();
        bool first = (g_aiDeathEventMs.find(actor) == g_aiDeathEventMs.end());
        g_aiDeathEventMs[actor] = now;
        if (first)
            LOG("[AI-DEATH] tombstone actor=%p reason=%s", (void*)actor, reason ? reason : "unknown");
        if (g_aiDeathEventMs.size() > 512)
        {
            for (auto it = g_aiDeathEventMs.begin(); it != g_aiDeathEventMs.end(); )
                if (!Mem::IsReadable(it->first, 0x30) || now - it->second > 10 * 60 * 1000ULL) it = g_aiDeathEventMs.erase(it);
                else ++it;
        }
    }

    bool IsLiveCombatTarget(UObject* target)
    {
        if (!target || !AiUsable(target))
            return false;
        auto dit = g_aiDeathEventMs.find(target);
        if (dit != g_aiDeathEventMs.end())
        {
            // Tombstone is authoritative for this session. Do not let a stale corpse
            // with readable/nonzero health keep a Twin in combat forever.
            return false;
        }
        uint8_t* base = reinterpret_cast<uint8_t*>(target);
        if (Mem::IsReadable(base + AH::Char_bIsDead, 1) && *reinterpret_cast<bool*>(base + AH::Char_bIsDead))
        {
            MarkAiDeathTombstone(target, "Char_bIsDead");
            return false;
        }
        float cur = 0.0f, mx = 0.0f;
        if (ReadCharacterHealth(target, cur, mx) && mx > 0.001f)
        {
            bool alive = std::isfinite(cur) && cur > 0.001f;
            if (!alive)
                MarkAiDeathTombstone(target, "health<=0");
            return alive;
        }
        // Some special combat actors expose no normal health attribute. Keep them
        // eligible only while the object is still a valid, not-dead AHAICharacter.
        return true;
    }

    void ResolveHookTwinDeathPipelineFns()
    {
        auto cache = [](std::atomic<void*>& slot, const char* fullName)
        {
            if (!slot.load(std::memory_order_relaxed))
                slot.store(CachedFn(fullName), std::memory_order_relaxed);
        };
        cache(g_fnHookK2OnDeath, AH::Fn_AHAICharacter_K2_OnDeath);
        cache(g_fnHookK2OnLoadDeathState, AH::Fn_AHAICharacter_K2_OnLoadDeathState);
        cache(g_fnHookLoadDeadState, AH::Fn_AHAICharacter_LoadDeadState);
        // Keep the reflected UFunction separate from the native helper RVA. The old
        // code overwrote this with moduleBase+0x1B93A50, so ProcessEvent comparisons
        // against AHAICharacter.TryActivateFightStagingAbility could never match.
        cache(g_fnHookTryFightStaging, AH::Fn_AHAICharacter_TryActivateFightStagingAbility);
        // Ghidra-verified RVA 0x1B93A50: 3-arg native fight-staging selection writer
        // void(UAIFightStagingAbility*, void* selectedObj, bool), not the reflected
        // AHAICharacter::TryActivateFightStagingAbility bool/no-arg UFunction.
        if (G::moduleBase)
            g_fnHookTryFightStagingNative.store(
                reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(G::moduleBase) + 0x1B93A50),
                std::memory_order_relaxed);
        cache(g_fnHookSendDeathEvent, AH::Fn_AHCharacterEventUtils_SendDeathEvent);
        cache(g_fnHookSendLoadDeathStateEvent, AH::Fn_AHCharacterEventUtils_SendLoadDeathStateEvent);
        cache(g_fnHookSendCharacterDiedEvent, AH::Fn_GameplayEventSubsystem_SendCharacterDiedEvent);
        cache(g_fnHookStartVersusQTE, AH::Fn_QTESubsystem_StartVersusQTE);
        cache(g_fnHookCacheDeathPose, AH::Fn_QTESubsystem_CacheDeathPose);
        cache(g_fnHookDestroyOwnerCharacter, AH::Fn_AIDeathAbility_DestroyOwnerCharacter);
    }

    // Native detour for TryActivateFightStagingAbility.
    // Ghidra reversal at RVA 0x1B93A50: void(UAIFightStagingAbility* ability, void* params, bool flag).
    // param_1 = UAIFightStagingAbility* (not AHAICharacter*).
    // Character (CachedAIOwner) is at ability + 0x658 (AH::AIAbility_CachedAIOwner).
    // param_2 = the new selected object being set (passed to FUN_14277c040 as lookup key).
    // param_1 + 0x690 is READ (current selection) then WRITTEN (new selection) by the native.
    // Logs every call and blocks only if the selected fight-staging object matches a
    // downed/final/QTE token. Normal movement/combat stages pass through.
    void __fastcall hkTryFightStaging(void* ability, void* params, bool param_3)
    {
        // param_1 is UAIFightStagingAbility*, not AHAICharacter*.
        // Get the character from CachedAIOwner at offset +0x658.
        UObject* ch = HookTwinOwnerFromDeathAbility(ability);
        std::string chName = ch && ch->Class() ? ch->Class()->GetName() : "(null)";

        // param_2 is the new selected object being set (verified via FUN_14277c040 call).
        // Do NOT read from ability + 0x690 — that is the write destination, not the input.
        void* selObj = params;
        UObject* selUObj = reinterpret_cast<UObject*>(selObj);
        std::string selName = selUObj && selUObj->Class() ? selUObj->Class()->GetName() : "(none)";

        // Check the CHARACTER (CachedAIOwner), not the ability itself.
        bool isTwin = ch && IsHookTwinBodyguard(ch);
        bool block = false;

        if (isTwin)
        {
            block = MatchesDownedToken(selName);
        }

        if (isTwin)
        {
            g_hookTwinFightStagingSelectorHookLive.store(true, std::memory_order_relaxed);
            g_hookTwinFightStagingSelections.fetch_add(1, std::memory_order_relaxed);
        }

        LOG("[AI-FSTAGE] TryActivateFightStagingAbility ability=%p char=%p charClass=%s selObj=%p selClass=%s block=%d", 
            ability, ch, chName.c_str(), selObj, selName.c_str(), block ? 1 : 0);

        if (block)
        {
            g_hookTwinFightStagingBlocks.fetch_add(1, std::memory_order_relaxed);
            LOG("[AI-FSTAGE] BLOCKED Hook Twin campaign/downed fight-stage select: %s -> %s", chName.c_str(), selName.c_str());
            // Do NOT call the trampoline -- the native function at RVA 0x1B93A50
            // writes the new selection to ability + 0x690 and sets flags at +0x6A0/+0x6A1.
            // Returning early prevents that write, so the downed pose logic never executes.
            return;
        }

        g_oTryFightStaging(ability, params, param_3);
    }

    // Action-container factory detour (RVA 0x1CA06E0). Logs containers spawned
    // after fight-staging selection on Hook Twins.
    void* __fastcall hkActionContainerFactory(void* obj, void* params)
    {
        void* container = g_oActionContainerFactory(obj, params);
        if (container && IsHookTwinBodyguard(reinterpret_cast<UObject*>(obj)))
        {
            g_hookTwinActionContainerHookLive.store(true, std::memory_order_relaxed);
            g_hookTwinFightStagingContainerLogs.fetch_add(1, std::memory_order_relaxed);
            UObject* cont = reinterpret_cast<UObject*>(container);
            std::string contName = cont && cont->Class() ? cont->Class()->GetName() : "(none)";
            std::string objName = reinterpret_cast<UObject*>(obj) && 
                                  reinterpret_cast<UObject*>(obj)->Class() ? 
                                  reinterpret_cast<UObject*>(obj)->Class()->GetName() : "(none)";
            LOG("[AI-FSTAGE] Hook Twin action-container create obj=%p objClass=%s container=%p containerClass=%s",
                obj, objName.c_str(), container, contName.c_str());
        }
        return container;
    }

    // Ensure a MinHook detour is active on AHAICharacter::TryActivateFightStagingAbility.
    // Idempotent: if the hook is already live, returns true immediately.
    bool EnsureHookTwinFightStagingNativeHook()
    {
        void* target = g_fnHookTryFightStagingNative.load(std::memory_order_relaxed);
        if (!target && G::moduleBase)
        {
            target = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(G::moduleBase) + 0x1B93A50);
            g_fnHookTryFightStagingNative.store(target, std::memory_order_relaxed);
        }
        if (!target || !Scanner::IsExecutableAddress(target, 8))
        {
            bool already = g_hookTwinFightStagingHookAttempted.exchange(true, std::memory_order_relaxed);
            if (!already)
                LOG_HOOK("TryActivateFightStagingAbility native hook unavailable target=%p moduleBase=%p", target, (void*)G::moduleBase);
            return false;
        }

        // Already hooked.
        if (g_hookTwinFightStagingSelectorHookLive.load(std::memory_order_relaxed))
            return true;

        MH_STATUS status = MH_CreateHook(target,
            reinterpret_cast<void*>(&hkTryFightStaging),
            reinterpret_cast<void**>(&g_oTryFightStaging));
        if (status != MH_OK && status != MH_ERROR_ALREADY_CREATED)
        {
            LOG_HOOK("TryActivateFightStagingAbility native hook: MH_CreateHook failed %d", status);
            return false;
        }

        status = MH_EnableHook(target);
        if (status != MH_OK && status != MH_ERROR_ENABLED)
        {
            LOG_HOOK("TryActivateFightStagingAbility native hook: MH_EnableHook failed %d", status);
            return false;
        }

        g_hookTwinFightStagingSelectorHookLive.store(true, std::memory_order_relaxed);
        LOG_HOOK("TryActivateFightStagingAbility native hook LIVE @ %p", target);
        return true;
    }

    // Ensure a MinHook detour is active on the action-container factory (RVA 0x1CA06E0).
    // The game module base is looked up from G::moduleBase; RVA 0x1CA06E0 is the
    // factory that spawns AHActionContainer instances after fight-staging selection.
    bool EnsureHookTwinActionContainerFactory()
    {
        if (g_hookTwinActionContainerHookLive.load(std::memory_order_relaxed))
            return true;
        if (!G::moduleBase)
            return false;

        void* target = reinterpret_cast<void*>(
            reinterpret_cast<uintptr_t>(G::moduleBase) + 0x1CA06E0);
        if (!Scanner::IsExecutableAddress(target, 8))
            return false;

        MH_STATUS status = MH_CreateHook(target,
            reinterpret_cast<void*>(&hkActionContainerFactory),
            reinterpret_cast<void**>(&g_oActionContainerFactory));
        if (status != MH_OK && status != MH_ERROR_ALREADY_CREATED)
        {
            LOG_HOOK("ActionContainerFactory native hook: MH_CreateHook failed %d", status);
            return false;
        }

        status = MH_EnableHook(target);
        if (status != MH_OK && status != MH_ERROR_ENABLED)
        {
            LOG_HOOK("ActionContainerFactory native hook: MH_EnableHook failed %d", status);
            return false;
        }

        g_hookTwinActionContainerHookLive.store(true, std::memory_order_relaxed);
        LOG_HOOK("ActionContainerFactory native hook LIVE @ %p", target);
        return true;
    }

    bool IsHookTwinBodyguard(UObject* ai)
    {
        return Mem::IsReadable(ai, 0x30) && IsHookBodyguard(ai) && IsMixedNavCharacter(ai);
    }

    UObject* HookTwinOwnerFromDeathAbility(void* ability)
    {
        uint8_t* base = reinterpret_cast<uint8_t*>(ability);
        if (!Mem::IsReadable(base + AH::AIAbility_CachedAIOwner, sizeof(void*)))
            return nullptr;
        return *reinterpret_cast<UObject**>(base + AH::AIAbility_CachedAIOwner);
    }

    void RestoreHookTwinAfterDeathPipelineBlock(UObject* guard)
    {
        uint8_t* base = reinterpret_cast<uint8_t*>(guard);
        if (!Mem::IsReadable(base, 0x30))
            return;
        if (Mem::IsReadable(base + AH::Char_bIsDead, 1))
            *reinterpret_cast<bool*>(base + AH::Char_bIsDead) = false;
        SetCharacterHealthFull(guard);
        if (Mem::IsReadable(base + AH::AICh_bActorTickEnabled, 1))
            *reinterpret_cast<bool*>(base + AH::AICh_bActorTickEnabled) = true;
        if (Mem::IsReadable(base + AH::AICh_bMeshTickEnabled, 1))
            *reinterpret_cast<bool*>(base + AH::AICh_bMeshTickEnabled) = true;
    }

    void ArmHookTwinDeathPoseCacheGuard(UObject* guard, ULONGLONG now)
    {
        g_hookTwinDeathPoseBlockUntilMs.store((unsigned long long)(now + 5000), std::memory_order_relaxed);
        if (guard)
            g_hookTwinDeathPoseGuard.store(guard, std::memory_order_relaxed);
    }

    bool HookTwinArrayContainsManagedBodyguard(const TArray<void*>& arr, UObject*& guard)
    {
        if (!arr.Data || arr.Count <= 0 || arr.Count > 32)
            return false;
        if (!Mem::IsReadable(arr.Data, sizeof(void*) * (size_t)arr.Count))
            return false;
        for (int32_t i = 0; i < arr.Count; ++i)
        {
            UObject* candidate = reinterpret_cast<UObject*>(arr.Data[i]);
            if (IsHookTwinBodyguard(candidate))
            {
                guard = candidate;
                return true;
            }
        }
        return false;
    }

    void LogHookTwinDeathBlock(const char* stage, UObject* guard, void* obj, void* fn, bool qte, uint64_t total)
    {
        static ULONGLONG lastLogMs = 0;
        ULONGLONG now = GetTickCount64();
        if (total > 12 && now - lastLogMs < 750)
            return;
        lastLogMs = now;

        float cur = -1.0f, mx = -1.0f;
        bool hasHealth = guard && ReadCharacterHealth(guard, cur, mx);
        int dead = -1;
        uint8_t* base = reinterpret_cast<uint8_t*>(guard);
        if (Mem::IsReadable(base + AH::Char_bIsDead, 1))
            dead = *reinterpret_cast<bool*>(base + AH::Char_bIsDead) ? 1 : 0;

        if (qte)
        {
            LOG("[AI-DEATH] blocked Hook Twin QTE/death-pose stage=%s guard=%p obj=%p fn=%p total=%llu health=%s%.1f/%.1f dead=%d",
                stage, (void*)guard, obj, fn, (unsigned long long)total, hasHealth ? "" : "?",
                cur, mx, dead);
        }
        else
        {
            LOG("[AI-DEATH] blocked Hook Twin death pipeline stage=%s guard=%p obj=%p fn=%p total=%llu health=%s%.1f/%.1f dead=%d",
                stage, (void*)guard, obj, fn, (unsigned long long)total, hasHealth ? "" : "?",
                cur, mx, dead);
        }
    }

    bool HookTwinDeathPipelineShouldSwallow(void* obj, void* fn, void* params)
    {
        ResolveHookTwinDeathPipelineFns();
        EnsureHookTwinFightStagingNativeHook();
        EnsureHookTwinActionContainerFactory();

        void* fK2Death = g_fnHookK2OnDeath.load(std::memory_order_relaxed);
        void* fK2Load = g_fnHookK2OnLoadDeathState.load(std::memory_order_relaxed);
        void* fLoadDead = g_fnHookLoadDeadState.load(std::memory_order_relaxed);
        void* fStaging = g_fnHookTryFightStaging.load(std::memory_order_relaxed);
        void* fStagingNative = g_fnHookTryFightStagingNative.load(std::memory_order_relaxed);
        void* fSendDeath = g_fnHookSendDeathEvent.load(std::memory_order_relaxed);
        void* fSendLoad = g_fnHookSendLoadDeathStateEvent.load(std::memory_order_relaxed);
        void* fDied = g_fnHookSendCharacterDiedEvent.load(std::memory_order_relaxed);
        void* fStartQte = g_fnHookStartVersusQTE.load(std::memory_order_relaxed);
        void* fCachePose = g_fnHookCacheDeathPose.load(std::memory_order_relaxed);
        void* fDestroyOwner = g_fnHookDestroyOwnerCharacter.load(std::memory_order_relaxed);

        // Forensics pass #2: after a Twin kills with laser/explosive ability, the game
        // immediately delivers self/AOE damage to the Twin, runs ProcessIncomingDamage,
        // then BP_AIHitReactions_Twins_Default and montage interruption kick her into the
        // floor pose. Hook Twins are supposed to be invincible bodyguards, so fail all
        // reflected incoming-damage paths closed BEFORE hit reactions/ability interrupts.
        if (obj && IsHookTwinBodyguard(reinterpret_cast<UObject*>(obj)) && fn)
        {
            std::string fnName = SafeObjectFullName(static_cast<UObject*>(fn));
            if (fnName.find("K2_ModifyIncomingDamageWithSpecialDamageResistance") != std::string::npos)
            {
                if (Mem::IsReadable(params, sizeof(P_HookModifyIncomingDamage)))
                {
                    auto* p = reinterpret_cast<P_HookModifyIncomingDamage*>(params);
                    p->OutDamage = 0.0f;
                }
                static ULONGLONG lastDmgLog = 0; ULONGLONG nowD = GetTickCount64();
                if (nowD - lastDmgLog > 500) { lastDmgLog = nowD; LOG("[AI-DEATH] blocked Hook Twin incoming damage modifier obj=%p fn=%p", obj, fn); }
                return true;
            }
            if (fnName.find("ProcessIncomingDamage") != std::string::npos)
            {
                if (Mem::IsReadable(params, sizeof(P_HookProcessIncomingDamage)))
                {
                    auto* p = reinterpret_cast<P_HookProcessIncomingDamage*>(params);
                    p->DamageDone = 0.0f;
                    p->ReturnValue = 0.0f;
                }
                RestoreHookTwinAfterDeathPipelineBlock(reinterpret_cast<UObject*>(obj));
                static ULONGLONG lastProcDmgLog = 0; ULONGLONG nowD = GetTickCount64();
                if (nowD - lastProcDmgLog > 500) { lastProcDmgLog = nowD; LOG("[AI-DEATH] blocked Hook Twin ProcessIncomingDamage obj=%p fn=%p", obj, fn); }
                return true;
            }
        }

        const bool objectBound = (fn == fK2Death || fn == fK2Load || fn == fLoadDead || fn == fStaging);
        const bool ownerEvent = (fn == fSendDeath || fn == fSendLoad);
        const bool gameplayDied = (fn == fDied);
        const bool qteStart = (fn == fStartQte);
        const bool qteCachePose = (fn == fCachePose);
        const bool destroyOwner = (fn == fDestroyOwner);
        if (fn == fStaging && obj && IsHookTwinBodyguard(reinterpret_cast<UObject*>(obj)))
        {
            static ULONGLONG lastStagingPeLogMs = 0;
            ULONGLONG nowS = GetTickCount64();
            if (nowS - lastStagingPeLogMs > 1000)
            {
                lastStagingPeLogMs = nowS;
                LOG("[AI-FSTAGE] reflected TryActivateFightStagingAbility observed for Hook Twin; native selector hook owns the decision obj=%p fn=%p", obj, fn);
            }
            // Do not return here. This reflected UFunction is the AAHAICharacter
            // bool/no-arg TryActivateFightStagingAbility, and for Hook Twins we want
            // to fail it closed below by setting ReturnValue=false and swallowing.
        }
        if (!objectBound && !ownerEvent && !gameplayDied && !qteStart && !qteCachePose && !destroyOwner)
            return false;

        ULONGLONG now = GetTickCount64();
        UObject* guard = nullptr;
        const char* stage = "unknown";
        bool qte = false;

        if (objectBound)
        {
            guard = reinterpret_cast<UObject*>(obj);
            if (!IsHookTwinBodyguard(guard))
                return false;
            if (fn == fK2Death) stage = "AHAICharacter.K2_OnDeath";
            else if (fn == fK2Load) stage = "AHAICharacter.K2_OnLoadDeathState";
            else if (fn == fLoadDead) stage = "AHAICharacter.LoadDeadState";
            else
            {
                stage = "AHAICharacter.TryActivateFightStagingAbility";
                if (Mem::IsReadable(params, sizeof(P_HookTryFightStaging)))
                    reinterpret_cast<P_HookTryFightStaging*>(params)->ReturnValue = false;
                g_hookTwinFightStagingBlocks.fetch_add(1, std::memory_order_relaxed);
                LOG("[AI-FSTAGE] BLOCKED reflected Hook Twin TryActivateFightStagingAbility obj=%p fn=%p native=%p", obj, fn, fStagingNative);
            }
        }
        else if (ownerEvent)
        {
            if (!Mem::IsReadable(params, sizeof(P_HookDeathEventOwner)))
                return false;
            guard = reinterpret_cast<UObject*>(reinterpret_cast<P_HookDeathEventOwner*>(params)->EventOwnerCharacter);
            if (!IsHookTwinBodyguard(guard))
                return false;
            stage = (fn == fSendDeath) ? "AHCharacterEventUtils.SendDeathEvent" : "AHCharacterEventUtils.SendLoadDeathStateEvent";
        }
        else if (gameplayDied)
        {
            if (!Mem::IsReadable(params, sizeof(P_HookGameplayCharacterDied)))
                return false;
            guard = reinterpret_cast<UObject*>(reinterpret_cast<P_HookGameplayCharacterDied*>(params)->DeadCharacter);
            if (!IsHookTwinBodyguard(guard))
                return false;
            stage = "GameplayEventSubsystem.SendCharacterDiedEvent";
        }
        else if (destroyOwner)
        {
            guard = HookTwinOwnerFromDeathAbility(obj);
            if (!IsHookTwinBodyguard(guard))
                return false;
            stage = "AIDeathAbility.DestroyOwnerCharacter";
        }
        else if (qteStart)
        {
            if (!Mem::IsReadable(params, sizeof(P_HookStartVersusQTE)))
                return false;
            auto* p = reinterpret_cast<P_HookStartVersusQTE*>(params);
            UObject* assaulter = reinterpret_cast<UObject*>(p->InAssaulter);
            UObject* victim = reinterpret_cast<UObject*>(p->InVictim);
            if (IsHookTwinBodyguard(assaulter)) guard = assaulter;
            else if (IsHookTwinBodyguard(victim)) guard = victim;
            else HookTwinArrayContainsManagedBodyguard(p->InAdditionalActors, guard);
            if (!guard)
                return false;
            qte = true;
            stage = "QTESubsystem.StartVersusQTE";
        }
        else
        {
            unsigned long long until = g_hookTwinDeathPoseBlockUntilMs.load(std::memory_order_relaxed);
            if ((unsigned long long)now > until)
                return false;
            guard = reinterpret_cast<UObject*>(g_hookTwinDeathPoseGuard.load(std::memory_order_relaxed));
            if (guard && !IsHookTwinBodyguard(guard))
                guard = nullptr;
            qte = true;
            stage = "QTESubsystem.CacheDeathPose";
        }

        if (guard)
            RestoreHookTwinAfterDeathPipelineBlock(guard);
        ArmHookTwinDeathPoseCacheGuard(guard, now);

        uint64_t total = qte
            ? g_hookTwinQtePipelineBlocks.fetch_add(1, std::memory_order_relaxed) + 1
            : g_hookTwinDeathPipelineBlocks.fetch_add(1, std::memory_order_relaxed) + 1;
        LogHookTwinDeathBlock(stage, guard, obj, fn, qte, total);
        return true;
    }

    // Make `ai` hostile toward `other` using the game's own team setter, so the
    // engine's attitude solver returns Hostile and hits between them deal real
    // damage (no friendly-fire zeroing). attitude: 0 Friendly, 1 Neutral, 2 Hostile.
    bool SwitchAiTeamAttitude(UObject* ai, UObject* other, uint8_t attitude)
    {
        UFunction* fn = CachedClassFn(AH::Cls_AICharacter, "SwitchTeamToMatchCharacterAttitude");
        if (!AiUsable(ai) || !Mem::IsReadable(other, 0x30) || !fn)
            return false;
        EnsureOriginalTeamRecorded(ai); // so Release can undo this team change
        P_SwitchTeamToMatchCharacterAttitude p{ other, attitude, {} };
        ai->ProcessEvent(fn, &p);
        return true;
    }

    // Is this AI a COMBAT character? Non-combat NPCs (Larisa, civilians, pedestrians,
    // animals) are AHAICharacters too (so AiUsable is true) but run a pedestrian-style
    // behaviour tree with NO combat state machine -- calling SetCharacterAggressive on
    // them AVs deep in game code (the "enabling combat on Larisa crashes" bug). We
    // detect them by their OrdinaryBehaviorTree asset name (e.g. "BT_Pedestrian") and
    // refuse combat for them. Memoised per UClass (the BT is per type). Game-thread.
    bool AiIsCombatCapable(UObject* ai)
    {
        if (!AiUsable(ai))
            return false;
        UClass* cls = ai->Class();
        if (!Mem::IsReadable(cls, 0x30))
            return false;
        thread_local std::unordered_map<UClass*, unsigned char> memo;
        auto it = memo.find(cls);
        if (it != memo.end())
            return it->second != 0;

        bool combat = true; // default to combat-capable unless we positively detect a civilian BT
        try
        {
            uint8_t* b = reinterpret_cast<uint8_t*>(ai);
            if (Mem::IsReadable(b + AH::AICh_OrdinaryBehaviorTree, sizeof(void*)))
            {
                UObject* bt = *reinterpret_cast<UObject**>(b + AH::AICh_OrdinaryBehaviorTree);
                if (Mem::IsReadable(bt, 0x30))
                {
                    std::string n = bt->GetName();
                    for (char& c : n) c = (char)std::tolower((unsigned char)c);
                    static const char* kNonCombat[] = { "pedestrian", "civil", "animal", "passive", "crowd", "idle", "talk" };
                    for (const char* k : kNonCombat)
                        if (n.find(k) != std::string::npos) { combat = false; break; }
                }
            }
        }
        catch (...) { combat = false; } // unsure -> treat as non-combat (the safe side)
        memo[cls] = combat ? 1u : 0u;
        return combat;
    }

    // THE attack-forcing primitive: the native UAIUtils.SetCharacterAggressive
    // drives the AI into its aggressive state machine with `enemy` loaded.
    bool ForceCharacterAggressive(UObject* ai, UObject* enemy)
    {
        // Single crash chokepoint: never drive the aggressive state machine on a
        // non-combat NPC (no combat BT) -- it AVs past /EHa in game code.
        if (!AiUsable(ai) || !AiIsCombatCapable(ai))
            return false;
        UObject* lib = CachedObject(AH::Obj_AIUtils);
        UFunction* fn = CachedFn(AH::Fn_AIUtils_SetCharacterAggressive);
        if (!Mem::IsReadable(lib, 0x30) || !fn)
            return false;
        P_SetCharacterAggressive p{};
        p.InAICharacter = ai;
        p.bShouldBeActive = true;
        p.InTargetEnemy = enemy;
        p.InAggressiveAnimationPlayRate = 1.0f;
        lib->ProcessEvent(fn, &p);
        return true;
    }

    // The inverse: DEACTIVATE the aggressive state machine so a guard cleanly EXITS combat
    // (otherwise the attack state stays latched and it keeps swinging at where its dead
    // target was). Call only on the combat->idle transition (it's a state-machine kick).
    bool StopCharacterAggressive(UObject* ai)
    {
        if (!AiUsable(ai) || !AiIsCombatCapable(ai))
            return false;
        UObject* lib = CachedObject(AH::Obj_AIUtils);
        UFunction* fn = CachedFn(AH::Fn_AIUtils_SetCharacterAggressive);
        if (!Mem::IsReadable(lib, 0x30) || !fn)
            return false;
        P_SetCharacterAggressive p{};
        p.InAICharacter = ai;
        p.bShouldBeActive = false;
        p.InTargetEnemy = nullptr;
        p.InAggressiveAnimationPlayRate = 1.0f;
        lib->ProcessEvent(fn, &p);
        return true;
    }

    // Per-actor injection memory (game-thread/pump only). Lets us re-assert the
    // light writes every tick while throttling the heavy state-machine kick to
    // target changes / a slow heartbeat, so montages don't thrash, and rate-limit
    // the follow move requests.
    struct InjectState
    {
        UObject*  target = nullptr;
        ULONGLONG lastHeavyMs = 0;
        ULONGLONG lastMoveMs = 0;
        FVector   lastGoal{};        // player pos at the last MoveTo issue (re-issue on drift, not every pump)
        float     lastDist = 0.0f;   // last distance to player (progress / stuck detection)
        ULONGLONG lastProgressMs = 0;
        ULONGLONG lastFriendlyMs = 0; // last time we re-asserted Friendly-to-player (throttled)
        UObject*  hitTarget = nullptr;
        UObject*  lastClearedTarget = nullptr;
        float     lastTargetHealth = -1.0f;
        ULONGLONG targetArmedMs = 0;
        ULONGLONG lastClearMs = 0;
        ULONGLONG lastClearHeavyMs = 0;
        bool      followMoving = false;
        ULONGLONG lastFollowGoalMs = 0;
        ULONGLONG lastMoveRecoveryMs = 0;
        FVector   followSampleLoc{};
        ULONGLONG followSampleMs = 0;
        ULONGLONG followLastProgressMs = 0;
        bool      velocityFallback = false;
        uint32_t  movementRecoveries = 0;
    };
    std::unordered_map<UObject*, InjectState> g_inject; // game thread (pump) only

    // When a guard is actively engaging a threat, the bodyguard combat injection stamps
    // "engaged until now+X" here so the per-frame follow drive (DriveSquadVelocityGameThread)
    // LEAVES IT ALONE to fight, instead of yanking its FollowLocation back to the player
    // every 50ms (that tug-of-war was why a recruited robot stalled vel=0 until combat
    // ended, then finally followed). Game-thread only (both writers/readers run there).
    std::unordered_map<UObject*, unsigned long long> g_engagedUntilMs;

    // Last time a guard had a live threat in range. Used to HOLD the combat posture for
    // a short window after the last threat so a single-pump gap in threat selection does
    // NOT flip the guard back to its docile team (that flip-flop reset the native combat
    // state every ~200ms, so it never sustained a fight -- the "never engages" bug).
    std::unordered_map<UObject*, unsigned long long> g_lastThreatMs;
    constexpr unsigned long long kCombatHoldMs = 1200; // bridge brief threat-selection gaps, then resume follow

    // =======================================================================
    //  TWIN  --  the flagship "super bodyguard" (separate from the squad drive)
    // -----------------------------------------------------------------------
    //  The Twin (AAIMixedNavigationCharacter) is driven by her OWN dedicated path,
    //  NOT the normal squad bodyguard code (DriveSpawnedAllies / the per-frame squad
    //  follow). The goal: recreate her native AI -- real attack montages, flight, smooth
    //  locomotion -- but fully under our control. Her native captures (released, chasing
    //  the player) show the recipe: target + IsAggressive + her own BT/path-following move
    //  + attack her smoothly, on 2D nav by default, briefly flying (movement_mode 5) at
    //  close range. So we SEED combat like a native enemy and otherwise GET OUT OF HER WAY,
    //  instead of the per-frame key-hammering / nav-pinning / state-machine re-kicking that
    //  the normal squad drive does (that fought her flight and restarted her montages).
    //  All state here is GAME-THREAD ONLY (the combat brain + per-frame follow both run there).
    struct TwinState
    {
        UObject*  target        = nullptr; // current engaged threat
        ULONGLONG engagedUntilMs = 0;      // per-frame follow leaves her alone until this (she's fighting)
        ULONGLONG lastThreatMs   = 0;      // last in-range threat -> combat hold across brief gaps
        ULONGLONG lastKickMs     = 0;      // last native attack-state kick (throttled -> montage-safe)
        ULONGLONG lastTeamMs     = 0;      // last team write (SetGenericTeamId re-triggers perception)
        ULONGLONG lastFriendlyMs = 0;      // last friendly re-assert while idle
        int       combatPhase    = -1;     // -1 init, 0 idle/follow, 1 engaged
        // follow smoothing (per-frame)
        bool      moving        = false;   // hysteresis: currently driving a move-to
        int       followState   = 0;       // 0 init, 1 moving, 2 holding (transition detection)
        ULONGLONG lastGoalMs    = 0;       // last Mercuna MoveToActor re-target
        ULONGLONG lastFlushMs   = 0;       // last standalone Mercuna Stop (flush stacked path requests; own pump)
        int       mercCount     = 0;       // diag: total MoveToActor issues
        // stuck/stairs traversal: FORCE flight to cross, then ground her ONCE back on flat
        float     closeBestM    = -1.0f;   // smallest follow distance reached since last progress
        ULONGLONG closeBestMs   = 0;       // when she last made progress closing on you
        bool      traverse3D    = false;   // true = forced to FLIGHT to cross stairs/gap (grounds ONCE on recovery)
    };
    std::unordered_map<UObject*, TwinState> g_twin; // game thread only

    constexpr float              kTwinEngageM         = 40.0f; // engage the nearest threat within this of you OR her (= kGuardEngageM)
    constexpr unsigned long long kTwinCombatHoldMs    = 1500;  // hold the combat posture across brief threat-selection gaps
    constexpr unsigned long long kTwinKickHeartbeatMs = 1500;  // re-kick the attack state while IN melee range (sustained swings, but not every pump)
    constexpr float              kTwinFollowBandM     = 1.5f;  // hysteresis: start moving only when this far BEYOND the stop ring

    // ---- follow-the-player (squad members + spawns) ----
    constexpr float     kFollowKeepM     = 3.5f;  // legacy; clean follow uses State.aiFollowStopM
    constexpr float     kFollowTeleportM = 30.0f; // OPT-IN teleport leash distance (only if aiAllowTeleport; default off -> they WALK)
    constexpr float     kGuardEngageM    = 40.0f; // a guard engages a threat within this range of YOU or of itself
    constexpr ULONGLONG kMoveReissueMs   = 800;   // don't restart pathfinding more often than this -- re-issuing every pump WAS the "stutter / stand still / circle"
    // She's fast and overshoots, so finish a bit early and stand off: aim the MoveTo at a
    // 2.5 m ring (her braking/momentum then coasts her to ~2 m) and only (re)chase once you're
    // past 3.5 m. The 1 m hysteresis band keeps her parked instead of jittering at the edge.
    constexpr float     kHookFollowStopM = 4.0f;   // wider hold ring = less twitchy recast/turn spam
    constexpr float     kHookFollowStartM = 6.0f;  // re-chase ring (hysteresis)
    constexpr float     kHookTwinPrettyWalkSpeed = 260.0f; // keep Twin in walk-cycle range instead of rigid run
    constexpr float     kHookTwinCombatWalkSpeed = 650.0f; // restore combat-capable movement while engaging
    constexpr float     kHookPerimeterM = 8.0f;   // Hook Twin only engages enemies very close to the player
    constexpr float     kHookAttackerAwarenessM = 18.0f; // or active attackers in a wider-but-bounded bubble
    constexpr float     kHookFirstHitContactM = 4.5f;

    bool ApplyAiKill(UObject* ai);

    // Reliable nav walk toward an actor (AIBlueprintHelperLibrary.SimpleMoveToActor):
    // sets up persistent path following on the controller so it WALKS (with locomotion
    // anim) to the goal. This is what actually makes an arbitrary enemy AI follow you.
    bool SimpleMoveToActor(UObject* ctrl, UObject* controlledPawn, UObject* goal)
    {
        UObject* lib = CachedObject(AH::Obj_AIBlueprintHelper);
        UFunction* fn = CachedFn(AH::Fn_AIBlueprintHelper_SimpleMoveToActor);
        if (!Mem::IsReadable(lib, 0x30) || !ControllerPathFollowingReady(ctrl, controlledPawn) ||
            !Mem::IsReadable(goal, 0x30) || !Mem::IsReadable(fn, 0x30))
            return false;
        struct { void* Controller; void* Goal; } p{ ctrl, goal };
        lib->ProcessEvent(fn, &p);
        return true;
    }

    // LOW-LEVEL movement: push the pawn directly via the character movement component
    // (Pawn.AddMovementInput), bypassing the behaviour tree. dir need not be unit.
    bool AddPawnMovementInput(UObject* pawn, const FVector& dir, float scale)
    {
        UFunction* fn = CachedFn(AH::Fn_Pawn_AddMovementInput);
        if (!PawnUsable(pawn) || !Mem::IsReadable(fn, 0x30))
            return false;
        P_AddMovementInput p{};
        p.WorldDirection = dir;
        p.ScaleValue = scale;
        p.bForce = true; // ignore the AI's input-ignore gate
        pawn->ProcessEvent(fn, &p);
        return true;
    }

    uint8_t* GetCharacterMovementPtr(UObject* pawn)
    {
        if (!CharacterUsable(pawn))
            return nullptr;
        uint8_t* base = reinterpret_cast<uint8_t*>(pawn);
        if (!Mem::IsReadable(base + AH::Char_CharacterMovement, sizeof(void*)))
            return nullptr;
        uint8_t* movement = *reinterpret_cast<uint8_t**>(base + AH::Char_CharacterMovement);
        return Mem::IsReadable(movement, AH::Move_MaxWalkSpeed + sizeof(float)) ? movement : nullptr;
    }

    bool WriteHookFollowVelocity(UObject* pawn, const FVector& direction, float speed)
    {
        uint8_t* movement = GetCharacterMovementPtr(pawn);
        if (!movement || !Mem::IsReadable(movement + AH::Move_Velocity, sizeof(FVector)))
            return false;
        FVector& velocity = *reinterpret_cast<FVector*>(movement + AH::Move_Velocity);
        velocity.X = direction.X * speed;
        velocity.Y = direction.Y * speed;
        // Preserve vertical physics: stairs, falling and ability landing remain native.
        return true;
    }

    bool RecoverHookFollowerMovement(UObject* pawn, UObject* controller)
    {
        uint8_t* movement = GetCharacterMovementPtr(pawn);
        if (!movement)
            return false;

        UObject* movementObject = reinterpret_cast<UObject*>(movement);
        bool touched = false;
        // Reactivate the component and its tick using reflected engine functions. These
        // are only called after measured zero displacement, never continuously.
        if (UFunction* fn = CachedObjectClassFn(movementObject, "SetComponentTickEnabled"))
        {
            P_BoolParam p{ true };
            movementObject->ProcessEvent(fn, &p);
            touched = true;
        }
        if (UFunction* fn = CachedObjectClassFn(movementObject, "Activate"))
        {
            P_BoolParam p{ true }; // bReset=true
            movementObject->ProcessEvent(fn, &p);
            touched = true;
        }

        // A stopped combat/ability state can leave this regular ground guard in
        // MOVE_None/custom mode. Hook Diagnostics excludes Twins here, so restoring
        // Walking is the correct fail-closed recovery for its follow state.
        if (Mem::IsReadable(movement + AH::Move_MovementMode, 1))
        {
            uint8_t& mode = *reinterpret_cast<uint8_t*>(movement + AH::Move_MovementMode);
            if (mode != AH::MOVE_Walking)
            {
                mode = AH::MOVE_Walking;
                touched = true;
            }
        }
        if (controller)
        {
            if (UFunction* stop = CachedObjectClassFn(controller, "StopMovement"))
                ProcessNoParams(controller, stop);
        }
        EnsureFollowerCanMove(pawn);
        return touched;
    }

    struct HookPathChainState
    {
        UObject* pathFollower = nullptr;
        UObject* boundMovement = nullptr;
        UObject* characterMovement = nullptr;
        UObject* navData = nullptr;
        FVector velocity{};
        uint8_t movementMode = 0;
        bool repairedBinding = false;
        bool reactivated = false;
    };

    bool ActivateHookComponent(UObject* component)
    {
        if (!Mem::IsReadable(component, 0x30)) return false;
        bool ok = false;
        if (UFunction* fn = CachedObjectClassFn(component, "SetComponentTickEnabled"))
        {
            P_BoolParam p{ true };
            component->ProcessEvent(fn, &p);
            ok = true;
        }
        if (UFunction* fn = CachedObjectClassFn(component, "Activate"))
        {
            P_BoolParam p{ true };
            component->ProcessEvent(fn, &p);
            ok = true;
        }
        return ok;
    }

    // ReVa chain: AIController +0x798 -> PathFollowingComponent +0x428 -> MovementComp +0x108.
    // A spawned Hook Twin can accept the route and report Moving while that final binding is null,
    // stale, inactive, or still attached to its 3D movement backend. Repair only the Hook Twin's
    // ground-follow chain and leave every other AI/component untouched.
    bool EnsureHookTwinGroundPathChain(UObject* guard, UObject* ctrl, bool reactivate,
                                       HookPathChainState& out)
    {
        if (!ControllerOwnsPawn(ctrl, guard)) return false;
        auto* controllerBytes = reinterpret_cast<uint8_t*>(ctrl);
        if (!Mem::IsReadable(controllerBytes + AH::AICtrl_PathFollowing, sizeof(void*))) return false;
        out.pathFollower = *reinterpret_cast<UObject**>(controllerBytes + AH::AICtrl_PathFollowing);
        uint8_t* path = reinterpret_cast<uint8_t*>(out.pathFollower);
        uint8_t* movement = GetCharacterMovementPtr(guard);
        out.characterMovement = reinterpret_cast<UObject*>(movement);
        if (!Mem::IsReadable(path, 0x30) || !movement ||
            !Mem::IsReadable(path + AH::PathFollow_MovementComp, sizeof(void*))) return false;

        out.boundMovement = *reinterpret_cast<UObject**>(path + AH::PathFollow_MovementComp);
        if (out.boundMovement != out.characterMovement)
        {
            *reinterpret_cast<UObject**>(path + AH::PathFollow_MovementComp) = out.characterMovement;
            out.boundMovement = out.characterMovement;
            out.repairedBinding = true;
            reactivate = true;
        }
        if (Mem::IsReadable(path + AH::PathFollow_NavData, sizeof(void*)))
            out.navData = *reinterpret_cast<UObject**>(path + AH::PathFollow_NavData);

        if (reactivate)
        {
            out.reactivated = ActivateHookComponent(out.pathFollower);
            out.reactivated = ActivateHookComponent(out.characterMovement) || out.reactivated;
        }

        if (Mem::IsReadable(movement + AH::Move_MovementMode, 1))
        {
            out.movementMode = *reinterpret_cast<uint8_t*>(movement + AH::Move_MovementMode);
            if (out.movementMode != AH::MOVE_Walking)
            {
                UObject* movementObject = reinterpret_cast<UObject*>(movement);
                if (UFunction* fn = CachedObjectClassFn(movementObject, "SetMovementMode"))
                {
                    P_SetMovementMode p{ AH::MOVE_Walking, 0 };
                    movementObject->ProcessEvent(fn, &p);
                }
                if (Mem::IsReadable(movement + AH::Move_MovementMode, 1) &&
                    *reinterpret_cast<uint8_t*>(movement + AH::Move_MovementMode) != AH::MOVE_Walking)
                    *reinterpret_cast<uint8_t*>(movement + AH::Move_MovementMode) = AH::MOVE_Walking;
                out.movementMode = *reinterpret_cast<uint8_t*>(movement + AH::Move_MovementMode);
                out.reactivated = true;
            }
        }
        if (Mem::IsReadable(movement + AH::Move_Velocity, sizeof(FVector)))
            out.velocity = *reinterpret_cast<FVector*>(movement + AH::Move_Velocity);
        EnsureFollowerCanMove(guard);
        return out.boundMovement == out.characterMovement && out.movementMode == AH::MOVE_Walking;
    }

    // Teleport a follower right next to the player. The reliability failsafe so
    // guards stay CLOSE even across gaps / kill-zones / nav holes where pathing
    // can't reach you. ~2 m behind the player, dropped onto the floor.
    bool TeleportActorNear(UObject* actor, const FVector& playerLoc)
    {
        UFunction* fn = CachedFn(AH::Fn_K2_TeleportTo);
        if (!Mem::IsReadable(actor, 0x30) || !fn)
            return false;
        FRotator rot{};
        GetControlRotation(rot);
        const float yaw = rot.Yaw * 0.01745329251994329577f;
        P_TeleportTo p{};
        p.DestLocation = { playerLoc.X - cosf(yaw) * 200.0f, playerLoc.Y - sinf(yaw) * 200.0f, playerLoc.Z + 30.0f };
        p.DestRotation = { 0.0f, rot.Yaw, 0.0f };
        actor->ProcessEvent(fn, &p);
        return true;
    }

    // Follow driver (game-thread half). The real, smooth locomotion is done per-frame
    // on the game thread by DriveSquadVelocityGameThread (RequestDirectMove emulation,
    // driven from hkProcessEvent -- nav-free + BT-proof, the primitive the game's own
    // movement bottoms out at). Here we only keep the member tick-enabled + brisk walk
    // (EnsureFollowerCanMove) so that per-frame velocity actually carries it, and give
    // the opt-in teleport backstop for a member stranded across a gap. No MoveToActor
    // (the Mercuna PathFollowingComponent ignores its abstract path) and no glide.
    bool DriveFollow(UObject* ai, UObject* player, const FVector& playerLoc, bool busyFighting)
    {
        if (!FollowPawnUsable(ai) || !Mem::IsReadable(player, 0x30) || ai == player)
            return false;
        EnsureFollowerCanMove(ai);
        FVector aiLoc{};
        if (!ReadActorLocationFast(ai, aiLoc))
            return false;
        float d = DistanceMetres(aiLoc, playerLoc);

        InjectState& s = g_inject[ai];
        ULONGLONG now = GetTickCount64();

        float stopM = Features::Get().aiFollowStopM;
        if (stopM < 0.5f) stopM = 0.5f;
        if (stopM > 15.0f) stopM = 15.0f;

        // Fighting a close threat: let it fight, don't pull. Reset follow bookkeeping.
        if (busyFighting)
        {
            s.lastMoveMs = now; s.lastGoal = playerLoc; s.lastDist = d; s.lastProgressMs = now;
            return true;
        }

        // The actual locomotion is driven per game frame by DriveSquadVelocityGameThread
        // (RequestDirectMove emulation from hkProcessEvent -- the nav-free, BT-proof
        // primitive the game's own movement bottoms out at). Here we only (a) keep the
        // unit tick-enabled + at a brisk walk speed so that per-frame velocity actually
        // carries it (done above via EnsureFollowerCanMove), and (b) provide the opt-in
        // teleport backstop for a member stranded across a gap the walk can't cross.
        if (d <= stopM)
        {
            s.lastGoal = playerLoc; s.lastDist = d; s.lastProgressMs = now;
            return true;
        }

        if (s.lastProgressMs == 0 || d + 0.3f < s.lastDist) { s.lastDist = d; s.lastProgressMs = now; }
        bool stuck = (now - s.lastProgressMs > 3000);
        if (stuck && Features::Get().aiAllowTeleport && d > kFollowTeleportM)
        {
            TeleportActorNear(ai, playerLoc);
            s.lastDist = 0.0f; s.lastProgressMs = now;
        }
        return true;
    }

    // Full "all bases covered" attack injection of `enemy` into `ai`. The cheap
    // raw writes + game setter run every call; the heavy native attack-state kick
    // is throttled. There are two mutually exclusive team modes, picked by caller:
    // teamRef/attitudeVsRef drive the native team switch (used by bodyguard:
    // friendly to the player). forceTeamId >= 0 instead writes a raw team id and
    // skips the native switch -- that is the "fight each other" path, which keeps
    // every brawler hostile to (killable by) the player. Pass exactly one mode.
    bool InjectAttack(UObject* ai, UObject* enemy, UObject* teamRef, uint8_t attitudeVsRef, int forceTeamId = -1)
    {
        if (!AiUsable(ai) || !Mem::IsReadable(enemy, 0x30) || ai == enemy)
            return false;

        // (1)(2) raw aggro target + aggressive gates -- crash-safe data writes.
        WriteAiTargetField(ai, enemy);
        WriteAiAggressiveFlags(ai, true);
        // (3) the game's own setter routes the target into the blackboard.
        SetAiTargetEnemy(ai, enemy, true);
        // (3b) drive the controller blackboard so the BT enters its ATTACK branch.
        // SetBlackboardIsAggressive is what turns "approach" into an actual swing.
        if (UObject* ctrl = GetAiController(ai))
        {
            SetControllerTargetEnemy(ctrl, enemy, true);
            SetControllerAggressive(ctrl, true);
        }

        ULONGLONG now = GetTickCount64();
        InjectState& s = g_inject[ai];
        bool targetChanged = (s.target != enemy);

        // (4) team handling. Both team paths now go through the CHARACTER's own
        // Get/SetGenericTeamId (ProcessEvent), which can re-trigger perception, so
        // unlike the old raw byte poke we must NOT hammer them every pass. The
        // bodyguard friendly switch is gated to target changes; the fight-team split
        // is folded into the throttled heavy block below.
        if (forceTeamId < 0 && targetChanged && teamRef && Mem::IsReadable(teamRef, 0x30))
            SwitchAiTeamAttitude(ai, teamRef, attitudeVsRef);
        // (5) heavy, throttled: fight-team split + force the attack state machine.
        if (targetChanged || (now - s.lastHeavyMs > 1500))
        {
            if (forceTeamId >= 0)
                SetAiTeamIdTracked(ai, (uint8_t)forceTeamId);
            ForceCharacterAggressive(ai, enemy);
            s.lastHeavyMs = now;
        }
        s.target = enemy;
        s.lastClearedTarget = nullptr;
        s.lastClearMs = 0;
        return true;
    }

    // Belt-and-suspenders player safety: if a guard's cached aggro target is YOU,
    // blank it across every layer (raw field + game setter + controller blackboard)
    // right now. Re-run every pump so even a perception flip after taking damage
    // can't land a hit on you before the next drive. Crash-safe (guarded reads).
    void SuppressGuardTargetingPlayer(UObject* guard, UObject* player)
    {
        uint8_t* gb = reinterpret_cast<uint8_t*>(guard);
        if (!Mem::IsReadable(gb + AH::AICh_CachedTargetEnemy, sizeof(void*)))
            return;
        UObject* cur = *reinterpret_cast<UObject**>(gb + AH::AICh_CachedTargetEnemy);
        if (cur != player)
            return;
        WriteAiTargetField(guard, nullptr);
        SetAiTargetEnemy(guard, nullptr, true);
        if (UObject* ctrl = GetAiController(guard))
            SetControllerTargetEnemy(ctrl, nullptr, true);
    }

    void ClearHookBodyguardCombat(UObject* guard, UObject* player, const char* reason)
    {
        InjectState& s = g_inject[guard];
        UObject* cached = ReadAiTargetField(guard);
        UObject* clearTarget = nullptr;
        if (s.target && s.target != player) clearTarget = s.target;
        else if (cached && cached != player) clearTarget = cached;
        ULONGLONG now = GetTickCount64();
        bool duplicateClear = clearTarget && s.lastClearedTarget == clearTarget && now - s.lastClearMs < 2500;
        bool heavyClear = clearTarget && (!duplicateClear || now - s.lastClearHeavyMs >= 1000);
        bool firstClear = clearTarget && !duplicateClear;
        bool hadEngaged = g_engagedUntilMs.find(guard) != g_engagedUntilMs.end();
        bool hadThreatHold = g_lastThreatMs.find(guard) != g_lastThreatMs.end();

        // Always remove the raw cached target; the game can rehydrate this from blackboard
        // after death. Duplicate clears stay quiet and avoid re-kicking the AI state machine.
        if (clearTarget)
            WriteAiTargetField(guard, nullptr);

        const bool twinSoftClear = IsHookTwinBodyguard(guard);

        if (heavyClear)
        {
            if (twinSoftClear)
            {
                // For the Twins, reflected AHAICharacter.SetTargetEnemy(nullptr) and
                // AHAIController.SetBlackboardTargetEnemy(nullptr) fire GA_FightStaging_Twins
                // K2_OnCharacterEvent, K2_OnEndAbility and PlayAnimMontage interruption.
                // That is the bad/stuck pose root cause from forensics. Clear only the raw
                // cached target + raw BlackboardComponent key, bypassing AHAI wrappers.
                if (UObject* ctrl = GetAiController(guard))
                    SetControllerObjectKeyAt(ctrl, AH::AICtrl_Key_TargetEnemy, nullptr);
            }
            else
            {
                SetAiTargetEnemy(guard, nullptr, true);
                if (UObject* ctrl = GetAiController(guard))
                    SetControllerTargetEnemy(ctrl, nullptr, true);
            }
            s.lastClearHeavyMs = now;
        }

        if (firstClear)
        {
            if (clearTarget)
                g_hookSuppressedThreatUntilMs[clearTarget] = now + 120000; // never reacquire the corpse/stale actor this fight
            if (UObject* ctrl = GetAiController(guard))
            {
                if (twinSoftClear)
                    SetControllerBoolKeyAt(ctrl, AH::AICtrl_Key_IsAggressive, false);
                else
                    SetControllerAggressive(ctrl, false);
            }
            // Yes: if the target is dead/gone, remove the Twin's aggressive state too.
            // We still avoid reflected target-clear wrappers, but SetCharacterAggressive(false)
            // plus ending melee/special abilities is the proper state-machine exit so
            // she doesn't keep looping attacks/circling a corpse in place.
            StopCharacterAggressive(guard);
            if (twinSoftClear)
            {
                ClearFightStagingSelectedObject(guard);
                HardStopHookTwinCombatAbilities(guard, reason);
            }
            if (!twinSoftClear)
            {
                SwitchAiTeamFriendlyTo(guard, player);
                s.lastFriendlyMs = now;
            }
            g_hookStaleTargetsCleared.fetch_add(1, std::memory_order_relaxed);
            LOG("[AI-HOOK] combat target cleared immediately: guard=%p target=%p reason=%s%s",
                (void*)guard, (void*)clearTarget, reason ? reason : "stand-down",
                twinSoftClear ? " (Twin soft-clear: no reflected target clear)" : "");
        }

        bool standDown = firstClear || heavyClear || (!clearTarget && (hadEngaged || hadThreatHold || s.hitTarget || s.targetArmedMs || s.lastTargetHealth > 0.0f));
        if (standDown)
        {
            if (twinSoftClear)
                g_hookCombatRouteAbortPending.insert(guard);
            if (UObject* ctrl = GetAiController(guard))
            {
                if (twinSoftClear)
                    SetControllerBoolKeyAt(ctrl, AH::AICtrl_Key_IsAggressive, false);
                else
                    SetControllerAggressive(ctrl, false);
            }
            StopCharacterAggressive(guard);
            if (twinSoftClear)
            {
                ClearFightStagingSelectedObject(guard);
                HardStopHookTwinCombatAbilities(guard, reason);
            }
            ClearAiAggressiveLatch(guard);
        }
        g_engagedUntilMs.erase(guard);
        g_lastThreatMs.erase(guard);
        if (clearTarget)
        {
            s.lastClearedTarget = clearTarget;
            s.lastClearMs = now;
        }
        s.target = nullptr;
        s.hitTarget = nullptr;
        s.lastTargetHealth = -1.0f;
        s.targetArmedMs = 0;
    }

    bool TryHookFirstHitKill(UObject* guard, UObject* target, const FVector& guardLoc,
                             const FVector& targetLoc, ULONGLONG now)
    {
        InjectState& s = g_inject[guard];
        float cur = -1.0f, mx = 0.0f;
        bool hasHealth = ReadCharacterHealth(target, cur, mx) && mx > 0.001f && std::isfinite(cur);

        if (s.hitTarget != target)
        {
            s.hitTarget = target;
            s.lastTargetHealth = hasHealth ? cur : -1.0f;
            s.targetArmedMs = now;
            return false;
        }

        bool damageObserved = hasHealth && s.lastTargetHealth > 0.0f && cur + 0.01f < s.lastTargetHealth;
        float targetDistance = DistanceMetres(guardLoc, targetLoc);
        // Damage deltas are authoritative. The close-contact fallback handles robots
        // whose friendly-fire filter suppresses the damage event despite a connected
        // native melee montage; 850 ms leaves time for the first swing to land.
        bool firstContact = targetDistance <= kHookFirstHitContactM &&
                            s.targetArmedMs && now - s.targetArmedMs >= 850;
        if (hasHealth)
            s.lastTargetHealth = cur;
        if (!damageObserved && !firstContact)
            return false;

        const char* source = damageObserved ? "damage-delta" : "melee-contact";
        bool killed = ApplyAiKill(target);
        if (killed)
        {
            uint64_t total = g_hookFirstHitKills.fetch_add(1, std::memory_order_relaxed) + 1;
            LOG("[AI-HIT] first-hit kill CONFIRMED: guard=%p target=%p source=%s distance=%.1fm total=%llu",
                (void*)guard, (void*)target, source, targetDistance, (unsigned long long)total);        }
        return killed;
    }

    // Dedicated Hook Diagnostics bodyguard state machine. Perception is an
    // all-direction cache scan (no view-cone test); the selector below limits it to
    // a 12 m protection perimeter, or 35 m only when an enemy is actively targeting
    // the player/managed guard. No stale combat hold is retained after death.
    bool InjectHookBodyguard(UObject* guard, UObject* player, const FVector& playerLoc,
                             UObject* threat, bool forceEngage = false)
    {
        if (!FollowPawnUsable(guard) || !Mem::IsReadable(player, 0x30) || guard == player)
            return false;

        EnsureFollowerCanMove(guard);
        SuppressGuardTargetingPlayer(guard, player);
        FVector guardLoc{}, threatLoc{};
        // Hook Diagnostics owns its protection/combat policy. Do not depend on
        // the normal AI/Squad tab's "squad aggressive" setting.
        bool engage = ReadActorLocationFast(guard, guardLoc) &&
                      IsLiveCombatTarget(threat) && threat != guard && threat != player &&
                      !IsHookBodyguard(threat) && ReadActorLocationFast(threat, threatLoc);

        if (engage)
        {
            UObject* enemyTarget = ReadAiTargetField(threat);
            bool attacksProtected = enemyTarget == player ||
                                    (enemyTarget && IsHookBodyguard(enemyTarget));
            float dPlayer = DistanceMetres(playerLoc, threatLoc);
            float dGuard = DistanceMetres(guardLoc, threatLoc);
            bool continuing = g_inject[guard].target == threat;
            // Strict bodyguard policy: protect, don't initiate. A Hook guard may attack
            // only a robot that is actively targeting the player / managed guard. Do not
            // continue just because this was the old target or is near the player; that is
            // exactly how the Twin stayed latched to the first corpse/fight spot.
            engage = forceEngage || (attacksProtected && std::min(dPlayer, dGuard) <= kHookAttackerAwarenessM);
        }

        if (!engage)
        {
            ClearHookBodyguardCombat(guard, player,
                threat && !IsLiveCombatTarget(threat) ? "dead/unreadable" : "no eligible 360-degree threat");
            return true; // Hook native follow owns all movement outside combat
        }

        SetCharacterInstigatedDamage(guard, 10000.0f);
        InjectAttack(guard, threat, nullptr, 0, (int)kGuardTeam);
        SuppressGuardTargetingPlayer(guard, player);
        g_engagedUntilMs[guard] = GetTickCount64() + 350; // bridges the 5 Hz selector only

        ULONGLONG now = GetTickCount64();
        if (TryHookFirstHitKill(guard, threat, guardLoc, threatLoc, now))
        {
            ClearHookBodyguardCombat(guard, player, "first hit completed");
            return true;
        }
        return true; // native combat/abilities own movement while engaged
    }

    // Force a mixed-navigation character (the Twins = AAIMixedNavigationCharacter) onto
    // GROUND nav so the follow drive can actually move it. WHY: a Twin switches between
    // ground walk and 3D flight; while flight nav is active it ignores the ground
    // FollowLocation / Mercuna path, so a recruited Twin just hovers in place ("every
    // character follows me except the Twin"). SetCanUseAutomaticNavigationTypeSelection(false)
    // stops it flipping back; ForceNavigationType(Navigation2D=1) pins it to ground.
    // Best-effort + fully guarded: both functions only exist on the mixed-nav class, so
    // CachedObjectClassFn returns null for every normal pawn -> this is a no-op for them
    // and cannot crash. Param buffers are oversized so ProcessEvent never reads past them.
    void ForceGroundNavIfMixed(UObject* ai)
    {
        if (!AiUsable(ai))
            return;
        bool okAuto = false, okType = false, exAuto = false, exType = false;
        if (UFunction* fn = CachedObjectClassFn(ai, "SetCanUseAutomaticNavigationTypeSelection"))
            if (Mem::IsReadable(fn, 0x30))
            { struct { bool v; uint8_t pad[7]; } p{ false, {0} }; try { ai->ProcessEvent(fn, &p); okAuto = true; } catch (...) { exAuto = true; } }
        if (UFunction* fn = CachedObjectClassFn(ai, "ForceNavigationType"))
            if (Mem::IsReadable(fn, 0x30))
            { struct { uint8_t t; uint8_t pad[7]; } p{ 1 /*ENavigationTypeSelector::Navigation2D (ground)*/, {0} }; try { ai->ProcessEvent(fn, &p); okType = true; } catch (...) { exType = true; } }
        // Throttled so the 1.5s re-pin doesn't flood, but exceptions always print.
        static ULONGLONG lastLog = 0; ULONGLONG nowL = GetTickCount64();
        if (exAuto || exType || nowL - lastLog > 3000)
        { lastLog = nowL; LOG("Twin nav -> GROUND(2D): setAuto(false)=%d forceType(1)=%d exAuto=%d exType=%d ai=%p", okAuto, okType, exAuto, exType, (void*)ai); }
    }

    // Is this a mixed-navigation character (the Twins = AAIMixedNavigationCharacter)?
    // ONLY that class exposes ForceNavigationType / SetCanUseAutomaticNavigationTypeSelection,
    // so a resolvable ForceNavigationType is a precise, crash-free "this is a Twin" test that
    // returns false for every normal pawn. Memoised per UClass (the result is per type), so
    // CachedObjectClassFn (which logs a one-time miss) runs at most once per class. This is the
    // gate that routes Twins into their OWN dedicated drive, fully separate from the normal squad.
    bool IsMixedNavCharacter(UObject* ai)
    {
        if (!AiUsable(ai))
            return false;
        UClass* cls = ai->Class();
        if (!Mem::IsReadable(cls, 0x30))
            return false;
        thread_local std::unordered_map<UClass*, unsigned char> memo;
        auto it = memo.find(cls);
        if (it != memo.end())
            return it->second != 0;
        bool mixed = (CachedObjectClassFn(ai, "ForceNavigationType") != nullptr);
        memo[cls] = mixed ? 1u : 0u;
        return mixed;
    }

    // Hand navigation-type control BACK to a Twin's own AI (the inverse of the ground pin
    // in ForceGroundNavIfMixed). We call this when she ENGAGES so her native combat AI can
    // transition to flight (movement_mode 5) on its own -- exactly like her released captures,
    // which fly briefly at close range while otherwise staying on 2D nav. Re-pinning to ground
    // every frame is what interrupted that transition mid-air ("flying bugs her out").
    void AllowMixedNavAuto(UObject* ai)
    {
        if (!AiUsable(ai))
            return;
        if (UFunction* fn = CachedObjectClassFn(ai, "SetCanUseAutomaticNavigationTypeSelection"))
            if (Mem::IsReadable(fn, 0x30))
            { struct { bool v; uint8_t pad[7]; } p{ true, {0} }; ai->ProcessEvent(fn, &p); }
    }

    // Force a mixed-nav Twin into FLIGHT (3D) nav so Mercuna paths her THROUGH THE AIR across
    // stairs/gaps the ground (2D) path can't cross. Called ONCE on entering the stuck/traversal
    // state (NOT on a timer). Verbose-logged so we can confirm the game accepts the command.
    void ForceFlightNavIfMixed(UObject* ai)
    {
        if (!AiUsable(ai))
            return;
        bool okAuto = false, okType = false, exAuto = false, exType = false;
        if (UFunction* fn = CachedObjectClassFn(ai, "SetCanUseAutomaticNavigationTypeSelection"))
            if (Mem::IsReadable(fn, 0x30))
            { struct { bool v; uint8_t pad[7]; } p{ false, {0} }; try { ai->ProcessEvent(fn, &p); okAuto = true; } catch (...) { exAuto = true; } }
        if (UFunction* fn = CachedObjectClassFn(ai, "ForceNavigationType"))
            if (Mem::IsReadable(fn, 0x30))
            { struct { uint8_t t; uint8_t pad[7]; } p{ 2 /*ENavigationTypeSelector::Navigation3D (flight)*/, {0} }; try { ai->ProcessEvent(fn, &p); okType = true; } catch (...) { exType = true; } }
        LOG("Twin nav -> FLIGHT(3D): setAuto(false)=%d forceType(2)=%d exAuto=%d exType=%d ai=%p", okAuto, okType, exAuto, exType, (void*)ai);
    }

    // Is this character currently airborne (movement_mode == Flying)? Crash-safe raw read.
    bool TwinIsAirborne(UObject* ai)
    {
        uint8_t* base = reinterpret_cast<uint8_t*>(ai);
        if (!Mem::IsReadable(base + AH::Char_CharacterMovement, sizeof(void*)))
            return false;
        uint8_t* mv = *reinterpret_cast<uint8_t**>(base + AH::Char_CharacterMovement);
        return Mem::IsReadable(mv + AH::Move_MovementMode, 1) &&
               *reinterpret_cast<uint8_t*>(mv + AH::Move_MovementMode) == AH::MOVE_Flying;
    }

    // Clear any idle/pedestrian SCHEDULE pinning her, so her BT falls through to its FOLLOW
    // branch instead of a schedule branch that ignores our follow keys (the "moving=1 but
    // vel=0 / turns to me but doesn't walk" stall). The schedule system re-applies the asset,
    // so we stash the original ONCE (ApplyAiRelease restores it) and null it every call. Also
    // clears the passive gate. Crash-safe raw writes. FOLLOW-only -- never run while she's
    // fighting (her combat AI owns her then, and this would be needless churn).
    void UnpinTwinSchedule(UObject* twin)
    {
        uint8_t* base = reinterpret_cast<uint8_t*>(twin);
        if (Mem::IsReadable(base + AH::AICh_bIsPassive, 1))
            *reinterpret_cast<bool*>(base + AH::AICh_bIsPassive) = false;
        if (Mem::IsReadable(base + AH::AICh_Schedule, sizeof(void*)))
        {
            UObject** slot = reinterpret_cast<UObject**>(base + AH::AICh_Schedule);
            if (*slot)
            {
                if (g_stashedSchedule.find(twin) == g_stashedSchedule.end())
                    g_stashedSchedule[twin] = *slot; // remember the real schedule once (for restore)
                *slot = nullptr;
            }
        }
    }

    // Bodyguard injection. THE FIX: a guard fights with the EXACT pipeline that
    // "enemies fight each other" uses -- it is parked on the dedicated combat ally
    // team kGuardTeam (NOT the player's docile team 0, which silently strips combat;
    // see kGuardTeam) and force-aggro'd onto the threat, so its OWN native combat AI
    // hunts the robots just like a free brawler. It NEVER hits you because (a) while
    // ENGAGING it is busy on the forced robot target and we blank any player-target
    // every pump, and (b) while IDLE (no threat) it is dropped back onto the docile
    // player faction (team 0, Friendly) where it can't acquire you at all. Two states:
    //   * threat in range  -> kGuardTeam + force attack (fight-each-other logic)
    //   * no threat        -> Friendly team 0, combat state cleared, follow only
    bool InjectBodyguard(UObject* guard, UObject* player, const FVector& playerLoc, UObject* threat)
    {
        if (!FollowPawnUsable(guard) || !Mem::IsReadable(player, 0x30) || guard == player)
            return false;
        if (g_hookBodyguardMode.load(std::memory_order_relaxed) && IsHookBodyguard(guard))
            return InjectHookBodyguard(guard, player, playerLoc, threat);

        // Drive combat for ANY squad AHAICharacter, NOT just ones the BT-name heuristic
        // (AiIsCombatCapable) flags "combat". WHY: the heuristic mis-flagged the Twins
        // (and bosses) as non-combat, so the gate skipped ALL injection -> they stayed on
        // the NEUTRAL player team 0 and never engaged (log: engaged=0 team=0 with a threat
        // 19m away). Combat here is the team-split (kGuardTeam) + forced target -- the
        // SAME perception-driven mechanism "enemies fight each other" uses, which does NOT
        // depend on that heuristic (fight-each-other already injects every nearby AI this
        // way). The only call that can fault on a TRUE civilian -- ForceCharacterAggressive
        // / StopCharacterAggressive -- stays self-gated on AiIsCombatCapable internally, so
        // this stays crash-safe while letting real combat units (mis-flagged or not) fight.
        const bool combatCapable = AiUsable(guard);
        InjectState& s = g_inject[guard];
        ULONGLONG nowMs = GetTickCount64();

        if (combatCapable)
        {
            // The player is protected/followed/focused but is NEVER stored in
            // TargetAlly: that native path requires AHAICharacter, while the player
            // is only AHBaseCharacter. Friendship is supplied by the Hook Debug
            // pair override and team logic instead.
            WriteAiAggressiveFlags(guard, true);
            SuppressGuardTargetingPlayer(guard, player);
        }

        // Engage a threat that is near YOU (defend me) OR near the guard itself
        // (intercept an attacker at the perimeter). A threat far from both is
        // ignored, so a guard keeps following you instead of running off across
        // the level after something distant.
        // Combat engage drives SetCharacterAggressive (the AI attack state machine).
        // It used to CRASH non-combat NPCs (Larisa), but combatCapable now excludes
        // those, so it's crash-safe. Gated by aiSquadAggressive (now DEFAULT ON) so the
        // squad fights for you out of the box; turn it off for peaceful followers. The
        // player is never a valid threat (threats come from the cache, which has no player).
        bool engage = false, busyFighting = false;
        FVector guardLoc{}, threatLoc{};
        bool guardLocOk = ReadActorLocationFast(guard, guardLoc);
        if (combatCapable && Features::Get().aiSquadAggressive
            && threat && Mem::IsReadable(threat, 0x30) && threat != guard && threat != player && guardLocOk
            && ReadActorLocationFast(threat, threatLoc))
        {
            float threatToYou   = DistanceMetres(playerLoc, threatLoc);
            float threatToGuard = DistanceMetres(guardLoc,  threatLoc);
            engage = (threatToYou <= kGuardEngageM) || (threatToGuard <= kGuardEngageM);
            busyFighting = engage && (threatToGuard <= 8.0f); // standing in melee
        }

        // HOLD the combat posture for a short window after the last in-range threat.
        // Without this, a single-pump gap in threat selection flipped the guard back to
        // its docile team every ~200ms -- which RESET the native combat state before it
        // could ever sustain a fight (the "moved to us but never attacked / never
        // engaged" bug). While holding we keep it armed and let its OWN combat AI run.
        if (engage)
            g_lastThreatMs[guard] = nowMs;
        bool combatHold = false;
        {
            auto it = g_lastThreatMs.find(guard);
            combatHold = combatCapable && it != g_lastThreatMs.end() && (nowMs - it->second < kCombatHoldMs);
        }

        if (combatCapable && engage)
        {
            // *** THE COPY YOU ASKED FOR: drive the guard with the EXACT "enemies fight
            // each other" injection and then GET OUT OF ITS WAY. forceTeamId = kGuardTeam
            // parks it on a real COMBAT team the robots read as hostile; we force the
            // first target + native attack-state kick, then its OWN combat AI moves it to
            // the enemy and attacks -- exactly like a released brawler (which is the ONLY
            // mode that ever worked, incl. for the Twins). Crucially we do NOT pin it
            // friendly or reset its team anywhere in the combat path; that per-pump reset
            // was why it never engaged. Player safety: it's busy on the forced robot
            // target, on a non-player team, and we re-blank any player-target every pump.
            InjectAttack(guard, threat, nullptr, 0, (int)kGuardTeam);
            SuppressGuardTargetingPlayer(guard, player);
            // Leave this guard alone in the per-frame follow drive long enough to actually
            // fight -- the follow tug-of-war (FollowLocation back to you every 50ms) was
            // half of why it stalled. The combat-hold re-stamps this through brief gaps.
            g_engagedUntilMs[guard] = nowMs + 1500;
            s.target = threat;
        }
        else if (combatCapable && combatHold)
        {
            // Recently fought, no fresh target THIS pump: HOLD. Stay on the combat team
            // (do NOT flip friendly), keep the engaged stamp so the follow drive keeps
            // its hands off, and let the guard's own perception keep hunting nearby
            // robots. Don't wipe its target -- let it finish what it's chasing.
            g_engagedUntilMs[guard] = nowMs + 600;
        }
        else if (combatCapable)
        {
            // Genuinely safe (no threat for kCombatHoldMs): stand down to docile follow.
            // Clear the BT attack state and drop onto the DOCILE player faction (team 0,
            // Friendly) so an idle guard can NEVER acquire you while there's nothing to
            // fight -- the hard never-hit-you guarantee for the idle case.
            WriteAiTargetField(guard, nullptr);
            SetAiTargetEnemy(guard, nullptr, true);
            if (UObject* ctrl = GetAiController(guard))
            {
                SetControllerTargetEnemy(ctrl, nullptr, true);
                SetControllerAggressive(ctrl, false);
            }
            g_engagedUntilMs.erase(guard);
            if (s.target != player)   // first idle pump after a fight -> exit combat cleanly
            {
                StopCharacterAggressive(guard); // un-latch the attack state machine
                SwitchAiTeamFriendlyTo(guard, player);
                s.lastFriendlyMs = nowMs;
            }
            else if (nowMs - s.lastFriendlyMs > 750)
            {
                SwitchAiTeamFriendlyTo(guard, player);
                s.lastFriendlyMs = nowMs;
            }
            s.target = player;
        }
        else
        {
            s.target = player;
        }

        // Follow only when NOT in a combat posture -- while engaged/holding, leave its
        // own combat locomotion alone so it can chase + attack (matches fight-each-other).
        bool followOk = DriveFollow(guard, player, playerLoc, busyFighting || combatHold);
        return followOk || combatCapable;
    }

    bool ApplyAiKill(UObject* ai)
    {
        if (!AiUsable(ai))
            return false;
        bool ok = SetCharacterHealthZero(ai); // safe direct write
        UFunction* suicide = CachedClassFn(AH::Cls_AICharacter, "Suicide");
        if (suicide && ProcessNoParams(ai, suicide))
            ok = true;
        if (ok)
            MarkAiDeathTombstone(ai, "ApplyAiKill");
        return ok;
    }

    bool ApplyAiFreeze(UObject* ai)
    {
        if (!AiUsable(ai))
            return false;
        // Memory-only freeze (see SetActorTimeDilation). The old freeze called
        // SetIsPassive / PauseBehaviorTree etc. via ProcessEvent, which crashed
        // the game when an actor went stale mid-command.
        return SetActorTimeDilation(ai, 0.0001f);
    }

    // NOTE on controller calls: the AHAIController is behaviour-tree driven, so
    // follow/attack come from its blackboard keys -- we drive those via the
    // controller's OWN guarded setters (SetBlackboardFollowLocation /
    // SetFollowLocationSpeed / SetBlackboardTargetAlly / SetBlackboardTargetEnemy /
    // SetBlackboardIsAggressive), each gated by ControllerBlackboardReady() (the
    // Blackboard component pointer must be readable) and run on the game-thread
    // pump. Generic AAIController::MoveToActor is only used by DriveFollow after
    // validating the controller owns the pawn and has a readable path-following
    // component; the unsafe focus / Start|Stop|Pause|ResumeBehaviorTree calls stay
    // removed because they fault inside game code when a controller's brain or
    // blackboard is null/mid-transition. AHAICharacter-level functions remain
    // guarded by AiUsable (IsReadable + IsA).

    bool ApplyAiRelease(UObject* ai)
    {
        if (!AiUsable(ai))
            return false;

        // *** Release => put the unit back EXACTLY as it was, so it behaves naturally. ***
        // Recruiting tracks the unit's real team (g_origTeam) before we ever touch it.
        // On release we restore that real robot team THROUGH THE ENGINE (SetGenericTeamId
        // refreshes the attitude/perception solver, unlike the old raw byte poke that
        // left it friendly+unkillable). Restoring the REAL team is strictly better than
        // the old "force Hostile-to-player": a real team-1 robot is already killable by
        // you (different team => your hits land) AND, with "fight each other" on, the
        // fight pump re-splits it so it rejoins the brawl. Forcing it Hostile-to-player
        // was exactly why a JUST-released unit spun around and attacked YOU instead of
        // going back to fighting the other enemies. Fallback to a hostile switch only if
        // we somehow never recorded an original team (guarantees it stays killable).
        UObject* player = GetLocalPawn();
        if (g_origTeam.count(ai))
            RestoreOriginalTeam(ai);                       // back to its real team; also erases the record
        else if (Mem::IsReadable(player, 0x30) && ai != player)
            SwitchAiTeamAttitude(ai, player, 2 /*ETeamAttitude::Hostile => guaranteed killable*/);
        g_origTeam.erase(ai); // it's a normal enemy now -- forget our team bookkeeping

        bool ok = SetActorTimeDilation(ai, 1.0f); // undo freeze (safe write)
        ok = SetAiPassive(ai, false) || ok;       // un-passive so it fights normally again
        SetAiTargetAlly(ai, nullptr);
        SetAiTargetEnemy(ai, nullptr, true);
        WriteAiTargetField(ai, nullptr);          // clear the raw cached aggro target
        // Clear the controller blackboard too, or a released enemy keeps the
        // aggressive/target state we pushed and won't actually stand down.
        if (UObject* ctrl = GetAiController(ai))
        {
            SetControllerTargetEnemy(ctrl, nullptr, true);
            SetControllerAggressive(ctrl, false);
            SetControllerForceFollow(ctrl, false); // let companion NPCs resume their own schedule
        }
        // Put back the idle schedule the follow-drive nulled so a released companion
        // resumes her own routine instead of standing unpinned forever.
        {
            auto its = g_stashedSchedule.find(ai);
            if (its != g_stashedSchedule.end())
            {
                uint8_t* b = reinterpret_cast<uint8_t*>(ai);
                if (Mem::IsReadable(b + AH::AICh_Schedule, sizeof(void*)) && Mem::IsReadable(its->second, 0x30))
                    *reinterpret_cast<UObject**>(b + AH::AICh_Schedule) = its->second;
                g_stashedSchedule.erase(its);
            }
        }
        // Twin: hand nav-type control back to her own AI (we pinned her to ground while she
        // followed as a bodyguard) and forget her dedicated controller state, so a released
        // Twin reverts to fully-native behaviour.
        if (IsMixedNavCharacter(ai))
            AllowMixedNavAuto(ai);
        g_twin.erase(ai);
        return ok;
    }

    bool ApplyAiBodyguard(UObject* ai, UObject* player, const FVector& playerLocHint)
    {
        if (!FollowPawnUsable(ai) || !Mem::IsReadable(player, 0x30) || ai == player)
            return false;

        // Baseline convert: friendly to the player (team 0, so it stops hitting you)
        // without ever storing the player in TargetAlly, and record its real team
        // so release can restore it. This
        // is only the DOCILE resting state -- team 0 does NOT make it hunt the robots
        // (team 0 is the game's passive/civilian faction). The per-frame squad pump
        // (InjectBodyguard) is what arms it: when a threat is near it parks the guard
        // on kGuardTeam (a combat team the robots read as hostile) and force-engages,
        // exactly like "enemies fight each other". The button just seeds the convert.
        bool ok = false;
        if (AiUsable(ai))
        {
            const bool hookOwned = g_hookBodyguardMode.load(std::memory_order_relaxed) &&
                                   IsHookBodyguard(ai);
            if (hookOwned) ClearAiAggressiveLatch(ai);
            else WriteAiAggressiveFlags(ai, true);
            ok = SetAiPassive(ai, false);
            ok = SwitchAiTeamFriendlyTo(ai, player) || ok;
            WriteAiTargetField(ai, nullptr);   // drop any aggro on the player
            SetAiTargetEnemy(ai, nullptr, true);
            // Make sure the BT isn't left in its attack branch from a prior state.
            if (UObject* ctrl = GetAiController(ai))
            {
                SetControllerTargetEnemy(ctrl, nullptr, true);
                SetControllerAggressive(ctrl, false);
            }
            if (!hookOwned)
            {
                // The normal squad path retains its existing one-time grounding.
                // Hook Diagnostics must not seed the legacy Twin movement state:
                // its native mixed-navigation transition owns 2D/3D selection.
                ForceGroundNavIfMixed(ai);
                g_twin.erase(ai);
            }
        }

        // Hook Diagnostics performs no movement here. Its 20 Hz native controller
        // acquires exactly one Mercuna/AIController request after this safe team and
        // target initialization, so recruitment cannot leave a legacy SDK request
        // queued underneath the ownership detours.
        const bool hookOwned = g_hookBodyguardMode.load(std::memory_order_relaxed) &&
                               IsHookBodyguard(ai);
        if (hookOwned)
            return ok;

        // Normal squad follow keeps its existing SDK behavior. Some callers pass
        // an empty hint (e.g. spawn fixup), so read a fresh player location.
        FVector playerLoc = playerLocHint;
        if (playerLoc.X == 0.0f && playerLoc.Y == 0.0f && playerLoc.Z == 0.0f)
            ReadActorLocationFast(player, playerLoc);
        bool followOk = DriveFollow(ai, player, playerLoc, false);
        return ok || followOk;
    }

    // "Fight each other": route to the all-bases deep injection (raw aggro write
    // + aggressive gates + game SetTargetEnemy + native SetCharacterAggressive),
    // forcing each actor onto a neighbour. `teamA` actors keep their REAL robot
    // team; the others move to kFightTeamB. The two groups then have different ids
    // (-> hits deal real damage) while BOTH stay hostile to the player (-> you can
    // still kill everyone). The old code put half the crowd on YOUR team, which
    // made them invincible to you; that is the bug this removes. One press holds
    // because the state-machine kick sticks; the continuous toggle re-asserts it.
    bool ApplyAiFight(UObject* ai, UObject* enemy, UObject* player, bool teamA)
    {
        // Fight-each-other is crowd control for NON-SQUAD enemies only. Letting this
        // path touch a guard overwrites its friendly team/target state and makes the
        // guard turn on the player when fight mode is later disabled.
        if (IsSquadMember(ai) || IsSquadMember(enemy))
            return false;
        // teamA -> leave team untouched (real robot team); else -> sentinel team B.
        bool ok = teamA ? InjectAttack(ai, enemy, nullptr, 0, -1)
                        : InjectAttack(ai, enemy, nullptr, 0, kFightTeamB);
        if (ok)
            g_fightParticipants.insert(ai);
        return ok;
    }

    // Ragdoll-launch an enemy skyward (spectacle). ACharacter::LaunchCharacter is
    // type-safe on an AiUsable-validated AHAICharacter (it derives from ACharacter).
    bool ApplyAiLaunch(UObject* ai)
    {
        UFunction* fn = CachedFn(AH::Fn_LaunchCharacter);
        if (!AiUsable(ai) || !fn)
            return false;

        float ang = (float)(GetTickCount64() % 628) / 100.0f; // pseudo-random spread
        struct { FVector Velocity; bool bXY; bool bZ; uint8_t pad[2]; } p{};
        p.Velocity = { cosf(ang) * 650.0f, sinf(ang) * 650.0f, 1900.0f };
        p.bXY = true;
        p.bZ = true;
        ai->ProcessEvent(fn, &p);
        return true;
    }

    // ---- spawn fixup: make a freshly spawned ally render + sit on the floor --
    // The "invisible body, only ESP sees it" symptom is a spawned pawn whose
    // mesh got culled by significance / left hidden, and the "spawns in the air
    // or inside geometry" one is no ground snap. We force both, repeatedly, over
    // the first few game frames so the fix wins even after construction finishes.
    void ForceActorVisible(UObject* actor)
    {
        if (!Mem::IsReadable(actor, 0x30))
            return;
        if (UFunction* fn = CachedFn(AH::Fn_SetActorHiddenInGame)) { P_SetActorHiddenInGame p{ false }; actor->ProcessEvent(fn, &p); }
        if (UFunction* fn = CachedFn(AH::Fn_SetActorTickEnabled))  { P_SetActorTickEnabled  p{ true  }; actor->ProcessEvent(fn, &p); }

        uint8_t* b = reinterpret_cast<uint8_t*>(actor);
        if (Mem::IsReadable(b + AH::AICh_bMeshTickEnabled, 1))  *reinterpret_cast<bool*>(b + AH::AICh_bMeshTickEnabled)  = true;
        if (Mem::IsReadable(b + AH::AICh_bActorTickEnabled, 1)) *reinterpret_cast<bool*>(b + AH::AICh_bActorTickEnabled) = true;

        if (Mem::IsReadable(b + AH::Char_Mesh, sizeof(void*)))
        {
            UObject* mesh = *reinterpret_cast<UObject**>(b + AH::Char_Mesh);
            if (Mem::IsReadable(mesh, 0x30))
            {
                if (UFunction* fn = CachedFn(AH::Fn_Comp_SetVisibility))   { P_SetVisibility   p{ true,  true }; mesh->ProcessEvent(fn, &p); }
                if (UFunction* fn = CachedFn(AH::Fn_Comp_SetHiddenInGame)) { P_SetHiddenInGame p{ false, true }; mesh->ProcessEvent(fn, &p); }
            }
        }
    }

    void SnapAiToGround(UObject* ai)
    {
        UFunction* fn = CachedClassFn(AH::Cls_AICharacter, "SnapCapsuleToGround");
        if (AiUsable(ai) && fn)
            ProcessNoParams(ai, fn);
    }

    // Clone source = the NEAREST live, healthy enemy's exact RUNTIME class. The
    // earlier "spawn crash" came from spawning the bare native preset class (e.g.
    // "AtomicHeart.TwinsCharacter") -- a base class with NO BP-configured mesh /
    // anim / abilities, so the engine AVs ticking it. A LIVE instance, by
    // contrast, is the fully-configured concrete class with all assets loaded, so
    // cloning ITS class reproduces a working, animated character. This is how you
    // get a working Twins: stand near the live Twins and clone it. Bosses can
    // still be unstable (encounter-singleton state), but this is the proper path.
    // Returns null (=> abort, never spawn a bare native class) if nothing live.
    UClass* NearestLiveEnemyClass()
    {
        std::vector<AiCachedActor> snap = CopyAiSnapshot(); // distance-sorted
        for (const AiCachedActor& e : snap)
        {
            if (!AiUsable(e.actor))
                continue;
            if (e.healthFrac == 0.0f) // skip the dead/dying
                continue;
            UObject* c = e.actor->Class();
            if (Mem::IsReadable(c, 0x30))
                return c;
        }
        return nullptr;
    }

    bool RefreshAiEntryFromIndex(const AiCachedActor& oldEntry, UClass* cls,
                                 const FVector& playerLoc, bool havePlayerLoc,
                                 AiCachedActor& out)
    {
        UObject* object = nullptr;
        if (oldEntry.index >= 0)
            object = GetObjectByIndex(oldEntry.index);
        if (object != oldEntry.actor)
            object = oldEntry.actor;

        if (!Mem::IsReadable(object, 0x30) || !object->IsA(cls))
            return false;

        FVector loc{};
        if (!ReadActorLocationFast(object, loc))
            return false;

        float cur = 0.0f, mx = 0.0f;
        float healthFrac = -1.0f;
        if (ReadCharacterHealth(object, cur, mx) && mx > 0.001f)
        {
            if (cur <= 0.0f)
                return false;
            healthFrac = cur / mx;
            if (healthFrac < 0.0f) healthFrac = 0.0f;
            if (healthFrac > 1.0f) healthFrac = 1.0f;
        }

        out = oldEntry;
        out.actor = object;
        out.location = loc;
        out.distanceM = havePlayerLoc ? DistanceMetres(loc, playerLoc) : oldEntry.distanceM;
        out.healthFrac = healthFrac;
        return true;
    }

    // Read a TArray<UObject*> field safely and append its readable elements.
    void AppendActorArray(uint8_t* owner, int fieldOffset, std::vector<UObject*>& out, int maxAppend)
    {
        if (!Mem::IsReadable(owner + fieldOffset, sizeof(TArray<void*>)))
            return;
        auto* arr = reinterpret_cast<TArray<UObject*>*>(owner + fieldOffset);
        int count = arr->Count;
        if (count <= 0 || count > kMaxActorsPerLevel)
            return;
        if (!Mem::IsReadable(arr->Data, (size_t)count * sizeof(void*)))
            return;
        for (int i = 0; i < count && (int)out.size() < maxAppend; ++i)
        {
            UObject* a = arr->Data[i];
            if (Mem::IsReadable(a, 0x30))
                out.push_back(a);
        }
    }

    // Gather live actors from the loaded levels (PersistentLevel + Levels[]),
    // instead of sweeping every UObject. This is the instant, no-hang path.
    void CollectLevelActors(std::vector<UObject*>& out)
    {
        UObject* world = GetWorld();
        uint8_t* w = reinterpret_cast<uint8_t*>(world);
        if (!Mem::IsReadable(w, Offsets::O_World_Levels + sizeof(TArray<void*>)))
            return;

        // Collect candidate levels: the persistent level plus the loaded-levels
        // array. Dedupe (the persistent level is usually also in Levels[]).
        std::vector<UObject*> levels;
        levels.reserve(64);
        if (Mem::IsReadable(w + Offsets::O_World_PersistentLevel, sizeof(void*)))
        {
            UObject* persistent = *reinterpret_cast<UObject**>(w + Offsets::O_World_PersistentLevel);
            if (Mem::IsReadable(persistent, 0x30))
                levels.push_back(persistent);
        }

        auto* levelArr = reinterpret_cast<TArray<UObject*>*>(w + Offsets::O_World_Levels);
        int levelCount = levelArr->Count;
        if (levelCount > 0 && levelCount <= kMaxScannedLevels &&
            Mem::IsReadable(levelArr->Data, (size_t)levelCount * sizeof(void*)))
        {
            for (int i = 0; i < levelCount; ++i)
            {
                UObject* lv = levelArr->Data[i];
                if (!Mem::IsReadable(lv, 0x30))
                    continue;
                bool dup = false;
                for (UObject* existing : levels)
                    if (existing == lv) { dup = true; break; }
                if (!dup)
                    levels.push_back(lv);
            }
        }

        for (UObject* lv : levels)
        {
            if ((int)out.size() >= kMaxScannedActors)
                break;
            AppendActorArray(reinterpret_cast<uint8_t*>(lv), Offsets::O_Level_Actors, out, kMaxScannedActors);
        }
    }

    // Incrementally discover AHAICharacter instances from the authoritative global
    // object array. Newly seen enemies become available immediately; stale entries
    // are pruned after each completed generation. `forceFull` is used by first-use
    // discovery so the initial scanner frame is complete rather than level-only.
    void RefreshGlobalAiDiscovery(UClass* cls, bool forceFull)
    {
        if (!Mem::IsReadable(cls, 0x30))
            return;
        int total = NumObjects();
        if (total <= 0)
            return;

        static UClass* memoForClass = nullptr;
        static std::unordered_map<UClass*, bool> classMemo;
        if (memoForClass != cls)
        {
            classMemo.clear();
            memoForClass = cls;
            g_globalAiSeen.clear();
            g_globalAiScanCursor = 0;
            g_globalAiScanGeneration = 0;
        }

        if (forceFull)
            g_globalAiScanCursor = 0;
        if (g_globalAiScanCursor == 0)
            ++g_globalAiScanGeneration;

        size_t start = g_globalAiScanCursor;
        size_t budget = forceFull ? (size_t)total : (size_t)kGlobalAiObjectsPerRefresh;
        size_t end = (std::min)((size_t)total, start + budget);
        for (size_t i = start; i < end; ++i)
        {
            UObject* o = GetObjectByIndex((int)i);
            if (!Mem::IsReadable(o, 0x30))
                continue;
            UClass* objectClass = static_cast<UClass*>(o->Class());
            if (!Mem::IsReadable(objectClass, 0x30))
                continue;
            bool isAi = false;
            auto it = classMemo.find(objectClass);
            if (it != classMemo.end())
                isAi = it->second;
            else
            {
                try { isAi = o->IsA(cls); } catch (...) { isAi = false; }
                classMemo[objectClass] = isAi;
            }
            if (isAi)
                g_globalAiSeen[o] = g_globalAiScanGeneration;
        }

        g_globalAiScanCursor = end;
        if (g_globalAiScanCursor >= (size_t)total)
        {
            for (auto it = g_globalAiSeen.begin(); it != g_globalAiSeen.end(); )
            {
                if (it->second != g_globalAiScanGeneration || !Mem::IsReadable(it->first, 0x30))
                    it = g_globalAiSeen.erase(it);
                else
                    ++it;
            }
            g_globalAiScanCursor = 0;
        }
    }

    const char* AiKindName(AiQueuedKind kind)
    {
        switch (kind)
        {
        case AiQueuedKind::Kill: return "kill";
        case AiQueuedKind::PassiveOn: return "passive";
        case AiQueuedKind::PassiveOff: return "aggressive";
        case AiQueuedKind::Follow: return "follow";
        case AiQueuedKind::FightEachOther: return "fight";
        case AiQueuedKind::Release: return "release";
        case AiQueuedKind::Launch: return "launch";
        default: return "none";
        }
    }

    int QueueAiOperation(AiQueuedKind kind, std::vector<UObject*> targets)
    {
        if (kind == AiQueuedKind::None)
            return 0;
        if (targets.empty())
        {
            RequestAiDiscovery();
            if (g_aiCachedCount.load() == 0)
            {
                // Nothing discovered yet -- remember the command and auto-fire it
                // (render thread) once the sweep finds enemies, so the button
                // works on a single press instead of needing a retry.
                g_aiDeferredKind = (int)kind;
                g_aiDeferredMs = GetTickCount64();
                LOG("AI command %s waiting: no cached targets yet, discovery requested (will auto-fire)", AiKindName(kind));
            }
            else
            {
                LOG("AI command %s: enemies cached but none within radius", AiKindName(kind));
            }
            return 0;
        }

        UObject* player = nullptr;
        FVector playerLoc{};
        ReadLocalPawnLocationFast(playerLoc, &player);

        std::lock_guard<std::mutex> lk(g_aiOpMutex);
        g_aiOp = {};
        g_aiOp.id = g_aiNextOpId++;
        g_aiOp.kind = kind;
        g_aiOp.targets = std::move(targets);
        g_aiOp.player = player;
        g_aiOp.playerLoc = playerLoc;
        g_aiPendingCount = (int)g_aiOp.targets.size();

        LOG("AI command queued: %s targets=%d player=%p",
            AiKindName(kind), (int)g_aiOp.targets.size(), (void*)player);
        return (int)g_aiOp.targets.size();
    }

    void DrainAiOperation()
    {
        AiQueuedOperation local{};
        size_t begin = 0;
        size_t end = 0;
        {
            std::lock_guard<std::mutex> lk(g_aiOpMutex);
            if (g_aiOp.kind == AiQueuedKind::None || g_aiOp.index >= g_aiOp.targets.size())
            {
                g_aiPendingCount = 0;
                return;
            }

            local = g_aiOp;
            begin = g_aiOp.index;
            end = std::min(g_aiOp.targets.size(), g_aiOp.index + (size_t)kAiOpsPerFrame);
            g_aiOp.index = end;
            g_aiPendingCount = (int)(g_aiOp.targets.size() - g_aiOp.index);
        }

        int applied = 0;
        for (size_t i = begin; i < end; ++i)
        {
            UObject* ai = local.targets[i];
            if (local.kind == AiQueuedKind::Follow)
            {
                if (!FollowPawnUsable(ai)) // actor may have died between queueing and now
                    continue;
            }
            else if (!AiUsable(ai)) // actor may have died between queueing and now
            {
                continue;
            }

            bool ok = false;
            try
            {
                switch (local.kind)
                {
                case AiQueuedKind::Kill:
                    ok = ApplyAiKill(ai);
                    break;
                case AiQueuedKind::PassiveOn:
                    ok = SetAiPassive(ai, true);
                    break;
                case AiQueuedKind::PassiveOff:
                    ok = SetAiPassive(ai, false);
                    break;
                case AiQueuedKind::Follow:
                    ok = ApplyAiBodyguard(ai, local.player, local.playerLoc);
                    if (ok)
                        SquadAdd(ai); // persistent follow: every pump keeps walking it to the player
                    break;
                case AiQueuedKind::FightEachOther:
                {
                    if (IsSquadMember(ai))
                        break;
                    // Pair each AI with a neighbour; the deep injection handles the
                    // team split + attack-state kick so the pair genuinely brawls.
                    // Alternate which team each joins so pairs are cross-team.
                    UObject* enemy = nullptr;
                    for (size_t step = 1; step < local.targets.size(); ++step)
                    {
                        UObject* candidate = local.targets[(i + step) % local.targets.size()];
                        if (candidate != ai && !IsSquadMember(candidate))
                        {
                            enemy = candidate;
                            break;
                        }
                    }
                    if (enemy)
                        ok = ApplyAiFight(ai, enemy, local.player, (i % 2 == 0));
                    break;
                }
                case AiQueuedKind::Release:
                {
                    bool hookOwned = IsHookBodyguard(ai);
                    if (hookOwned) ReleaseHookNativeMovement(ai, true);
                    ok = ApplyAiRelease(ai);
                    if (hookOwned)
                    {
                        SquadRemove(ai);
                        ForgetHookBodyguardRuntimeState(ai, !ok);
                    }
                    break;
                }
                case AiQueuedKind::Launch:
                    ok = ApplyAiLaunch(ai);
                    break;
                default:
                    break;
                }
            }
            catch (...) { ok = false; }

            if (ok)
                ++applied;
        }

        std::lock_guard<std::mutex> lk(g_aiOpMutex);
        if (g_aiOp.id != local.id)
            return;

        g_aiOp.applied += applied;
        if (g_aiOp.index >= g_aiOp.targets.size())
        {
            LOG("AI command complete: %s applied=%d/%d",
                AiKindName(g_aiOp.kind), g_aiOp.applied, (int)g_aiOp.targets.size());
            g_aiOp = {};
            g_aiPendingCount = 0;
        }
    }

    // Render thread: if a World AI command was pressed before discovery had any
    // enemies cached, fire it now that the worker thread has populated the list.
    // CollectNearbyAi / QueueAiOperation only read cached data + raw fields here,
    // so this stays on the render thread like every other ProcessEvent path.
    void DrainDeferredAiCommand()
    {
        int kindRaw = g_aiDeferredKind.load();
        if (kindRaw == (int)AiQueuedKind::None)
            return;

        ULONGLONG nowMs = GetTickCount64();
        if (nowMs - g_aiDeferredMs.load() > kAiDeferredCommandTtlMs)
        {
            g_aiDeferredKind = (int)AiQueuedKind::None;
            LOG("Deferred AI command expired before any enemies were found.");
            return;
        }

        if (g_aiCachedCount.load() <= 0)
            return; // discovery still running -- wait for it

        AiQueuedKind kind = (AiQueuedKind)kindRaw;
        bool requireTwo = (kind == AiQueuedKind::FightEachOther);
        std::vector<UObject*> targets = requireTwo
            ? CollectNearbyNonSquadAi(Features::Get().aiRadius, kMaxAiCommandTargets, true)
            : CollectNearbyAi(Features::Get().aiRadius, kMaxAiCommandTargets, false);
        if (targets.empty())
            return; // cached enemies exist but none in radius yet -- keep waiting until TTL

        g_aiDeferredKind = (int)AiQueuedKind::None;
        QueueAiOperation(kind, std::move(targets));
        LOG("Deferred AI command %s auto-fired after discovery", AiKindName(kind));
    }

    void UpdateAiAutoControls()
    {
        auto& st = Features::Get();

        // Falling edge of "Freeze nearby": auto-unfreeze so enemies don't stay
        // frozen forever (the memory freeze persists until restored). Pure guarded
        // writes -- no ProcessEvent -- so this can't crash.
        static bool wasFreeze = false;
        if (wasFreeze && !st.aiFreezeNearby)
        {
            std::vector<UObject*> targets = CollectNearbyAi(st.aiRadius, kMaxAiCommandTargets);
            int released = 0;
            for (UObject* ai : targets)
                if (AiUsable(ai) && SetActorTimeDilation(ai, 1.0f))
                    ++released;
            if (released > 0)
                LOG("AI auto-freeze off: unfroze %d nearby target(s)", released);
        }
        wasFreeze = st.aiFreezeNearby;

        if (!st.aiFreezeNearby)
            return;

        ULONGLONG nowMs = GetTickCount64();
        static ULONGLONG lastFreezeMs = 0;

        if (st.aiFreezeNearby && nowMs - lastFreezeMs >= 1000)
        {
            std::vector<UObject*> targets = CollectNearbyAi(st.aiRadius, kAiAutoOpsPerPass);
            int done = 0;
            for (UObject* ai : targets)
            {
                if (Mem::IsReadable(ai, 0x30) && ApplyAiFreeze(ai))
                    ++done;
            }
            if (done > 0)
                LOG("AI auto-freeze refreshed %d target(s)", done);
            lastFreezeMs = nowMs;
        }
    }

    // ---- persistent deep combat injection (render thread) -----------------
    // Re-asserts the full aggro/team/attack-state pipeline every ~150ms on a
    // rotating, bounded window so a large crowd never stalls Present. This is
    // what makes "fight each other" and "bodyguard" actually STICK instead of
    // being reverted by the AI's own perception a frame later.
    constexpr int       kAiInjectPerTick   = 10;   // FIGHT actors fully driven per pass (bounded game-thread work)
    constexpr int       kAiGuardPerTick    = 32;   // GUARDS driven per pass -- higher so every guard follows/protects each pass (you rarely recruit more), which fixes "rarely follows"
    constexpr ULONGLONG kAiInjectIntervalMs = 200;  // ~5Hz re-assert

    // parity is derived from the actor POINTER (not the sort index) so each
    // enemy's fight team is stable as the crowd moves and re-sorts -- otherwise
    // their team would flip every tick and thrash the SwitchTeam call.
    struct InjNode { UObject* actor; FVector loc; bool ok; int parity; };

    void BuildInjNodes(const std::vector<UObject*>& src, std::vector<InjNode>& out)
    {
        out.clear();
        out.reserve(src.size());
        for (UObject* a : src)
        {
            InjNode n{ a, {}, false, (int)(((uintptr_t)a >> 4) & 1) };
            if (AiUsable(a))
                n.ok = ReadActorLocationFast(a, n.loc);
            out.push_back(n);
        }
    }

    // Nearest node in `pool` to `from`, optionally requiring a given team parity
    // (for the fight split). Returns nullptr if none qualify.
    UObject* NearestNode(const std::vector<InjNode>& pool, const FVector& from,
                         UObject* exclude, int wantParityOrNeg, UObject* excludeSelf)
    {
        UObject* best = nullptr;
        float bestD = 3.4e38f;
        for (const InjNode& n : pool)
        {
            if (!n.ok || n.actor == exclude || n.actor == excludeSelf) continue;
            if (wantParityOrNeg >= 0 && n.parity != wantParityOrNeg) continue;
            float dx = n.loc.X - from.X, dy = n.loc.Y - from.Y, dz = n.loc.Z - from.Z;
            float d = dx * dx + dy * dy + dz * dz;
            if (d < bestD) { bestD = d; best = n.actor; }
        }
        return best;
    }

    // Hook Debug 360-degree perception: every cached live combat AI is considered,
    // independent of the guard's facing. Proximity is intentionally narrow so a
    // distant idle robot does not drag the bodyguard into combat across the level.
    UObject* SelectHookThreat360(const std::vector<InjNode>& pool, UObject* guard,
                                 const FVector& guardLoc, UObject* player,
                                 const FVector& playerLoc, float* outDistance = nullptr)
    {
        UObject* best = nullptr;
        float bestScore = 3.4e38f;
        float bestDistance = -1.0f;
        for (const InjNode& n : pool)
        {
            if (!n.ok || n.actor == guard || n.actor == player || IsHookBodyguard(n.actor) ||
                !IsLiveCombatTarget(n.actor) || !AiIsCombatCapable(n.actor))
                continue;
            ULONGLONG now = GetTickCount64();
            auto suppressed = g_hookSuppressedThreatUntilMs.find(n.actor);
            if (suppressed != g_hookSuppressedThreatUntilMs.end())
            {
                if (suppressed->second > now)
                    continue;
                g_hookSuppressedThreatUntilMs.erase(suppressed);
            }
            float dPlayer = DistanceMetres(n.loc, playerLoc);
            float dGuard = DistanceMetres(n.loc, guardLoc);
            UObject* currentTarget = ReadAiTargetField(n.actor);
            bool attacksProtected = currentTarget == player ||
                                    (currentTarget && IsHookBodyguard(currentTarget));
            // Strict bodyguard policy: do NOT chase robots just because they are near.
            // Hook bodyguards protect the player, they don't initiate fights. A robot is
            // eligible only while its current AI target is the player / managed guard,
            // and only inside a bounded bubble so stale far-away perception can't pull
            // the Twin back to a corpse or old fight spot.
            bool eligible = attacksProtected && std::min(dPlayer, dGuard) <= kHookAttackerAwarenessM;
            if (!eligible)
                continue;
            float nearestProtected = dPlayer;
            float score = nearestProtected - (attacksProtected ? 1000.0f : 0.0f);
            if (score < bestScore)
            {
                bestScore = score;
                bestDistance = nearestProtected;
                best = n.actor;
            }
        }
        if (outDistance) *outDistance = bestDistance;
        return best;
    }

    void ReleaseFightParticipants(const char* why)
    {
        int n = 0;
        std::vector<UObject*> participants(g_fightParticipants.begin(), g_fightParticipants.end());
        g_fightParticipants.clear();
        for (UObject* ai : participants)
        {
            if (!AiUsable(ai))
            {
                g_origTeam.erase(ai);
                g_inject.erase(ai);
                continue;
            }
            if (IsSquadMember(ai))
                continue; // bodyguard ownership wins unconditionally

            // Restore the exact pre-fight team instead of forcing a generic hostile
            // state, then clear only fight-owned target/aggression state.
            RestoreOriginalTeam(ai);
            SetAiPassive(ai, false);
            SetAiTargetAlly(ai, nullptr);
            SetAiTargetEnemy(ai, nullptr, true);
            WriteAiTargetField(ai, nullptr);
            StopCharacterAggressive(ai);
            if (UObject* ctrl = GetAiController(ai))
            {
                SetControllerTargetEnemy(ctrl, nullptr, true);
                SetControllerAggressive(ctrl, false);
            }
            g_inject.erase(ai);
            ++n;
        }
        LOG("AI %s: restored %d fight participant(s); squad untouched", why, n);
    }

    void UpdateAiCombatInjection()
    {
        auto& st = Features::Get();

        // Falling edge: stand the brawlers down + forget their state. (Bodyguard
        // auto-recruit is gone -- the squad roster replaces it; squad members are
        // driven by DriveSpawnedAllies, released explicitly via the roster.)
        static bool wasFight = false;
        if (wasFight && !st.aiFightEachOther) ReleaseFightParticipants("fight injection off");
        wasFight = st.aiFightEachOther;

        if (!st.aiFightEachOther)
            return;

        static ULONGLONG lastMs = 0;
        ULONGLONG now = GetTickCount64();
        if (now - lastMs < kAiInjectIntervalMs)
            return;
        lastMs = now;

        // Keep the per-actor throttle map from growing without bound (dead actor
        // pointers leak slowly otherwise); resetting only re-arms the heartbeat.
        if (g_inject.size() > 256)
            g_inject.clear();

        UObject* player = nullptr;
        FVector  playerLoc{};
        if (!ReadLocalPawnLocationFast(playerLoc, &player) || !Mem::IsReadable(player, 0x30))
            return;

        {
            std::vector<UObject*> set = CollectNearbyNonSquadAi(st.aiRadius, kMaxAiCommandTargets, true);
            if (set.size() < 2)
                return;

            std::vector<InjNode> nodes;
            BuildInjNodes(set, nodes);

            static size_t cursor = 0;
            size_t count = nodes.size();
            if (cursor >= count) cursor = 0;
            int take = (int)std::min(count, (size_t)kAiInjectPerTick);
            int applied = 0;
            for (int k = 0; k < take; ++k)
            {
                const InjNode& self = nodes[(cursor + (size_t)k) % count];
                if (!self.ok || self.actor == player) continue;
                // Two stable groups by pointer parity; attack the nearest member of
                // the OTHER group. parity 0 keeps its REAL robot team, parity 1 is
                // moved to kFightTeamB. Different ids => hits deal real damage, and
                // NEITHER is your team, so you can still kill everyone -- no robot is
                // ever parked on your team / made invincible (that was the old bug).
                int wantParity = 1 - self.parity;
                UObject* enemy = NearestNode(nodes, self.loc, player, wantParity, self.actor);
                if (!enemy) // no opposite-group member nearby: hit the nearest other robot
                    enemy = NearestNode(nodes, self.loc, player, -1, self.actor);
                int forceTeam = (self.parity == 0) ? -1 : (int)kFightTeamB;
                try { if (enemy && InjectAttack(self.actor, enemy, nullptr, 0, forceTeam)) ++applied; }
                catch (...) {} // one stale actor must not abort the whole pass
            }
            cursor = (cursor + (size_t)take) % count;
            static ULONGLONG lastLog = 0;
            if (now - lastLog > 4000) { lastLog = now; LOG("Fight injection: nearby=%d driven=%d", (int)nodes.size(), applied); }
        }
    }

    // Drive THE SQUAD (spawned + recruited) as permanent bodyguards -- fight the
    // nearest non-squad threat + follow you -- with no toggle, so a member protects
    // and follows the instant it joins. Prunes dead members. Game thread (pump).
    void DriveSpawnedAllies()
    {
        // Snapshot + prune under the lock, then drive the copy unlocked (so we never
        // hold the lock across ProcessEvent).
        //
        // CRITICAL: only drop a member that is TRULY GONE (pointer unreadable) or that
        // has failed for many CONSECUTIVE drives. A recruited robot's location/root
        // read fails transiently during streaming / significance changes; the old code
        // dropped it on the FIRST miss -> nothing kept it friendly anymore -> it reverted
        // to an enemy and started attacking the player ("my own bots attacked me after
        // they vanished from the list"). The miss-counter keeps a hitching guard in the
        // squad so it stays converted.
        static std::unordered_map<UObject*, int> miss; // consecutive transient misses (game thread)
        std::vector<UObject*> squad;
        {
            std::lock_guard<std::mutex> lk(g_squadMutex);
            g_spawnedAllies.erase(
                std::remove_if(g_spawnedAllies.begin(), g_spawnedAllies.end(),
                    [](UObject* a)
                    {
                        if (!Mem::IsReadable(a, 0x30)) { miss.erase(a); LOG("Squad prune: member %p gone (unreadable) -> dropped", (void*)a); return true; } // truly gone
                        if (FollowPawnUsable(a)) { miss.erase(a); return false; }       // healthy -> keep
                        if (++miss[a] >= 30) { LOG("Squad prune: member %p dropped after 30 misses (FollowPawnUsable=false)", (void*)a); miss.erase(a); return true; }
                        return false; // ~30 consecutive misses (~several s) -> give up; else KEEP
                    }),
                g_spawnedAllies.end());
            g_spawnedAllyCount = (int)g_spawnedAllies.size();
            squad = g_spawnedAllies;
        }
        if (squad.empty())
            return;

        static ULONGLONG lastMs = 0;
        ULONGLONG now = GetTickCount64();
        if (now - lastMs < kAiInjectIntervalMs)
            return;
        lastMs = now;

        UObject* player = nullptr;
        FVector  playerLoc{};
        if (!ReadLocalPawnLocationFast(playerLoc, &player) || !Mem::IsReadable(player, 0x30))
            return;

        // Threats = cached enemies that are NOT squad members AND are still ALIVE.
        // Excluding the dead is the fix for "the guard keeps fighting the same spot after
        // the enemy dies": a corpse lingers in the cache for a refresh or two, and if we
        // still count it as a threat the guard keeps attacking where it fell.
        std::vector<UObject*> allAi = CollectAllCachedAi(kMaxCachedAiActors);
        std::vector<UObject*> threatSrc;
        threatSrc.reserve(allAi.size());
        for (UObject* a : allAi)
        {
            bool member = false;
            for (UObject* s : squad) if (s == a) { member = true; break; }
            if (member) continue;
            float cur = 0.0f, mx = 0.0f;
            if (ReadCharacterHealth(a, cur, mx) && mx > 0.001f && cur <= 0.0f)
                continue; // dead/dying -> not a threat
            threatSrc.push_back(a);
        }
        std::vector<InjNode> threats;
        BuildInjNodes(threatSrc, threats);

        const bool invincible = Features::Get().aiInvincibleAllies;
        const bool squadAggr  = Features::Get().aiSquadAggressive;
        int dbgDriven = 0, dbgEngaged = 0; float dbgNearThreat = -1.0f; uint8_t dbgTeam = 255;
        for (UObject* ally : squad)
        {
            if (!FollowPawnUsable(ally) || ally == player)
                continue;
            const bool hookOwned = g_hookBodyguardMode.load(std::memory_order_relaxed) &&
                                   IsHookBodyguard(ally);
            // Twins are driven by their OWN dedicated brain (DriveTwinCombat) + per-frame
            // follow -- never the normal squad combat/follow path. Keep her completely
            // separate (no ground-nav hammering, no InjectBodyguard, no assist-kill here).
            if (IsMixedNavCharacter(ally) && !hookOwned)
                continue;
            FVector loc{};
            if (!ReadActorLocationFast(ally, loc))
                continue;
            ++dbgDriven;
            // Legacy squad members keep their old grounding heartbeat. Hook Twins
            // must never enter it: their ReVa-derived mixed-nav state owns this field.
            if (!hookOwned)
            {
                static std::unordered_map<UObject*, ULONGLONG> groundNavMs; // game thread
                ULONGLONG& gnm = groundNavMs[ally];
                if (now - gnm > 1500) { gnm = now; ForceGroundNavIfMixed(ally); }
                if (groundNavMs.size() > 64)
                    for (auto it = groundNavMs.begin(); it != groundNavMs.end(); )
                    { if (Mem::IsReadable(it->first, 0x30)) ++it; else it = groundNavMs.erase(it); }
            }
            // Defence priority: an enemy currently targeting the player always wins.
            // Otherwise choose the nearest threat to the guard as before.
            UObject* threat = nullptr;
            float bestAttackerD = 3.4e38f;
            if (hookOwned)
            {
                InjectState& hs = g_inject[ally];
                if (IsLiveCombatTarget(hs.target))
                {
                    auto sup = g_hookSuppressedThreatUntilMs.find(hs.target);
                    if (sup != g_hookSuppressedThreatUntilMs.end() && sup->second > now)
                    {
                        hs.target = nullptr;
                    }
                    else
                    {
                    FVector retainedLoc{};
                    if (ReadActorLocationFast(hs.target, retainedLoc))
                    {
                        float dPlayer = DistanceMetres(playerLoc, retainedLoc);
                        float dGuard = DistanceMetres(loc, retainedLoc);
                        UObject* tgtTarget = ReadAiTargetField(hs.target);
                        bool attacksProtected = tgtTarget == player || (tgtTarget && IsHookBodyguard(tgtTarget));
                        if (attacksProtected && std::min(dPlayer, dGuard) <= kHookAttackerAwarenessM)
                            threat = hs.target;
                    }
                    }
                }
                if (!threat)
                    threat = SelectHookThreat360(threats, ally, loc, player, playerLoc, &bestAttackerD);
            }
            else
            {
                for (const InjNode& candidate : threats)
                {
                    if (!candidate.ok || candidate.actor == ally || candidate.actor == player)
                        continue;
                    uint8_t* cb = reinterpret_cast<uint8_t*>(candidate.actor);
                    bool attacksPlayer =
                        Mem::IsReadable(cb + AH::AICh_CachedTargetEnemy, sizeof(void*)) &&
                        *reinterpret_cast<UObject**>(cb + AH::AICh_CachedTargetEnemy) == player;
                    if (!attacksPlayer)
                        continue;
                    float d = DistanceMetres(candidate.loc, playerLoc);
                    if (d < bestAttackerD) { bestAttackerD = d; threat = candidate.actor; }
                }
                if (!threat)
                    threat = NearestNode(threats, loc, player, -1, nullptr);
            }
            // Diagnostics: nearest threat distance to this guard + whether it's engaged.
            if (threat)
            {
                FVector tl{};
                if (ReadActorLocationFast(threat, tl))
                {
                    float td = DistanceMetres(loc, tl);
                    if (dbgNearThreat < 0.0f || td < dbgNearThreat) dbgNearThreat = td;
                }
            }
            {
                auto eit = g_engagedUntilMs.find(ally);
                if (eit != g_engagedUntilMs.end() && eit->second > now) ++dbgEngaged;
                // Read team only for the first driven member (one ProcessEvent/pump, diag only).
                if (dbgTeam == 255) { uint8_t tid = 0; if (ReadAiTeamId(ally, tid)) dbgTeam = tid; }
            }
            try
            {
                InjectBodyguard(ally, player, playerLoc, threat);
                if (invincible && AiUsable(ally)) SetCharacterHealthFull(ally); // keep combat AI alive
                // Obliterate: guards deal massively boosted damage so they shred the robots
                // attacking you. Applied to ANY usable squad member (raw attribute write,
                // crash-safe) -- gating this on the BT heuristic skipped mis-flagged combat
                // units like the Twins; a true civilian that never swings just never uses it.
                if (AiUsable(ally)) SetCharacterInstigatedDamage(ally, hookOwned ? 10000.0f : 100.0f);

                // ASSIST-KILL (user request: "trigger a kill event on the enemy it's
                // targeting, for speed"). A guard's own swings are slow/unreliable, so when
                // its threat is a real combat enemy the guard has CLOSED ON (<=12m) OR that
                // is actively ATTACKING YOU, finish it directly -> robots die fast even when
                // several pile on. Non-combat NPCs (civilians/Larisa) are NEVER touched, and
                // it's gated on aiSquadAggressive so "peaceful followers" stays peaceful.
                if (!hookOwned && Features::Get().aiSquadAggressive && threat && threat != player
                    && AiUsable(threat) && AiIsCombatCapable(threat))
                {
                    FVector tl{};
                    bool closed = ReadActorLocationFast(threat, tl) && DistanceMetres(loc, tl) <= 12.0f;
                    bool attackingPlayer = false;
                    uint8_t* tb = reinterpret_cast<uint8_t*>(threat);
                    if (Mem::IsReadable(tb + AH::AICh_CachedTargetEnemy, sizeof(void*)))
                        attackingPlayer = (*reinterpret_cast<UObject**>(tb + AH::AICh_CachedTargetEnemy) == player);
                    if (closed || attackingPlayer)
                    {
                        ApplyAiKill(threat);
                        // Target is dead -> only clear the guard's stale pointer to it so it
                        // re-acquires a FRESH threat next pump. KEEP IT ARMED: do NOT switch
                        // it friendly / mark idle / drop the engaged stamp here. Doing that
                        // every pump (whenever any attacker was in range) is exactly what
                        // reset its combat state before it could sustain a fight -- the
                        // "never engages" bug. The combat-hold carries it to the next enemy.
                        WriteAiTargetField(ally, nullptr);
                        SetAiTargetEnemy(ally, nullptr, true);
                        if (UObject* ctrl = GetAiController(ally))
                            SetControllerTargetEnemy(ctrl, nullptr, true);
                    }
                }
            }
            catch (...) {} // one stale member must not abort the rest
        }

        // Throttled combat heartbeat so the fight state is observable in the log:
        // engaged>0 + team=230 = a guard is actively fighting on the guard team. If
        // engaged stays 0 while threats are near, aiSquadAggressive is probably OFF
        // (the "Squad fights for you" checkbox) or no live threat is within kGuardEngageM.
        static ULONGLONG lastCombatLog = 0;
        if (now - lastCombatLog > 3000)
        {
            lastCombatLog = now;
            LOG("SquadCombat(GT): driven=%d engaged=%d squadAggr=%d nearestThreat=%.1fm threats=%d team=%d",
                dbgDriven, dbgEngaged, squadAggr ? 1 : 0,
                dbgNearThreat, (int)threats.size(), (int)dbgTeam);
        }
    }

    // =======================================================================
    //  TWIN COMBAT BRAIN  --  ~5 Hz, game thread (called from the AI pump)
    // -----------------------------------------------------------------------
    //  The "decide + seed" half of the Twin super-bodyguard. Picks a threat, seeds
    //  combat exactly like a native enemy (target + aggressive + combat team), kicks
    //  the attack state machine ONLY on a target change or a slow heartbeat (re-kicking
    //  every pump is what looped her montages), hands nav control back so she can fly,
    //  then stamps engagedUntil so the per-frame follow LEAVES HER ALONE to fight with
    //  her own smooth locomotion + montages. When safe she stands down to a docile follow.
    //  She NEVER targets you (blanked every pump, both here and in the per-frame drive).
    void DriveTwinCombat()
    {
        if (g_spawnedAllyCount.load() <= 0)
            return;

        static ULONGLONG lastMs = 0;
        ULONGLONG now = GetTickCount64();
        if (now - lastMs < kAiInjectIntervalMs) // ~5 Hz, same cadence as the squad pump
            return;
        lastMs = now;

        std::vector<UObject*> squad;
        { std::lock_guard<std::mutex> lk(g_squadMutex); squad = g_spawnedAllies; }

        std::vector<UObject*> twins;
        for (UObject* a : squad)
            if (Mem::IsReadable(a, 0x30) && IsMixedNavCharacter(a) &&
                !(g_hookBodyguardMode.load(std::memory_order_relaxed) && IsHookBodyguard(a)))
                twins.push_back(a);
        if (twins.empty())
            return;

        UObject* player = nullptr;
        FVector  playerLoc{};
        if (!ReadLocalPawnLocationFast(playerLoc, &player) || !Mem::IsReadable(player, 0x30))
            return;

        const bool squadAggr = Features::Get().aiSquadAggressive;
        const bool invincible = Features::Get().aiInvincibleAllies;

        // Live, non-squad, COMBAT threats (same definition the squad pump uses). Built once
        // for all Twins. Skipped entirely when "squad fights for you" is off -> peaceful Twin.
        std::vector<UObject*> threats;
        if (squadAggr)
        {
            std::vector<UObject*> allAi = CollectAllCachedAi(kMaxCachedAiActors);
            threats.reserve(allAi.size());
            for (UObject* a : allAi)
            {
                if (!Mem::IsReadable(a, 0x30) || !AiUsable(a) || !AiIsCombatCapable(a))
                    continue;
                bool member = false;
                for (UObject* s : squad) if (s == a) { member = true; break; }
                if (member)
                    continue;
                float cur = 0.0f, mx = 0.0f;
                if (ReadCharacterHealth(a, cur, mx) && mx > 0.001f && cur <= 0.0f)
                    continue; // dead/dying -> not a threat
                threats.push_back(a);
            }
        }

        float dbgDist = -1.0f, dbgThreatDist = -1.0f, dbgNearestAny = -1.0f;
        int   dbgEngage = 0, dbgPhase = -9, dbgAir = 0, dbgMerc = -1, dbgMercCount = -1;
        for (UObject* twin : twins)
        {
            if (!FollowPawnUsable(twin) || twin == player)
                continue;
            TwinState& s = g_twin[twin];
            FVector twinLoc{};
            if (!ReadActorLocationFast(twin, twinLoc))
                continue;

            // Keep her alive + her REAL hits lethal (so her own attack montage one-shots a
            // robot -> smooth kills, no instant-kill that would cut the animation short).
            EnsureFollowerCanMove(twin);
            if (invincible && AiUsable(twin)) SetCharacterHealthFull(twin);
            if (AiUsable(twin))               SetCharacterInstigatedDamage(twin, 100.0f);
            // (No ground-nav pinning -- her own AI decides nav type. Mercuna does the moving.)

            // HARD never-attack-you: she is always your ally; blank any aggro on you.
            WriteAiAggressiveFlags(twin, true);
            SuppressGuardTargetingPlayer(twin, player);

            // Threat selection: prefer an enemy attacking YOU, else the nearest to HER, and
            // only within engage range of you OR her. (A distant fight is ignored so she keeps
            // following instead of running off.)
            UObject* threat = nullptr;
            float best = 3.4e38f;
            float selDist = -1.0f;       // distance (m) twin->selected threat (diag)
            float nearestAnyM = -1.0f;   // nearest combat AI to her at ALL (diag: why no engage)
            for (UObject* c : threats)
            {
                if (c == twin || c == player)
                    continue;
                FVector cl{};
                if (!ReadActorLocationFast(c, cl))
                    continue;
                float dYou = DistanceMetres(playerLoc, cl);
                float dHer = DistanceMetres(twinLoc, cl);
                if (nearestAnyM < 0.0f || dHer < nearestAnyM) nearestAnyM = dHer;
                if (dYou > kTwinEngageM && dHer > kTwinEngageM)
                    continue;
                uint8_t* cb = reinterpret_cast<uint8_t*>(c);
                bool atkYou = Mem::IsReadable(cb + AH::AICh_CachedTargetEnemy, sizeof(void*)) &&
                              *reinterpret_cast<UObject**>(cb + AH::AICh_CachedTargetEnemy) == player;
                float score = dHer - (atkYou ? 1.0e4f : 0.0f); // attackers of you win
                if (score < best) { best = score; threat = c; selDist = dHer; }
            }

            bool engage = (threat != nullptr);
            // Diagnostics for the LAST twin this pump (usually the only one).
            dbgDist       = DistanceMetres(twinLoc, playerLoc);
            dbgEngage     = engage ? 1 : 0;
            dbgThreatDist = selDist;
            dbgNearestAny = nearestAnyM;
            dbgPhase      = s.combatPhase;
            dbgAir        = TwinIsAirborne(twin) ? 1 : 0;
            dbgMercCount  = s.mercCount;
            if (engage)
                s.lastThreatMs = now;
            bool combatHold = (now - s.lastThreatMs) < kTwinCombatHoldMs;

            if (engage)
            {
                bool targetChanged = (s.target != threat);
                // (Nav is left to her own AI now -- no ground pinning. Mercuna moves her to the
                //  threat below; her AI picks ground vs flight.)
                // Seed combat like a native enemy: raw target (every pump, keeps her locked on)
                // + game/blackboard target + aggressive branch.
                WriteAiTargetField(twin, threat);
                SetAiTargetEnemy(twin, threat, true);
                if (UObject* ctrl = GetAiController(twin))
                {
                    SetControllerTargetEnemy(ctrl, threat, true);
                    SetControllerAggressive(ctrl, true);
                }
                // Combat team: SetGenericTeamId re-triggers perception, so throttle it.
                if (targetChanged || now - s.lastTeamMs > 1500)
                {
                    SetAiTeamIdTracked(twin, kGuardTeam);
                    s.lastTeamMs = now;
                }
                // *** MOVE her to the threat with Mercuna. ***
                // Her boss BT's own combat locomotion does NOT move her under our injection (she
                // just stood "engaged" = frozen). Frequent MoveToActor re-targeting is what moves
                // her; a single order doesn't sustain her. MoveToActor STACKS path requests over
                // time (lag grows -> stall), so every few seconds we FLUSH with a STANDALONE Stop
                // on its own pump -- NOT in the same pump as a Move (Stop+Move together cancels
                // the move and she freezes, confirmed). Between flushes we re-target on a timer.
                if (now - s.lastFlushMs > 3000)
                {
                    MercunaStop(twin);          // flush accumulated path requests (own pump)
                    s.lastFlushMs = now;
                }
                else if (now - s.lastGoalMs > 400)
                {
                    dbgMerc = MercunaMoveToPlayer(twin, threat, 250.0f /*~2.5m, melee range*/, 600.0f) ? 1 : 0;
                    s.lastGoalMs = now; ++s.mercCount;
                }
                // ATTACK kick: on target change, then re-assert only while she's IN melee range
                // so she actually swings -- throttled so the montage isn't restarted every pump
                // (that restart was the looping-animation bug). No kick while merely approaching.
                bool inRange = (selDist >= 0.0f && selDist <= 5.0f);
                if (targetChanged || (inRange && now - s.lastKickMs > kTwinKickHeartbeatMs))
                {
                    ForceCharacterAggressive(twin, threat);
                    s.lastKickMs = now;
                }
                SuppressGuardTargetingPlayer(twin, player);
                s.engagedUntilMs = now + 1200; // per-frame follow keeps its hands off while she fights
                s.target = threat;
                s.combatPhase = 1;
            }
            else if (combatHold && s.combatPhase == 1)
            {
                // Brief gap with no fresh target: HOLD (stay armed, don't reset team/state every
                // pump -- that flip-flop is what stopped a guard ever sustaining a fight). Let her
                // own perception keep hunting; just keep the follow drive off her for a moment.
                s.engagedUntilMs = now + 400;
            }
            else
            {
                // Genuinely safe -> stand down to a docile, smooth follow. Un-latch the attack
                // state ONCE on the transition, drop onto the player's friendly team, and let the
                // per-frame follow take over (engagedUntil cleared).
                if (s.combatPhase != 0)
                {
                    StopCharacterAggressive(twin);
                    MercunaStop(twin); // clear her combat move so the follow can re-issue cleanly
                    WriteAiTargetField(twin, nullptr);
                    SetAiTargetEnemy(twin, nullptr, true);
                    if (UObject* ctrl = GetAiController(twin))
                    {
                        SetControllerTargetEnemy(ctrl, nullptr, true);
                        SetControllerAggressive(ctrl, false);
                    }
                    SwitchAiTeamFriendlyTo(twin, player);
                    s.lastFriendlyMs = now;
                    s.combatPhase = 0;
                }
                else if (now - s.lastFriendlyMs > 1000)
                {
                    SwitchAiTeamFriendlyTo(twin, player);
                    s.lastFriendlyMs = now;
                }
                s.target = nullptr;
                s.engagedUntilMs = 0;
            }
        }

        static ULONGLONG lastTwinLog = 0;
        if (now - lastTwinLog > 3000)
        {
            lastTwinLog = now;
            LOG("TwinCombat: twins=%zu threats=%zu dist=%.1fm engage=%d threatDist=%.1fm nearestAny=%.1fm phase=%d air=%d merc=%d issues=%d aggr=%d",
                twins.size(), threats.size(), dbgDist, dbgEngage, dbgThreatDist, dbgNearestAny,
                dbgPhase, dbgAir, dbgMerc, dbgMercCount, squadAggr ? 1 : 0);
        }

        // Prune dead Twins from the state map (bounded).
        if (g_twin.size() > 32)
            for (auto it = g_twin.begin(); it != g_twin.end(); )
            { if (Mem::IsReadable(it->first, 0x30)) ++it; else it = g_twin.erase(it); }
    }

    // Register a freshly spawned ally into the squad (locked, dedup + cap).

    // =======================================================================
    //  SHARED SPAWN  --  clone a class into a permanent bodyguard (game thread)
    // -----------------------------------------------------------------------
    //  Both "spawn nearest enemy" and "spawn a saved character" funnel here so
    //  the spawn/fixup/register logic lives in ONE place. Caller resolves the
    //  UClass (nearest live enemy, or a saved class re-resolved by name).
    // =======================================================================
    void SpawnAndRegisterAlly(UClass* pawnClass, bool hookOwned = false)
    {
        if (!Mem::IsReadable(pawnClass, 0x30))
        {
            LOG("SpawnAndRegisterAlly: pawn class null/not loaded -- cannot spawn");
            return;
        }
        UObject* lib = CachedObject(AH::Obj_AIBlueprintHelper);
        UFunction* fn = CachedClassFn(AH::Cls_AIBlueprintHelper, "SpawnAIFromClass");
        if (!Mem::IsReadable(lib, 0x30) || !fn)
        {
            LOG("SpawnAndRegisterAlly: lib=%p fn=%p", (void*)lib, (void*)fn);
            return;
        }

        UObject* player = nullptr;
        FVector playerLoc{};
        if (!ReadLocalPawnLocationFast(playerLoc, &player) || !Mem::IsReadable(player, 0x30))
        {
            LOG("SpawnAndRegisterAlly: no local player pawn");
            return;
        }
        UObject* world = GetWorld();
        if (!Mem::IsReadable(world, 0x30))
        {
            LOG("SpawnAndRegisterAlly: no world context");
            return;
        }

        FRotator viewRot{};
        GetControlRotation(viewRot);
        const float yaw = viewRot.Yaw * 0.01745329251994329577f;
        FVector spawnLoc = playerLoc;
        spawnLoc.X += cosf(yaw) * 250.0f;
        spawnLoc.Y += sinf(yaw) * 250.0f;
        spawnLoc.Z += 90.0f; // a little high; SnapCapsuleToGround drops it onto the floor

        P_SpawnAIFromClass p{};
        p.WorldContextObject = world;
        p.PawnClass = pawnClass;
        p.BehaviorTree = nullptr; // the AHAIController runs the pawn's own BT on possess
        p.Location = spawnLoc;
        p.Rotation = { 0.0f, viewRot.Yaw, 0.0f };
        p.bNoCollisionFail = true;
        p.Owner = player;

        LOG("SpawnAndRegisterAlly: calling SpawnAIFromClass cls=%p ...", (void*)pawnClass);
        lib->ProcessEvent(fn, &p);
        UObject* spawned = static_cast<UObject*>(p.ReturnValue);
        if (!Mem::IsReadable(spawned, 0x30))
        {
            LOG("SpawnAndRegisterAlly: SpawnAIFromClass returned null");
            return;
        }
        LOG("SpawnAndRegisterAlly: SpawnAIFromClass OK actor=%p; registering + fixup", (void*)spawned);

        // Register FIRST (so the squad tracks it and Stand-down can remove it even if
        // the fixup throws), then run the fixup. Fixup is best-effort and self-guarded.
        RequestAiDiscovery();
        LOG("SpawnAndRegisterAlly: spawned ally actor=%p", (void*)spawned);
    }

    // =======================================================================
    //  STREAMED SPAWN QUEUE  --  spawns trickle in one at a time, never freeze
    // -----------------------------------------------------------------------
    //  The old path queued the heavy SpawnAIFromClass straight onto the game
    //  thread, so asking for several copies (or mashing the button) ran several
    //  full actor constructions inside one game-thread borrow = a visible hitch
    //  or, with a bad class, a stall. Instead, every spawn request lands in this
    //  queue; the AI pump pops at most ONE per kSpawnIntervalMs and constructs it.
    //  Result: copies appear one-by-one, smoothly, exactly as asked ("streamed").
    //  A request carries one of three resolvers, tried on the game thread right
    //  before the spawn so we never hold a stale class:
    //    * cls         -- a live, loaded class pointer (from the model dropdown)
    //    * path        -- a saved class path to re-resolve via FindObject
    //    * cloneNearest-- clone the nearest live enemy's runtime class
    //  All three resolve to a LIVE, fully-configured class, never a bare template
    //  (spawning a bare native template was the original crash).
    // =======================================================================
    struct SpawnRequest
    {
        UClass*     cls = nullptr;
        std::string path;
        std::string label;
        bool        cloneNearest = false;
        bool        hookOwned = false;
    };
    std::mutex               g_spawnQueueMutex;
    std::vector<SpawnRequest> g_spawnQueue;        // any thread enqueues; game thread drains
    std::atomic<int>         g_spawnQueueCount{ 0 };
    constexpr int            kMaxSpawnQueue   = 16;  // don't let requests pile up without bound
    constexpr ULONGLONG      kSpawnIntervalMs = 300; // stream: at most one construction per window

    bool EnqueueSpawn(SpawnRequest req)
    {
        std::lock_guard<std::mutex> lk(g_spawnQueueMutex);
        if ((int)g_spawnQueue.size() >= kMaxSpawnQueue)
        {
            LOG("Spawn queue full (%d) -- ignoring request", (int)g_spawnQueue.size());
            return false;
        }
        g_spawnQueue.push_back(std::move(req));
        g_spawnQueueCount = (int)g_spawnQueue.size();
        return true;
    }

    // Load a class by its asset path ON DEMAND -- so we can spawn a saved NPC/boss
    // WITHOUT being near it (no proximity). path looks like
    // "/Game/Core/AI/Characters/Larisa/BP_NPC_Larisa.BP_NPC_Larisa_C".
    //   1. If it's already loaded -> instant name-index lookup (no GObjects scan).
    //   2. Else: KismetSystemLibrary.MakeSoftClassPath(string) builds a soft path
    //      (no FName construction needed), then LoadClassAsset_Blocking synchronously
    //      loads + returns the UClass*. Game thread only.
    // This replaces the old 6-second FindObject GObjects scan that froze the game and
    // still failed for unloaded classes.
    UClass* LoadClassByPath(const std::string& path)
    {
        if (path.empty())
            return nullptr;

        // (1) already loaded -> instant.
        if (UObject* fast = FindObjectFast(path.c_str()))
            if (Mem::IsReadable(fast, 0x30))
                return static_cast<UClass*>(fast);

        // (2) load on demand via Kismet soft-path + blocking class load.
        UObject*   ksl  = CachedObject(AH::Obj_KismetSystemLibrary);
        UFunction* mkFn = CachedFn(AH::Fn_MakeSoftClassPath);
        UFunction* ldFn = CachedFn(AH::Fn_LoadClassAsset_Blocking);
        if (!Mem::IsReadable(ksl, 0x30) || !mkFn || !ldFn)
            return nullptr;

        std::wstring w(path.begin(), path.end());
        struct { FString PathString; uint8_t Ret[0x18]; } mk{};   // ReturnValue = FSoftClassPath (0x18)
        mk.PathString.Data  = w.data();
        mk.PathString.Count = (int32_t)w.size() + 1;              // include the null terminator
        mk.PathString.Max   = mk.PathString.Count;
        // Per-call try/catch so we know EXACTLY which call faults (and fail gracefully
        // rather than letting it propagate). Prior logs showed this fault is catchable.
        try { ksl->ProcessEvent(mkFn, &mk); }
        catch (...) { LOG("LoadClassByPath: MakeSoftClassPath FAULTED for '%s'", path.c_str()); return nullptr; }

        // TSoftClassPtr (0x28) = TPersistentObjectPtr: WeakPtr@0x0, Tag@0x8, then the
        // FSoftObjectPath (the path we just built) at +0x10. Zero it so it resolves
        // purely from the path (no stale weak-ptr cache). For a top-level class asset
        // the SubPathString is empty, so there's no heap ownership to worry about.
        struct { uint8_t SoftPtr[0x28]; void* Ret; } ld{};
        memset(ld.SoftPtr, 0, sizeof(ld.SoftPtr));
        memcpy(ld.SoftPtr + 0x10, mk.Ret, 0x18);
        ld.Ret = nullptr;
        try { ksl->ProcessEvent(ldFn, &ld); }
        catch (...) { LOG("LoadClassByPath: LoadClassAsset_Blocking FAULTED loading '%s' (game asset loader)", path.c_str()); return nullptr; }

        UClass* cls = static_cast<UClass*>(ld.Ret);
        LOG("LoadClassByPath: '%s' -> cls=%p", path.c_str(), (void*)cls);
        return Mem::IsReadable(cls, 0x30) ? cls : nullptr;
    }

    // Game thread (AI pump): construct at most one queued spawn per window.
    void DrainSpawnQueue()
    {
        static ULONGLONG lastSpawnMs = 0;
        ULONGLONG now = GetTickCount64();
        if (now - lastSpawnMs < kSpawnIntervalMs)
            return;

        SpawnRequest req;
        {
            std::lock_guard<std::mutex> lk(g_spawnQueueMutex);
            if (g_spawnQueue.empty())
                return;
            req = std::move(g_spawnQueue.front());
            g_spawnQueue.erase(g_spawnQueue.begin());
            g_spawnQueueCount = (int)g_spawnQueue.size();
        }
        lastSpawnMs = now;

        // ONE-TIME, on the game thread, BEFORE the first construction: force animation
        // single-threaded. The game's parallel-anim workers otherwise build a freshly
        // spawned NPC's AnimInstance OFF the game thread the first time its mesh ticks
        // / becomes visible -> "AssembleReferenceTokenStream ... AnimInstance ...
        // called on a non-game thread" = hard fatal (hit reliably on animals / quest
        // NPCs like the Chicken / Larisa). Single-threaded anim assembles that token
        // stream on the game thread instead. Cheap; combat robots were already fine.
        static bool s_animCvarsSet = false;
        if (!s_animCvarsSet)
        {
            s_animCvarsSet = true;
            try { RunConsoleCommandImpl("a.ParallelAnimEvaluation 0"); } catch (...) {}
            try { RunConsoleCommandImpl("a.ParallelAnimUpdate 0"); } catch (...) {}
            LOG("Spawn: forced single-threaded animation (a.ParallelAnimEvaluation/Update 0) to prevent off-thread AnimInstance fatal");
        }

        const char* lbl = req.label.empty() ? "?" : req.label.c_str();
        try
        {
            UClass* cls = req.cls;
            if (req.cloneNearest)
            {
                cls = NearestLiveEnemyClass();
                LOG("Spawn '%s': clone-nearest resolved cls=%p", lbl, (void*)cls);
            }
            else if (!Mem::IsReadable(cls, 0x30) && !req.path.empty())
            {
                // Resolve instantly if loaded, else load on demand. LoadClassByPath is
                // now per-call guarded -- its fault is CAUGHT (it never hard-crashed; the
                // earlier hard crash was the nav move, now gone). It works for normal
                // classes; a quest NPC whose blueprint faults in the game's loader fails
                // gracefully with a logged reason instead of crashing.
                cls = LoadClassByPath(req.path);
            }

            if (!Mem::IsReadable(cls, 0x30))
            {
                LOG("Spawn '%s': could not resolve/load type (see LoadClassByPath log above)", lbl);
                return;
            }
            SpawnAndRegisterAlly(cls, req.hookOwned);
            LOG("Spawn '%s': SpawnAndRegisterAlly returned cleanly", lbl);
        }
        catch (...) { LOG("DrainSpawnQueue: exception during spawn of '%s' (ignored)", lbl); }
    }

    // =======================================================================
    //  LIVE MODEL LIST  --  the spawn dropdown's contents
    // -----------------------------------------------------------------------
    //  Built from the AI discovery cache: the deduped set of enemy classes that
    //  are LOADED right now. Every entry is therefore guaranteed safe to spawn
    //  (it's a live, configured class), which is why the dropdown can't pick the
    //  bare-template class that used to crash. UClass pointers persist for the
    //  session (classes aren't GC'd like instances), so caching them is safe.
    // =======================================================================
    // cls != null  -> a loaded class, spawn it directly.
    // cls == null  -> an Asset-Registry entry (game+DLC, maybe unloaded); `path` is its
    //                 class asset path, resolved on demand by LoadClassByPath at spawn.
    struct LiveModel { std::string name; UClass* cls; std::string path; };
    std::vector<LiveModel> g_liveModels;     // render thread only
    ULONGLONG              g_liveModelsMs = 0;

    std::string PrettyClassName(UClass* cls)
    {
        std::string n = cls->GetName();
        // Trim the BP boilerplate so the dropdown reads cleanly.
        if (n.size() > 2 && n.compare(n.size() - 2, 2, "_C") == 0) n.erase(n.size() - 2);
        if (n.rfind("BP_", 0) == 0) n.erase(0, 3);
        if (n.rfind("Default__", 0) == 0) n.erase(0, 9);
        return n.empty() ? cls->GetName() : n;
    }

    void RefreshLiveModels()
    {
        ULONGLONG now = GetTickCount64();
        if (!g_liveModels.empty() && now - g_liveModelsMs < 1000)
            return; // throttle: rebuilt ~1/sec so the dropdown index stays stable
        g_liveModelsMs = now;
        RequestAiDiscovery(); // keep the cache fresh while the dropdown is open

        std::vector<AiCachedActor> snap = CopyAiSnapshot();
        std::vector<LiveModel> out;
        out.reserve(16);
        for (const AiCachedActor& e : snap)
        {
            if (!AiUsable(e.actor)) continue;
            UClass* c = e.actor->Class();
            if (!Mem::IsReadable(c, 0x30)) continue;
            bool dup = false;
            for (const LiveModel& m : out) if (m.cls == c) { dup = true; break; }
            if (dup) continue;
            out.push_back({ PrettyClassName(c), c });
        }
        g_liveModels.swap(out);
    }

    // =======================================================================
    //  GLOBAL MODEL SEARCH  --  spawn ANY loaded character type, not just nearby
    // -----------------------------------------------------------------------
    //  The live-model dropdown above only lists enemy classes near you. This builds
    //  the FULL set: every loaded UClass whose superchain includes AHAICharacter --
    //  i.e. every character TYPE currently in memory anywhere (twins, robots, NPCs).
    //  Built on the WORKER thread (a bounded GObjects sweep) only while the search
    //  panel is open, throttled ~2s; the UI reads a locked snapshot + filters by text.
    //  Spawning reuses the normal streamed path (so non-combat models are handled
    //  cleanly -- the bodyguard injection is AiIsCombatCapable-gated, like Larisa).
    // =======================================================================
    std::vector<LiveModel>  g_allModels;            // guarded by g_allModelsMutex
    std::mutex              g_allModelsMutex;
    std::atomic<ULONGLONG>  g_allModelsWantedMs{ 0 }; // UI stamps this each frame the panel is open
    ULONGLONG               g_allModelsMs = 0;        // worker-thread throttle

    // ---- Asset-Registry model source (every character BP in the game + DLC) --------
    // Built ONCE on the game thread (ProcessEvent), cached here, merged into g_allModels
    // by the worker. Entries are path-only (cls=null) -> LoadClassByPath resolves them
    // at spawn, so even an UNLOADED boss (Twins) / DLC enemy can be spawned.
    std::vector<LiveModel>  g_assetModels;        // guarded by g_assetModelsMutex
    std::mutex              g_assetModelsMutex;
    std::atomic<bool>       g_assetModelsRequested{ false };
    bool                    g_assetModelsDone = false; // game-thread only

    struct BossSpawnPreset
    {
        const char* label;
        const char* aliases;
    };

    static const BossSpawnPreset kBossSpawnPresets[] =
    {
        { "Base: Twins / Ballerina",       "twinscharacter twins twin ballerina ballerinas" },
        { "Base: Belyash / MA-9",          "belyashcharacter belyash beylash ma-9 ma9" },
        { "Base: Hedgie / HOG-7",          "hedgiecharacter hedgie ezhh ezh hog-7 hog7" },
        { "Base: Plyusch / Ivy",           "plyuschcharacter plyusch plush ivy" },
        { "Base: Natasha / NA-T256",       "natashacharacter natasha na-t256 nat256" },
        { "Base: Dewdrop / ROSA",          "dewdropcharacter dewdrop rosa dew drop" },
        { "Base: VOV-A6 / Vova",           "vovcharacter vova vov-a6 vova6 vov" },
        { "DLC 1: BEA-D / BUSA",           "busacharacter busa bea-d bead beads" },
        { "DLC 1: Nora encounter",         "noracharacter nora eleanor" },
        { "DLC 2: Limbo Goose",            "goosecharacter goose limbo" },
        { "DLC 3: MOR-4Y",                 "mor-4y mor4y mory moray eel" },
        { "DLC 3: Undersea boss",          "undersea underwater sea_boss seaboss" },
        { "DLC 4: Polymorph boss",         "polymorph polymorphcharacter polymermorph" },
        { "DLC 4: Crystal boss",           "crystalboss crystal_boss chariton khariton" },
    };

    // FAssetData (CoreUObject_structs.hpp): 5 FNames then pad to 0x60.
    struct FAssetDataLite
    {
        FName ObjectPath; FName PackageName; FName PackagePath; FName AssetName; FName AssetClass;
        uint8_t pad[0x38];
    };
    static_assert(sizeof(FAssetDataLite) == 0x60, "FAssetData must be 0x60");

    // Is this asset (by lowercased path+name) plausibly a spawnable CHARACTER? Keeps the
    // list usable (the registry has tens of thousands of BPs -- props, UI, weapons...).
    bool LooksLikeCharacterAsset(const std::string& lowerPathName)
    {
        static const char* kKw[] = {
            "character", "/ai/", "_ai_", "npc", "enem", "boss", "twins", "robot",
            "mutant", "creature", "beast", "animal", "soldier", "guard", "zombie",
            "hero", "pawn_", "_pawn", "monster" };
        for (const char* k : kKw)
            if (lowerPathName.find(k) != std::string::npos) return true;
        return false;
    }

    void BuildAssetRegistryModelsGameThread()
    {
        if (g_assetModelsDone) return;
        g_assetModelsDone = true; // one attempt only -- avoid repeating the heavy hitch

        UObject*   helpers = CachedObject(AH::Obj_AssetRegistryHelpers);
        UFunction* getReg  = CachedFn(AH::Fn_GetAssetRegistry);
        if (!Mem::IsReadable(helpers, 0x30) || !getReg) { LOG("AssetModels: helpers/getReg unresolved"); return; }

        struct { void* ObjPtr; void* IfacePtr; } regRet{};
        try { helpers->ProcessEvent(getReg, &regRet); } catch (...) { LOG("AssetModels: GetAssetRegistry faulted"); return; }
        UObject* reg = reinterpret_cast<UObject*>(regRet.ObjPtr);
        if (!Mem::IsReadable(reg, 0x30)) { LOG("AssetModels: registry null"); return; }

        UFunction* byClass = CachedFn(AH::Fn_GetAssetsByClass);
        UObject*   metaBP  = FindObjectFast("Engine.BlueprintGeneratedClass");
        if (!byClass || !Mem::IsReadable(metaBP, 0x30)) { LOG("AssetModels: byClass/metaBP unresolved"); return; }

        struct { FName ClassName; TArray<FAssetDataLite> Out; bool bSearchSub; bool Ret; uint8_t pad[6]; } p{};
        p.ClassName = *metaBP->NamePtr(); // FName "BlueprintGeneratedClass"
        p.Out.Data = nullptr; p.Out.Count = 0; p.Out.Max = 0;
        p.bSearchSub = true;
        try { reg->ProcessEvent(byClass, &p); } catch (...) { LOG("AssetModels: GetAssetsByClass faulted"); return; }

        int count = p.Out.Count;
        if (count <= 0 || count > 200000 || !Mem::IsReadable(p.Out.Data, sizeof(FAssetDataLite)))
        { LOG("AssetModels: empty/invalid result count=%d", count); return; }

        auto fnameStr = [](FName& n) -> std::string { try { return n.ToString(); } catch (...) { return {}; } };
        std::vector<LiveModel> out;
        out.reserve(2048);
        int kept = 0;
        for (int i = 0; i < count && kept < 4000; ++i)
        {
            FAssetDataLite* a = &p.Out.Data[i];
            if (!Mem::IsReadable(a, sizeof(FAssetDataLite))) break;
            std::string obj = fnameStr(a->ObjectPath); // "/Game/.../BP_Foo.BP_Foo_C"
            std::string an  = fnameStr(a->AssetName);
            if (obj.size() < 3) continue;
            std::string lower = obj + "|" + an;
            for (char& c : lower) c = (char)std::tolower((unsigned char)c);
            if (!LooksLikeCharacterAsset(lower)) continue;
            if (lower.find("skel_") != std::string::npos || lower.find("reinst_") != std::string::npos)
                continue;
            // Normalise to a CLASS path (LoadClassByPath wants the generated class, _C).
            std::string path = obj;
            std::string::size_type dot = path.find_last_of('.');
            if (dot == std::string::npos || path.compare(path.size() - 2, 2, "_C") != 0)
                path += "_C";
            std::string name = an;
            if (name.size() > 2 && name.compare(name.size() - 2, 2, "_C") == 0) name.erase(name.size() - 2);
            if (name.rfind("BP_", 0) == 0) name.erase(0, 3);
            if (name.empty()) name = an;
            out.push_back({ name, nullptr, path });
            ++kept;
        }
        {
            std::lock_guard<std::mutex> lk(g_assetModelsMutex);
            g_assetModels.swap(out);
        }
        LOG("AssetModels: registry returned %d BP classes, kept %d character models", count, kept);
        // NOTE: p.Out.Data (game-allocated) is intentionally leaked once -- we build this
        // exactly once per session, so it's a small one-time leak, not a growing one.
    }

    // Walk a class's SuperStruct chain to test subclass-of (no ProcessEvent, guarded).
    bool ClassIsChildOf(UClass* cls, UClass* base)
    {
        for (int depth = 0; cls && depth < 64; ++depth)
        {
            if (cls == base) return true;
            if (!Mem::IsReadable((uint8_t*)cls + Offsets::O_UStruct_SuperStruct, sizeof(void*)))
                break;
            cls = *reinterpret_cast<UClass**>((uint8_t*)cls + Offsets::O_UStruct_SuperStruct);
        }
        return false;
    }

    void RefreshAllModelsWorker()
    {
        ULONGLONG now = GetTickCount64();
        // Only sweep while the search panel is open (the UI stamps a freshness time
        // each frame); once the panel closes the stamp goes stale and we stop.
        if (now - g_allModelsWantedMs.load() > 3000)
            return;
        if (!g_allModels.empty() && now - g_allModelsMs < 2000)
            return; // throttle: full sweep at most ~every 2s
        g_allModelsMs = now;

        if (!Mem::IsReadable(g_aiClass, 0x30))
            g_aiClass = FindObjectFast(AH::Cls_AICharacter);
        UClass* base = g_aiClass;
        if (!base)
            return;

        // We only list BLUEPRINT-generated classes (the fully-configured character
        // BPs with mesh/anim/abilities). Bare native classes are deliberately EXCLUDED:
        // spawning a bare native preset is the documented AV (the original Twins crash).
        // Pointer compare against the metaclass objects -> no GetName over all GObjects.
        static UObject* metaBPClass = nullptr;
        static UObject* metaDynClass = nullptr;
        if (!Mem::IsReadable(metaBPClass, 0x30)) metaBPClass = FindObjectFast("Engine.BlueprintGeneratedClass");
        if (!Mem::IsReadable(metaDynClass, 0x30))metaDynClass= FindObjectFast("Engine.DynamicClass");
        if (!metaBPClass)
            return; // index not ready yet -- retry next worker tick

        int n = NumObjects();
        std::vector<LiveModel> out;
        out.reserve(128);
        for (int i = 0; i < n; ++i)
        {
            UObject* o = GetObjectByIndex(i);
            if (!Mem::IsReadable(o, 0x30))
                continue;
            UObject* meta = o->Class();
            if (meta != metaBPClass && meta != metaDynClass)
                continue; // not a configured (BP) class object
            UClass* c = static_cast<UClass*>(o);
            if (c == base || !ClassIsChildOf(c, base))
                continue;
            std::string pn;
            try { pn = PrettyClassName(c); } catch (...) { continue; }
            if (pn.empty() || pn.rfind("SKEL_", 0) == 0 || pn.rfind("REINST_", 0) == 0)
                continue; // skip the compiler's abstract duplicates
            out.push_back({ pn, c, std::string() });
        }
        // Merge the Asset-Registry entries (whole game + DLC, incl. unloaded bosses),
        // deduped by name -- a loaded class (cls set) wins over a path-only entry.
        {
            std::lock_guard<std::mutex> lk(g_assetModelsMutex);
            for (const LiveModel& m : g_assetModels)
            {
                bool dup = false;
                for (const LiveModel& e : out) if (e.name == m.name) { dup = true; break; }
                if (!dup) out.push_back(m);
            }
        }
        std::sort(out.begin(), out.end(),
            [](const LiveModel& a, const LiveModel& b) { return a.name < b.name; });
        {
            std::lock_guard<std::mutex> lk(g_allModelsMutex);
            g_allModels.swap(out);
        }
    }

    int BossPresetCount()
    {
        return (int)(sizeof(kBossSpawnPresets) / sizeof(kBossSpawnPresets[0]));
    }

    std::string LowerBossText(std::string s)
    {
        for (char& c : s)
            c = (char)std::tolower((unsigned char)c);
        return s;
    }

    std::vector<std::string> BossAliasTerms(const char* aliases)
    {
        std::string flat = aliases ? aliases : "";
        for (char& c : flat)
        {
            c = (char)std::tolower((unsigned char)c);
            if (c == ',' || c == ';' || c == '|') c = ' ';
        }
        std::vector<std::string> terms;
        std::istringstream iss(flat);
        std::string term;
        while (iss >> term)
            if (term.size() >= 3)
                terms.push_back(term);
        return terms;
    }

    bool SameModelIdentity(const LiveModel& a, const LiveModel& b)
    {
        if (a.cls && b.cls && a.cls == b.cls) return true;
        if (!a.path.empty() && !b.path.empty() && a.path == b.path) return true;
        return a.name == b.name && a.path == b.path;
    }

    void AppendUniqueModel(std::vector<LiveModel>& out, const LiveModel& model)
    {
        for (const LiveModel& existing : out)
            if (SameModelIdentity(existing, model))
                return;
        out.push_back(model);
    }

    void KickBossModelDiscovery()
    {
        g_allModelsWantedMs = GetTickCount64();
        RequestAiDiscovery();
        if (!g_assetModelsRequested.exchange(true))
        {
            InstallProcessEventHook();
            QueueGameThread([]() { try { BuildAssetRegistryModelsGameThread(); } catch (...) {} });
        }
    }

    std::vector<LiveModel> BossModelSnapshot()
    {
        std::vector<LiveModel> out;
        {
            std::lock_guard<std::mutex> lk(g_allModelsMutex);
            out = g_allModels;
        }
        RefreshLiveModels();
        for (const LiveModel& model : g_liveModels)
            AppendUniqueModel(out, model);
        {
            std::lock_guard<std::mutex> lk(g_assetModelsMutex);
            for (const LiveModel& model : g_assetModels)
                AppendUniqueModel(out, model);
        }
        return out;
    }

    int ScoreBossModel(const LiveModel& model, const BossSpawnPreset& preset)
    {
        std::string name = LowerBossText(model.name);
        std::string path = LowerBossText(model.path);
        std::string hay = name + " " + path;
        int score = Mem::IsReadable(model.cls, 0x30) ? 8 : 2;
        for (const std::string& term : BossAliasTerms(preset.aliases))
        {
            if (name == term) score += 1000;
            else if (name.find(term) != std::string::npos) score += 220 + (int)term.size();
            else if (hay.find(term) != std::string::npos) score += 80 + (int)term.size();
        }
        return score;
    }

    bool ResolveBossPresetModel(int index, LiveModel& out, int& scoreOut)
    {
        scoreOut = 0;
        if (index < 0 || index >= BossPresetCount())
            return false;
        KickBossModelDiscovery();
        std::vector<LiveModel> snap = BossModelSnapshot();
        int bestScore = 0;
        LiveModel best{};
        for (const LiveModel& model : snap)
        {
            int score = ScoreBossModel(model, kBossSpawnPresets[index]);
            if (score > bestScore)
            {
                bestScore = score;
                best = model;
            }
        }
        scoreOut = bestScore;
        if (bestScore < 80)
            return false;
        out = best;
        return Mem::IsReadable(out.cls, 0x30) || !out.path.empty();
    }

    bool QueueBossPresetSpawn(int index, bool hookOwned)
    {
        if (!G::sdkReady.load())
            return false;
        if (index < 0 || index >= BossPresetCount())
            return false;

        LiveModel model{};
        int score = 0;
        if (!ResolveBossPresetModel(index, model, score))
        {
            LOG("%s boss preset '%s' unresolved yet (score=%d). Open the model search once or wait for AssetModels to finish.",
                hookOwned ? "[AI-HOOK]" : "AiSpawnBossPreset", kBossSpawnPresets[index].label, score);
            return false;
        }
        if (!InstallProcessEventHook())
            return false;

        SpawnRequest req;
        if (Mem::IsReadable(model.cls, 0x30)) req.cls = model.cls; else req.path = model.path;
        req.label = std::string(hookOwned ? "Hook Diagnostics " : "") + kBossSpawnPresets[index].label;
        if (!model.name.empty()) req.label += " -> " + model.name;
        req.hookOwned = hookOwned;
        bool queued = EnqueueSpawn(std::move(req));
        LOG("%s boss preset '%s' resolved to '%s' score=%d spawn %s (%s)",
            hookOwned ? "[AI-HOOK]" : "AiSpawnBossPreset", kBossSpawnPresets[index].label,
            model.name.empty() ? model.path.c_str() : model.name.c_str(), score,
            queued ? "queued" : "refused", model.cls ? "loaded runtime class" : "load-on-demand class path");
        return queued;
    }

    // =======================================================================
    //  DEEP ENEMY SWEEP  --  catch the "uncatchable, unkillable" edge case
    // -----------------------------------------------------------------------
    //  The normal AI cache is a level-walk (PersistentLevel + Levels[]). ~20% of the
    //  time an enemy lives in a streaming sublevel the walk misses, so it never enters
    //  the cache -> ESP/Kill-all/Release never touch it -> it keeps attacking and
    //  "Kill all" can't reach it (and if it was ever team-flipped friendly it reads as
    //  unkillable). This sweeps ALL of GObjects for live AHAICharacter instances (the
    //  authoritative set), excluding you + your squad, and queues a kill on every one
    //  (health-zero is a direct write, so it kills even a leaked-friendly). Worker
    //  thread + class memo so it's a one-shot ~tens-of-ms, never the per-frame path.
    // =======================================================================
    std::atomic<bool> g_deepKillRequested{ false };

    void RunDeepKillWorker()
    {
        if (!g_deepKillRequested.exchange(false))
            return;
        if (!Mem::IsReadable(g_aiClass, 0x30))
            g_aiClass = FindObjectFast(AH::Cls_AICharacter);
        UClass* cls = g_aiClass;
        if (!cls)
            return;

        UObject* player = GetLocalPawn();
        std::vector<UObject*> squad;
        { std::lock_guard<std::mutex> lk(g_squadMutex); squad = g_spawnedAllies; }

        int n = NumObjects();
        std::vector<UObject*> targets;
        targets.reserve(128);
        std::unordered_map<UClass*, bool> memo; // isAi by class (one IsA per distinct class)
        for (int i = 0; i < n && (int)targets.size() < kMaxDeepAiTargets; ++i)
        {
            UObject* o = GetObjectByIndex(i);
            if (!Mem::IsReadable(o, 0x30) || o == player)
                continue;
            bool skip = false;
            for (UObject* s : squad) if (s == o) { skip = true; break; }
            if (skip)
                continue;
            UClass* oc = o->Class();
            if (!Mem::IsReadable(oc, 0x30))
                continue;
            bool isAi;
            auto it = memo.find(oc);
            if (it != memo.end()) isAi = it->second;
            else { try { isAi = o->IsA(cls); } catch (...) { isAi = false; } memo[oc] = isAi; }
            if (!isAi)
                continue;
            std::string on;
            try { on = o->GetName(); } catch (...) { continue; }
            if (on.rfind("Default__", 0) == 0 || on.find("GEN_VARIABLE") != std::string::npos)
                continue; // CDOs/templates, not live actors
            targets.push_back(o);
        }
        if (!targets.empty())
            QueueAiOperation(AiQueuedKind::Kill, targets);
        LOG("DeepKill: swept %d objects -> queued %d enemy kill(s)", n, (int)targets.size());
    }

    // =======================================================================
    //  SAVED-CHARACTER DB  --  persist cloned characters to JSON, spawn later
    // -----------------------------------------------------------------------
    //  We can't persist a UClass pointer across sessions, so we store the
    //  class's runtime path (the part of GetFullName after the type prefix) and
    //  re-resolve it by name when spawning. The class only resolves if that
    //  character type is loaded in the current level -- otherwise spawn fails
    //  gracefully with a log line. File: AtomicHeartMenu_bodyguards.json beside
    //  the game exe. Plain hand-rolled JSON (no dependency added).
    // =======================================================================
    struct SavedCharacter { std::string name; std::string classPath; UClass* cachedClass = nullptr; };
    std::vector<SavedCharacter> g_savedChars;       // guarded by g_savedCharsMutex
    std::mutex                  g_savedCharsMutex;
    bool                        g_savedCharsLoaded = false;

    bool BuildBodyguardDbPath(char out[MAX_PATH])
    {
        char exePath[MAX_PATH]{};
        DWORD len = GetModuleFileNameA(nullptr, exePath, MAX_PATH);
        if (len == 0 || len >= MAX_PATH)
            return false;
        strcpy_s(out, MAX_PATH, exePath);
        char* slash  = strrchr(out, '\\');
        char* slash2 = strrchr(out, '/');
        if (slash2 && (!slash || slash2 > slash)) slash = slash2;
        if (!slash)
            return false;
        slash[1] = '\0';
        strcat_s(out, MAX_PATH, "AtomicHeartMenu_bodyguards.json");
        return true;
    }

    std::string JsonEscape(const std::string& s)
    {
        std::string o;
        o.reserve(s.size() + 8);
        for (char c : s)
        {
            switch (c)
            {
                case '\"': o += "\\\""; break;
                case '\\': o += "\\\\"; break;
                case '\n': o += "\\n";  break;
                case '\r': o += "\\r";  break;
                case '\t': o += "\\t";  break;
                default:
                    // Strip other control chars (< 0x20) -> space, so a garbage
                    // FName picked up by the broad pointer/component scans can never
                    // emit raw control bytes that would corrupt the JSON.
                    if ((unsigned char)c < 0x20) o += ' ';
                    else o += c;
                    break;
            }
        }
        return o;
    }

    // Read the quoted JSON string whose opening quote is at t[from]; unescapes it
    // and sets `end` just past the closing quote.
    bool ReadJsonString(const std::string& t, size_t from, std::string& out, size_t& end)
    {
        if (from >= t.size() || t[from] != '\"')
            return false;
        std::string o;
        for (size_t i = from + 1; i < t.size(); ++i)
        {
            char c = t[i];
            if (c == '\\' && i + 1 < t.size())
            {
                char n = t[++i];
                switch (n)
                {
                    case 'n': o += '\n'; break;
                    case 'r': o += '\r'; break;
                    case 't': o += '\t'; break;
                    default:  o += n;    break; // covers \" \\ \/ and any literal
                }
            }
            else if (c == '\"')
            {
                end = i + 1;
                out = o;
                return true;
            }
            else
            {
                o += c;
            }
        }
        return false;
    }

    // Find the next "key" from `pos`, then read the string value after its colon.
    bool NextKeyString(const std::string& t, size_t& pos, const char* key, std::string& out)
    {
        std::string needle = std::string("\"") + key + "\"";
        size_t k = t.find(needle, pos);
        if (k == std::string::npos) return false;
        size_t colon = t.find(':', k + needle.size());
        if (colon == std::string::npos) return false;
        size_t q = t.find('\"', colon + 1);
        if (q == std::string::npos) return false;
        size_t end = 0;
        if (!ReadJsonString(t, q, out, end)) return false;
        pos = end;
        return true;
    }

    // Caller MUST hold g_savedCharsMutex. Loads once; missing file => empty list.
    void LoadSavedCharacters()
    {
        if (g_savedCharsLoaded)
            return;
        g_savedCharsLoaded = true;
        char path[MAX_PATH]{};
        if (!BuildBodyguardDbPath(path))
            return;
        std::ifstream f(path, std::ios::binary);
        if (!f)
            return;
        std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        f.close();
        size_t pos = 0;
        std::string name, cls;
        while (NextKeyString(text, pos, "name", name) && NextKeyString(text, pos, "class", cls))
            if (!cls.empty())
                g_savedChars.push_back({ name, cls });
        LOG("Bodyguard DB: loaded %d saved character(s)", (int)g_savedChars.size());
    }

    // Caller MUST hold g_savedCharsMutex.
    void WriteSavedCharacters()
    {
        char path[MAX_PATH]{};
        if (!BuildBodyguardDbPath(path))
        {
            LOG("Bodyguard DB: cannot resolve file path");
            return;
        }
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        if (!f)
        {
            LOG("Bodyguard DB: cannot open %s for write", path);
            return;
        }
        f << "{\n  \"characters\": [\n";
        for (size_t i = 0; i < g_savedChars.size(); ++i)
        {
            f << "    { \"name\": \"" << JsonEscape(g_savedChars[i].name)
              << "\", \"class\": \"" << JsonEscape(g_savedChars[i].classPath) << "\" }"
              << (i + 1 < g_savedChars.size() ? "," : "") << "\n";
        }
        f << "  ]\n}\n";
        f.close();
        LOG("Bodyguard DB: wrote %d character(s)", (int)g_savedChars.size());
    }

    // =======================================================================
    //  HORDE ROUNDS  --  arena wave survival (HOSTILE, killable robots)
    // -----------------------------------------------------------------------
    //  This is a SEPARATE system from the squad/bodyguard code. It reuses the
    //  same proven spawn primitive (AIBlueprintHelperLibrary.SpawnAIFromClass,
    //  exactly as SpawnAndRegisterAlly uses it -- read, not modified), but where
    //  the bodyguard path registers the spawn as a friendly, invincible ally,
    //  the horde path does the OPPOSITE: it parks the robot Hostile to the player
    //  and force-aggros it onto you, and it NEVER joins the squad -- so the squad's
    //  "keep allies topped to full health" logic can't touch it and it stays fully
    //  killable (the "not in godmode" requirement). All of the live state below is
    //  GAME-THREAD ONLY (driven from the AI pump), guarded by atomics the menu reads.
    // =======================================================================

    // --- robot roster (the live wave) -------------------------------------
    std::vector<UObject*> g_hordeEnemies;       // game-thread only (the AI pump)
    std::atomic<bool>     g_hordeActive{ false };
    std::atomic<int>      g_hordeAlive{ 0 };
    std::atomic<int>      g_hordeKills{ 0 };
    std::atomic<int>      g_hordeRound{ 0 };
    std::atomic<int>      g_hordePending{ 0 };   // robots still to spawn this wave

    constexpr int      kMaxHordeAlive   = 18;    // perf cap on concurrent live robots
    constexpr int      kMaxHordePerWave = 40;    // hard cap on a single wave's budget
    constexpr ULONGLONG kHordeSpawnMs   = 450;   // stream one robot per this window (never a hitch)
    constexpr ULONGLONG kHordeAggroMs   = 700;   // re-assert each robot's player aggro at most this often
    constexpr float    kHordeSpawnMinU  = 850.0f;// spawn ring: closest robots appear ~8.5m out
    constexpr float    kHordeSpawnMaxU  = 1600.0f;//            farthest ~16m out

    // Return point captured at run start so Stop / death can restore you exactly.
    FVector   g_hordeReturnLoc{};
    FRotator  g_hordeReturnRot{};
    bool      g_hordeTeleported = false;         // did this run teleport (location != "Here")?
    bool      g_hordeHaveReturn = false;

    // Tiny self-contained RNG (avoids pulling in <cstdlib> / global rand state).
    uint32_t HordeRand()
    {
        static uint32_t s = 0x9E3779B9u ^ (uint32_t)GetTickCount64();
        s ^= s << 13; s ^= s >> 17; s ^= s << 5;
        return s;
    }
    float HordeRandRange(float lo, float hi)
    {
        return lo + (hi - lo) * ((float)(HordeRand() & 0xFFFFFF) / (float)0x1000000);
    }

    // Resolve + cache the three save UFunctions so hkProcessEvent can swallow them by
    // pointer while a run is active. Game thread (CachedFn walks GObjects on first use).
    void ResolveSaveBlockFns()
    {
        if (!g_fnSaveProgress.load())
            g_fnSaveProgress.store(CachedFn(AH::Fn_AHGameInstance_SaveProgress));
        if (!g_fnSavePersistentData.load())
            g_fnSavePersistentData.store(CachedFn(AH::Fn_AHGameInstance_SavePersistentData));
        if (!g_fnCheckpointSaveProgress.load())
            g_fnCheckpointSaveProgress.store(CachedFn(AH::Fn_CheckpointObject_SaveProgress));
    }

    // --- arena "locations" DB (persisted to AtomicHeartMenu_arenas.json) ----
    // Index 0 is the implicit "Here" (no teleport); the vector holds the user-saved
    // spots only. Each is a captured pawn position + control rotation.
    struct ArenaLocation { std::string name; FVector loc{}; FRotator rot{}; };
    std::vector<ArenaLocation> g_arenas;            // guarded by g_arenasMutex
    std::mutex                 g_arenasMutex;
    bool                       g_arenasLoaded = false;

    bool BuildArenaDbPath(char out[MAX_PATH])
    {
        char exePath[MAX_PATH]{};
        DWORD len = GetModuleFileNameA(nullptr, exePath, MAX_PATH);
        if (len == 0 || len >= MAX_PATH)
            return false;
        strcpy_s(out, MAX_PATH, exePath);
        char* slash  = strrchr(out, '\\');
        char* slash2 = strrchr(out, '/');
        if (slash2 && (!slash || slash2 > slash)) slash = slash2;
        if (!slash)
            return false;
        slash[1] = '\0';
        strcat_s(out, MAX_PATH, "AtomicHeartMenu_arenas.json");
        return true;
    }

    // Caller MUST hold g_arenasMutex. Loads once; missing file => empty list.
    void LoadArenas()
    {
        if (g_arenasLoaded)
            return;
        g_arenasLoaded = true;
        char path[MAX_PATH]{};
        if (!BuildArenaDbPath(path))
            return;
        std::ifstream f(path, std::ios::binary);
        if (!f)
            return;
        std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        f.close();
        size_t pos = 0;
        std::string name, coords;
        while (NextKeyString(text, pos, "name", name) && NextKeyString(text, pos, "pos", coords))
        {
            ArenaLocation a; a.name = name;
            // "pos" is "X Y Z Yaw" -- a compact, human-editable record.
            float yaw = 0.0f;
            if (sscanf_s(coords.c_str(), "%f %f %f %f", &a.loc.X, &a.loc.Y, &a.loc.Z, &yaw) >= 3)
            {
                a.rot.Yaw = yaw;
                g_arenas.push_back(a);
            }
        }
        LOG("Arena DB: loaded %d location(s)", (int)g_arenas.size());
    }

    // Caller MUST hold g_arenasMutex.
    void SaveArenas()
    {
        char path[MAX_PATH]{};
        if (!BuildArenaDbPath(path))
            return;
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        if (!f)
        {
            LOG("Arena DB: cannot open %s for write", path);
            return;
        }
        f << "{\n  \"arenas\": [\n";
        for (size_t i = 0; i < g_arenas.size(); ++i)
        {
            char coords[96];
            sprintf_s(coords, "%.1f %.1f %.1f %.1f",
                      g_arenas[i].loc.X, g_arenas[i].loc.Y, g_arenas[i].loc.Z, g_arenas[i].rot.Yaw);
            f << "    { \"name\": \"" << JsonEscape(g_arenas[i].name)
              << "\", \"pos\": \"" << coords << "\" }"
              << (i + 1 < g_arenas.size() ? "," : "") << "\n";
        }
        f << "  ]\n}\n";
        f.close();
        LOG("Arena DB: wrote %d location(s)", (int)g_arenas.size());
    }

    // Pick a robot class to spawn: a LOADED, combat-capable enemy type from the AI
    // discovery cache (same safety guarantee as the spawn dropdown -- only ever a
    // live, fully-configured class, never a bare native template). Prefers obvious
    // robot/enemy names and skips bosses (Twins) so a wave is rank-and-file robots.
    // Returns null if nothing suitable is loaded (caller tells the player to move
    // near some robots so their type streams in). Game thread.
    UClass* PickHordeRobotClass()
    {
        std::vector<AiCachedActor> snap = CopyAiSnapshot();
        std::vector<UClass*> preferred; // robot-named combat classes
        std::vector<UClass*> anyCombat;  // any combat-capable class (fallback)
        static const char* kBossSkip[]  = { "twin", "boss", "natasha", "plyush", "hlebozavod" };
        static const char* kRobotHint[] = { "robot", "vov", "pchela", "belyash", "gatling",
                                            "hedgie", "ezhik", "mendel", "lutsh", "soldier",
                                            "rotorobot", "fryer", "suslik", "laser" };
        for (const AiCachedActor& e : snap)
        {
            if (!AiUsable(e.actor) || e.healthFrac == 0.0f) continue;
            if (!AiIsCombatCapable(e.actor)) continue;
            UClass* c = e.actor->Class();
            if (!Mem::IsReadable(c, 0x30)) continue;
            std::string n = c->GetName();
            for (char& ch : n) ch = (char)std::tolower((unsigned char)ch);
            bool boss = false;
            for (const char* k : kBossSkip) if (n.find(k) != std::string::npos) { boss = true; break; }
            if (boss) continue;
            // dedupe within each bucket
            auto has = [](const std::vector<UClass*>& v, UClass* x){ for (UClass* q : v) if (q == x) return true; return false; };
            if (!has(anyCombat, c)) anyCombat.push_back(c);
            bool robot = false;
            for (const char* k : kRobotHint) if (n.find(k) != std::string::npos) { robot = true; break; }
            if (robot && !has(preferred, c)) preferred.push_back(c);
        }
        const std::vector<UClass*>& pool = !preferred.empty() ? preferred : anyCombat;
        if (pool.empty())
            return nullptr;
        return pool[HordeRand() % (uint32_t)pool.size()];
    }

    // Spawn ONE hostile robot around the player and force it to hunt you. Mirrors
    // SpawnAndRegisterAlly's use of SpawnAIFromClass but: rings the spawn around you,
    // does NOT add to the squad, and seeds combat AGAINST the player. Game thread.
    bool SpawnHordeEnemy(UClass* cls)
    {
        if (!Mem::IsReadable(cls, 0x30))
            return false;
        UObject* lib = CachedObject(AH::Obj_AIBlueprintHelper);
        UFunction* fn = CachedClassFn(AH::Cls_AIBlueprintHelper, "SpawnAIFromClass");
        if (!Mem::IsReadable(lib, 0x30) || !fn)
            return false;

        UObject* player = nullptr;
        FVector playerLoc{};
        if (!ReadLocalPawnLocationFast(playerLoc, &player) || !Mem::IsReadable(player, 0x30))
            return false;
        UObject* world = GetWorld();
        if (!Mem::IsReadable(world, 0x30))
            return false;

        // Ring placement: a random bearing + radius so a wave surrounds you instead
        // of stacking on one spot. Z lifted so SnapCapsuleToGround drops it to the floor.
        float ang    = HordeRandRange(0.0f, 6.2831853f);
        float radius = HordeRandRange(kHordeSpawnMinU, kHordeSpawnMaxU);
        FVector spawnLoc = playerLoc;
        spawnLoc.X += cosf(ang) * radius;
        spawnLoc.Y += sinf(ang) * radius;
        spawnLoc.Z += 110.0f;
        float faceYaw = (ang + 3.1415927f) * 57.29577951f; // face back toward the player

        P_SpawnAIFromClass p{};
        p.WorldContextObject = world;
        p.PawnClass = cls;
        p.BehaviorTree = nullptr;
        p.Location = spawnLoc;
        p.Rotation = { 0.0f, faceYaw, 0.0f };
        p.bNoCollisionFail = true;
        p.Owner = nullptr; // an ENEMY -- not owned by the player

        lib->ProcessEvent(fn, &p);
        UObject* robot = static_cast<UObject*>(p.ReturnValue);
        if (!Mem::IsReadable(robot, 0x30))
        {
            LOG("SpawnHordeEnemy: SpawnAIFromClass returned null");
            return false;
        }

        g_hordeEnemies.push_back(robot);
        g_hordeAlive = (int)g_hordeEnemies.size();

        try
        {
            ForceActorVisible(robot);
            SnapAiToGround(robot);
            // Seed combat AGAINST the player: target = player, aggressive, team forced
            // Hostile-to-player. The per-pump UpdateHorde keeps re-asserting it.
            InjectAttack(robot, player, player, 2 /*ETeamAttitude::Hostile*/, -1);
        }
        catch (...) { LOG("SpawnHordeEnemy: fixup threw (ignored)"); }
        RequestAiDiscovery();
        LOG("SpawnHordeEnemy: spawned hostile robot=%p (alive=%d)", (void*)robot, g_hordeAlive.load());
        return true;
    }

    // Remove every roster robot. destroy=true deletes them from the world outright
    // (clean arena teardown); destroy=false just forgets them (used when the level is
    // already tearing down, e.g. after a death-reload, so we never poke a dying actor).
    void HordeClearEnemies(bool destroy)
    {
        UFunction* destroyFn = destroy ? CachedFn(AH::Fn_K2DestroyActor) : nullptr;
        for (UObject* r : g_hordeEnemies)
        {
            if (!Mem::IsReadable(r, 0x30)) continue;
            try
            {
                if (destroy && destroyFn)
                {
                    uint8_t noParams = 0;
                    r->ProcessEvent(destroyFn, &noParams);
                }
                else
                {
                    SetCharacterHealthZero(r); // safe direct write
                }
            }
            catch (...) {}
            g_inject.erase(r);
            g_engagedUntilMs.erase(r);
            g_lastThreatMs.erase(r);
            g_origTeam.erase(r);
        }
        g_hordeEnemies.clear();
        g_hordeAlive = 0;
    }

    // Begin a wave: set the spawn budget (it streams in over UpdateHorde ticks).
    void HordeStartWave(int round)
    {
        g_hordeRound = round;
        int budget = g_state.hordePerRound + (round - 1) * 2; // each wave adds 2 robots
        if (budget < 1) budget = 1;
        if (budget > kMaxHordePerWave) budget = kMaxHordePerWave;
        g_hordePending = budget;
        LOG("Horde: wave %d begins (budget=%d)", round, budget);
    }

    // Tear the run down cleanly. restorePos teleports you back to the run-start point
    // (used by Stop); on a death we skip the teleport and let the game's own death /
    // reload flow run, but we ALWAYS clear robots and re-enable saving.
    void HordeEndRun(bool restorePos, bool destroyEnemies, const char* why)
    {
        LOG("Horde: ending run (%s) restorePos=%d", why ? why : "?", restorePos);
        g_hordeActive = false;
        g_hordePending = 0;
        g_hordeRound = 0;
        HordeClearEnemies(destroyEnemies);

        // Re-enable saving FIRST so the game can checkpoint normally again.
        g_blockSaves = false;

        if (restorePos && g_hordeHaveReturn && g_hordeTeleported)
        {
            UObject* pawn = GetLocalPawn();
            if (Mem::IsReadable(pawn, 0x30))
            {
                SetActorLocation(pawn, g_hordeReturnLoc, true);
                RefreshFlyStreaming(pawn, true); // make the original area stream back in
                LOG("Horde: restored player to %.0f %.0f %.0f",
                    g_hordeReturnLoc.X, g_hordeReturnLoc.Y, g_hordeReturnLoc.Z);
            }
        }
        g_hordeTeleported = false;
        g_hordeHaveReturn = false;
        g_state.hordeActive = false;
    }

    // The per-pump heartbeat (game thread, from DrainAiGameThreadWork). Spawns the
    // wave gradually, keeps every robot aggro'd on you, prunes the dead, advances
    // waves, and -- the safety rail -- HALTS the instant you die.
    void UpdateHorde()
    {
        if (!g_hordeActive.load())
            return;

        UObject* player = nullptr;
        FVector playerLoc{};
        if (!ReadLocalPawnLocationFast(playerLoc, &player) || !Mem::IsReadable(player, 0x30))
            return; // pawn transiently gone (loading) -- try again next pump

        // --- DEATH HALT: if you're dead, stop everything and clean up immediately.
        bool dead = false;
        float cur = 0.0f, mx = 0.0f;
        if (ReadCharacterHealth(player, cur, mx) && mx > 0.001f && cur <= 0.0f)
            dead = true;
        uint8_t* pb = reinterpret_cast<uint8_t*>(player);
        if (!dead && Mem::IsReadable(pb + AH::Char_bIsDead, 1) && *reinterpret_cast<bool*>(pb + AH::Char_bIsDead))
            dead = true;
        if (dead)
        {
            LOG("Horde: PLAYER DIED -- halting run + cleaning up");
            // Don't teleport a dying/reloading pawn; just forget robots (the level may
            // be reloading) and re-enable saving so the game recovers normally.
            HordeEndRun(false /*restorePos*/, false /*destroyEnemies*/, "player death");
            return;
        }

        // --- prune dead robots, count kills ---------------------------------
        ULONGLONG now = GetTickCount64();
        std::vector<UObject*> survivors;
        survivors.reserve(g_hordeEnemies.size());
        for (UObject* r : g_hordeEnemies)
        {
            bool aliveRobot = false;
            if (Mem::IsReadable(r, 0x30) && AiUsable(r))
            {
                float rc = 0.0f, rm = 0.0f;
                if (ReadCharacterHealth(r, rc, rm))
                    aliveRobot = (rm <= 0.001f) ? true : (rc > 0.0f); // unknown max -> assume alive
                else
                    aliveRobot = true;
            }
            if (aliveRobot)
            {
                survivors.push_back(r);
            }
            else
            {
                g_hordeKills = g_hordeKills.load() + 1;
                g_inject.erase(r);
                g_engagedUntilMs.erase(r);
                g_lastThreatMs.erase(r);
                g_origTeam.erase(r);
            }
        }
        if (survivors.size() != g_hordeEnemies.size())
            g_hordeEnemies.swap(survivors);
        g_hordeAlive = (int)g_hordeEnemies.size();

        // --- keep every survivor hunting YOU (throttled re-aggro) -----------
        for (UObject* r : g_hordeEnemies)
        {
            if (!Mem::IsReadable(r, 0x30)) continue;
            InjectState& s = g_inject[r];
            if (s.target != player || now - s.lastHeavyMs > kHordeAggroMs)
            {
                try { InjectAttack(r, player, player, 2 /*Hostile*/, -1); } catch (...) {}
            }
        }

        // --- stream the wave in (one robot per window, respecting the cap) --
        static ULONGLONG lastSpawnMs = 0;
        if (g_hordePending.load() > 0 && g_hordeAlive.load() < kMaxHordeAlive &&
            now - lastSpawnMs >= kHordeSpawnMs)
        {
            lastSpawnMs = now;
            UClass* cls = PickHordeRobotClass();
            if (cls)
            {
                if (SpawnHordeEnemy(cls))
                    g_hordePending = g_hordePending.load() - 1;
            }
            else
            {
                static ULONGLONG lastWarnMs = 0;
                if (now - lastWarnMs > 3000)
                {
                    lastWarnMs = now;
                    LOG("Horde: no robot models loaded -- move near some robots so their type streams in");
                }
            }
        }

        // --- wave clear -> advance (or idle until the player starts the next) ---
        if (g_hordePending.load() == 0 && g_hordeAlive.load() == 0)
        {
            if (g_state.hordeAutoAdvance)
            {
                static ULONGLONG clearedAtMs = 0;
                if (clearedAtMs == 0) clearedAtMs = now;
                if (now - clearedAtMs > 2500) // brief breather between waves
                {
                    clearedAtMs = 0;
                    HordeStartWave(g_hordeRound.load() + 1);
                }
            }
        }
    }

    // =======================================================================
    //  GAME-THREAD AI PUMP  --  why every AI ProcessEvent runs here
    // -----------------------------------------------------------------------
    //  UE4's ProcessEvent and the behaviour tree / blackboard are NOT thread
    //  safe. Our menu lives in the DX12 Present hook (render thread). Calling AI
    //  UFunctions there races the game thread's own AI tick; a one-shot is a tiny
    //  window, but the CONTINUOUS fight/bodyguard injection hammered it for
    //  minutes and eventually corrupted BT/blackboard state mid-iteration -> the
    //  game thread spun forever inside engine AI code -> the hard freeze.
    //
    //  Fix: do ALL AI ProcessEvent work on the GAME thread. We already borrow it
    //  via the ProcessEvent hook (used for spawning). The render thread only
    //  SCHEDULES one bounded pump task at a time; the pump runs the drain +
    //  injection on the game thread, serialised with the game's own AI tick, so
    //  there is no race. Raw memory writes are thread-safe and stay inline.
    // =======================================================================
    void DrainAiGameThreadWork()
    {
        // All touch the AI via ProcessEvent -> must be on the game thread.
        DrainSpawnQueue();         // construct at most one queued spawn (streamed)
        DrainAiOperation();        // one-shot queued ops (kill / fight / launch / ...)
        UpdateAiAutoControls();    // auto follow / freeze
        UpdateAiCombatInjection(); // continuous fight / bodyguard injection
        DriveSpawnedAllies();      // spawned allies = permanent bodyguards
        DriveTwinCombat();         // the Twin runs a SEPARATE, fully-controlled combat brain
        UpdateHorde();             // horde rounds: spawn waves, re-aggro, prune, death-halt
    }

    // Render thread: keep at most one bounded AI pump in flight on the game
    // thread. Never calls AI ProcessEvent itself. If the game thread can't be
    // borrowed yet (hook not installable), it simply does nothing this frame --
    // strictly safer than touching AI from the render thread.
    void ScheduleAiGameThreadWork()
    {
        auto& st = Features::Get();

        bool aiActive = st.aiFightEachOther || st.aiFreezeNearby ||
                        g_spawnedAllyCount.load() > 0 ||   // squad members need driving
                        g_hordeActive.load() ||            // a horde run drives waves every pump
                        g_aiPendingCount.load() > 0 ||
                        g_spawnQueueCount.load() > 0 ||
                        g_aiDeferredKind.load() != (int)AiQueuedKind::None;

        // Keep pumping for a short grace window after everything turns off so the
        // falling-edge release (un-aggro / unfreeze) actually runs once.
        static ULONGLONG lastActiveMs = 0;
        ULONGLONG now = GetTickCount64();
        if (aiActive)
            lastActiveMs = now;
        if (!aiActive && now - lastActiveMs > 2500)
            return;

        if (g_aiPumpInFlight.load())
            return; // previous pump still queued/running -- don't pile up

        if (!InstallProcessEventHook())
            return; // no game thread yet: do NOT run AI on the render thread

        static ULONGLONG lastQueueMs = 0;
        if (now - lastQueueMs < 120) // cap how often we hand the game thread a batch
            return;
        lastQueueMs = now;

        g_aiPumpInFlight = true;
        QueueGameThread([]()
        {
            try { DrainAiGameThreadWork(); }
            catch (...) {}
            g_aiPumpInFlight = false;
        });
    }

    // Worker thread only: rebuild the AI list (ESP + every World->Enemy AI
    // command read it). Walks the loaded levels' actor lists -- a few thousand
    // actors, a few ms -- instead of sweeping ~360k UObjects, so it is effectively
    // instant, never hangs, and the wholesale rebuild keeps the list fresh
    // (new spawns appear, dead actors drop out) without any cursor/budget state.
    void RefreshAiActors()
    {
        const auto& st = Features::Get();
        bool discoveryRequested = g_aiDiscoveryRequested.exchange(false);
        bool featuresWantAi = st.espEnabled || st.aiFreezeNearby || st.aiFightEachOther ||
                              g_spawnedAllyCount.load() > 0 ||  // squad needs the cache fresh
                              g_hordeActive.load();             // horde picks robot classes from the cache
        bool pendingWork = g_aiPendingCount.load() > 0 || g_aiDeferredKind.load() != (int)AiQueuedKind::None;
        bool wantsScan = featuresWantAi || pendingWork || discoveryRequested;
        if (!wantsScan)
            return;

        static ULONGLONG lastUpdateMs = 0;
        ULONGLONG now = GetTickCount64();
        const ULONGLONG intervalMs = (st.aiFreezeNearby || st.aiFightEachOther ||
                                      g_spawnedAllyCount.load() > 0 || pendingWork) ? 200 : 350;
        if (!discoveryRequested && now - lastUpdateMs < intervalMs)
            return;
        lastUpdateMs = now;

        if (!Mem::IsReadable(g_aiClass, 0x30))
            g_aiClass = FindObjectFast(AH::Cls_AICharacter); // cached UClass*; name-index lookup, no GObjects sweep
        UClass* cls = g_aiClass;
        if (!cls)
            return;

        FVector playerLoc{};
        bool havePlayerLoc = ReadActorLocationFast(GetLocalPawn(), playerLoc);

        // Fast path: loaded-level actor arrays. Authoritative fallback: an
        // incremental GObjects pass merged below catches streamed enemies omitted
        // from those arrays (the exact ESP/Kill-All blind spot).
        std::vector<UObject*> actors;
        actors.reserve(2048);
        try { CollectLevelActors(actors); }
        catch (...) { return; }
        static bool globalDiscoveryPrimed = false;
        bool forceGlobal = !globalDiscoveryPrimed;
        try { RefreshGlobalAiDiscovery(cls, forceGlobal); }
        catch (...) {}
        globalDiscoveryPrimed = true;
        for (const auto& seen : g_globalAiSeen)
            if (Mem::IsReadable(seen.first, 0x30))
                actors.push_back(seen.first);

        ULONGLONG scanStartMs = GetTickCount64();
        std::vector<AiCachedActor> rebuilt;
        rebuilt.reserve(64);
        std::unordered_map<UObject*, bool> classCache; // isAi by UClass* (cached per rebuild)
        std::unordered_set<UObject*> visited;
        int aiHits = 0;
        int scanExceptions = 0;

        for (UObject* o : actors)
        {
            try
            {
                if (!visited.insert(o).second)
                    continue;
                UObject* objectClass = o->Class();
                if (!Mem::IsReadable(objectClass, 0x30))
                    continue;

                bool isAi;
                auto it = classCache.find(objectClass);
                if (it != classCache.end())
                    isAi = it->second;
                else { isAi = o->IsA(cls); classCache[objectClass] = isAi; }
                if (!isAi)
                    continue;

                ++aiHits;
                AiCachedActor entry{};
                if (!RefreshAiEntryFromIndex({ o, -1 }, cls, playerLoc, havePlayerLoc, entry)) // -1: use the actor directly
                    continue;
                rebuilt.push_back(entry);
            }
            catch (...) { ++scanExceptions; }
        }

        std::sort(rebuilt.begin(), rebuilt.end(),
            [](const AiCachedActor& a, const AiCachedActor& b) { return a.distanceM < b.distanceM; });
        if ((int)rebuilt.size() > kMaxCachedAiActors)
            rebuilt.resize(kMaxCachedAiActors);

        {
            std::lock_guard<std::mutex> lk(g_aiMutex);
            g_aiActors.swap(rebuilt); // full wholesale rebuild: always fresh, dead actors drop out
            g_aiCachedCount = (int)g_aiActors.size();
        }

        ULONGLONG elapsedMs = GetTickCount64() - scanStartMs;
        static ULONGLONG lastLogMs = 0;
        // Only log occasionally or on a genuinely slow scan -- a normal scan of ~10k
        // actors is 15-16ms, so the old elapsedMs>8 condition logged EVERY refresh and
        // flooded the log file.
        if (elapsedMs > 40 || now - lastLogMs > 5000)
        {
            lastLogMs = now;
            LOG("AI refresh (levels+global): candidates=%d global=%d aiHits=%d cached=%d exceptions=%d time=%llums",
                (int)actors.size(), (int)g_globalAiSeen.size(), aiHits,
                g_aiCachedCount.load(), scanExceptions, elapsedMs);
        }
    }

    // =======================================================================
    //  RENDER HIJACK  --  chams / world-sky tint / console
    // -----------------------------------------------------------------------
    //  We deliberately do NOT hook D3D12 draw calls / swap PSOs per draw (the
    //  crash-prone path on a DLSS title with immutable PSOs). Instead we drive
    //  the engine's OWN render objects through ProcessEvent: mesh material
    //  parameters (chams), light colours (sky/world tint), and console cvars /
    //  viewmodes. All of this runs on the GAME thread via the same safe pump as
    //  the AI work, so it never races the render thread or the engine AI tick.
    // =======================================================================
    struct FLinearColor { float R, G, B, A; };

    struct P_ExecuteConsoleCommand { UObject* WorldContext; FString Command; UObject* SpecificPlayer; };
    struct P_SetLightColor         { FLinearColor NewLightColor; };
    struct P_MeshSetVectorParam    { UE::FName Param; FVector Value; };
    struct P_MeshSetScalarParam    { UE::FName Param; float Value; };
    struct P_ActorGetComponentsByClass { UObject* ComponentClass; UE::TArray<UObject*> ReturnValue; };
    struct P_SetRenderCustomDepth  { bool bValue; };
    struct P_SetCustomDepthStencil { int32_t Value; };
    struct P_PrimSetMaterial       { int32_t ElementIndex; uint8_t Pad[4]; UObject* Material; };
    struct P_PrimCreateMID         { int32_t ElementIndex; uint8_t Pad[4]; UObject* ReturnValue; };
    struct P_PrimCreateMIDFromMaterial { int32_t ElementIndex; uint8_t Pad[4]; UObject* Parent; UObject* ReturnValue; };
    struct P_MIDSetVectorParam     { UE::FName Param; FLinearColor Value; };
    struct P_MIDSetScalarParam     { UE::FName Param; float Value; };
    static_assert(sizeof(UE::FName) == 8, "FName must be 8 bytes for the material param structs");
    static_assert(sizeof(P_ExecuteConsoleCommand) == 0x20, "ExecuteConsoleCommand params must match Dumper-7");
    static_assert(sizeof(P_SetLightColor) == 0x10, "Light.SetLightColor params must match Dumper-7");
    static_assert(sizeof(P_MeshSetVectorParam) == 0x14, "SetVectorParameterValueOnMaterials params must match Dumper-7");
    static_assert(sizeof(P_MeshSetScalarParam) == 0x0C, "SetScalarParameterValueOnMaterials params must match Dumper-7");
    static_assert(sizeof(P_ActorGetComponentsByClass) == 0x18, "Actor.K2_GetComponentsByClass params must match Dumper-7");
    static_assert(sizeof(P_PrimSetMaterial) == 0x10, "PrimitiveComponent.SetMaterial params must match Dumper-7");
    static_assert(sizeof(P_PrimCreateMID) == 0x10, "CreateAndSetMaterialInstanceDynamic params must match Dumper-7");
    static_assert(sizeof(P_PrimCreateMIDFromMaterial) == 0x18, "CreateAndSetMaterialInstanceDynamicFromMaterial params must match Dumper-7");
    static_assert(sizeof(P_MIDSetVectorParam) == 0x18, "MaterialInstanceDynamic.SetVectorParameterValue params must match Dumper-7");
    static_assert(sizeof(P_MIDSetScalarParam) == 0x0C, "MaterialInstanceDynamic.SetScalarParameterValue params must match Dumper-7");

    // Hue (0..1) -> saturated RGB. Used by chams + world tint rainbow modes.
    void HueToRgb(float t, float& r, float& g, float& b)
    {
        t -= floorf(t);
        float h = t * 6.0f;
        int   i = (int)h;
        float f = h - (float)i;
        switch (i % 6)
        {
        case 0: r = 1;     g = f;     b = 0;     break;
        case 1: r = 1 - f; g = 1;     b = 0;     break;
        case 2: r = 0;     g = 1;     b = f;     break;
        case 3: r = 0;     g = 1 - f; b = 1;     break;
        case 4: r = f;     g = 0;     b = 1;     break;
        default:r = 1;     g = 0;     b = 1 - f; break;
        }
    }

    // ---- console command executor -----------------------------------------
    // The biggest single "change the look" lever: viewmodes (wireframe / unlit),
    // show flags, r.* cvars, slomo, screenshots. One ProcessEvent, fully reliable.
    void RunConsoleCommandImpl(const std::string& cmd)
    {
        UObject*   ksl   = CachedObject(AH::Obj_KismetSystemLibrary);
        UFunction* fn    = CachedFn(AH::Fn_ExecuteConsoleCommand);
        UObject*   world = GetWorld();
        if (!Mem::IsReadable(ksl, 0x30) || !fn || !world)
        {
            LOG("Console '%s' failed: ksl=%p fn=%p world=%p", cmd.c_str(), (void*)ksl, (void*)fn, (void*)world);
            return;
        }

        // ASCII commands only -> a straight widen is correct. std::wstring::data()
        // is null-terminated and stays alive through the synchronous call.
        std::wstring w(cmd.begin(), cmd.end());
        P_ExecuteConsoleCommand p{};
        p.WorldContext = world;
        p.Command.Data  = w.data();
        p.Command.Count = (int32_t)w.size() + 1; // include the null terminator
        p.Command.Max   = p.Command.Count;
        p.SpecificPlayer = nullptr;              // null => first local player controller
        ksl->ProcessEvent(fn, &p);
        LOG("Console executed: %s", cmd.c_str());
    }

    // ---- world / sky tint via every light's colour ------------------------
    bool SetLightColorOn(UObject* lightActor, const FLinearColor& c)
    {
        UFunction* fn = CachedFn(AH::Fn_Light_SetLightColor);
        if (!Mem::IsReadable(lightActor, 0x30) || !fn)
            return false;
        P_SetLightColor p{ c };
        lightActor->ProcessEvent(fn, &p);
        return true;
    }

    // Cached list of light actors. The expensive part of world tint is NOT finding
    // the lights -- it is calling SetLightColor, which re-registers that light's
    // render state on the GPU. Doing that to hundreds of lights at 10Hz is exactly
    // what tanked frame times. So we (a) build the light list rarely (every few
    // seconds, to catch newly streamed lights), and (b) only re-apply the colour
    // when it has actually changed by a visible step. Game thread only.
    std::vector<UObject*> g_lightActors;
    ULONGLONG             g_lightListMs = 0;

    void RebuildLightListIfStale(UClass* lightCls, bool force)
    {
        ULONGLONG now = GetTickCount64();
        if (!force && !g_lightActors.empty() && now - g_lightListMs < 4000)
            return;
        g_lightListMs = now;
        if (!lightCls) { g_lightActors.clear(); return; }

        std::vector<UObject*> actors; actors.reserve(2048);
        try { CollectLevelActors(actors); } catch (...) { return; }

        std::vector<UObject*> lights; lights.reserve(256);
        for (UObject* a : actors)
        {
            if ((int)lights.size() >= 1024) break;
            if (Mem::IsReadable(a, 0x30) && a->IsA(lightCls))
                lights.push_back(a);
        }
        g_lightActors.swap(lights);
    }

    void ApplyWorldTint()
    {
        auto& st = Features::Get();
        static bool was = false;

        static UClass* lightCls = nullptr;
        if (!Mem::IsReadable(lightCls, 0x30)) lightCls = FindObjectFast(AH::Cls_Light);

        // Falling edge: reset lights to white (we don't capture per-light originals;
        // exact colours return on area reload). One pass over the cached list.
        if (!st.worldTint)
        {
            if (was)
            {
                RebuildLightListIfStale(lightCls, true);
                int reset = 0;
                FLinearColor white{ 1.0f, 1.0f, 1.0f, 1.0f };
                for (UObject* a : g_lightActors)
                    if (Mem::IsReadable(a, 0x30))
                        try { if (SetLightColorOn(a, white)) ++reset; } catch (...) {}
                LOG("World tint off: reset %d light(s) to white", reset);
                g_lightActors.clear();
                was = false;
            }
            return;
        }
        was = true;
        if (!lightCls) return;

        static ULONGLONG lastMs = 0;
        ULONGLONG now = GetTickCount64();
        if (now - lastMs < 120) return; // throttle the whole pass
        lastMs = now;

        FLinearColor c{ st.worldTintColor[0], st.worldTintColor[1], st.worldTintColor[2], 1.0f };
        if (st.worldTintRainbow)
        {
            float t = (float)(now % 10000) / 10000.0f * (st.worldTintCycle <= 0.0f ? 1.0f : st.worldTintCycle * 3.0f);
            HueToRgb(t, c.R, c.G, c.B);
        }

        // Quantise the colour to ~32 steps/channel and skip the re-apply unless it
        // changed. A STATIC colour then applies exactly once; a rainbow re-applies
        // only when the visible hue actually steps (a few dozen times per cycle),
        // instead of re-dirtying every light's render state on every pass.
        int q = (int)(c.R * 31.0f) | ((int)(c.G * 31.0f) << 5) | ((int)(c.B * 31.0f) << 10);
        static int lastQ = -1;
        RebuildLightListIfStale(lightCls, false); // refresh the cached list slowly
        if (q == lastQ) return;
        lastQ = q;

        int tinted = 0;
        for (UObject* a : g_lightActors)
        {
            if (!Mem::IsReadable(a, 0x30)) continue;
            try { if (SetLightColorOn(a, c)) ++tinted; } catch (...) {}
        }
        static ULONGLONG lastLog = 0;
        if (now - lastLog > 4000) { lastLog = now; LOG("World tint: %d cached light(s) -> %.2f %.2f %.2f", tinted, c.R, c.G, c.B); }
    }

    // ---- chams: enemy model recolour via material parameters --------------
    // Material parameter names are content-specific, so we spray a set of common
    // colour/emissive parameter names; whichever the material actually exposes
    // takes effect, the rest are harmless no-ops. FNames are resolved once (via
    // the name index) and cached. We also flag custom depth so a custom-depth
    // post-process (if present) can show the model through walls.
    std::unordered_map<UObject*, bool> g_chamsMeshes; // tracked enemy meshes (game thread only)

    const char* kChamsColorParams[]    = { "BaseColor", "Color", "Tint", "TintColor", "EmissiveColor", "BodyColor", "BaseColorTint" };
    const char* kChamsEmissiveParams[] = { "EmissiveStrength", "Emissive", "EmissivePower", "EmissiveMultiplier", "Glow", "EmissiveIntensity" };

    // Resolve-and-cache the parameter-name FNames. Returns false until the name
    // index is built (chams then simply waits, never no-ops loudly).
    bool ResolveChamsParamNames(std::vector<UE::FName>& colorOut, std::vector<UE::FName>& emissiveOut)
    {
        static std::vector<UE::FName> color;
        static std::vector<UE::FName> emissive;
        static bool resolved = false;
        if (!resolved)
        {
            for (const char* n : kChamsColorParams)    { UE::FName f{}; if (UE::TryGetFName(n, f)) color.push_back(f); }
            for (const char* n : kChamsEmissiveParams) { UE::FName f{}; if (UE::TryGetFName(n, f)) emissive.push_back(f); }
            if (!color.empty())
            {
                resolved = true;
                LOG("Chams param names resolved: %zu colour, %zu emissive", color.size(), emissive.size());
            }
        }
        colorOut = color;
        emissiveOut = emissive;
        return !color.empty();
    }

    void SetMeshVectorParam(UObject* mesh, const UE::FName& name, const FVector& v)
    {
        UFunction* fn = CachedFn(AH::Fn_MeshSetVectorParamOnMaterials);
        if (!fn) return;
        P_MeshSetVectorParam p{ name, v };
        mesh->ProcessEvent(fn, &p);
    }

    void SetMeshScalarParam(UObject* mesh, const UE::FName& name, float value)
    {
        UFunction* fn = CachedFn(AH::Fn_MeshSetScalarParamOnMaterials);
        if (!fn) return;
        P_MeshSetScalarParam p{ name, value };
        mesh->ProcessEvent(fn, &p);
    }

    void SetMeshCustomDepth(UObject* mesh, bool on, int stencil)
    {
        if (UFunction* fn = CachedFn(AH::Fn_PrimSetRenderCustomDepth))
        { P_SetRenderCustomDepth p{ on }; mesh->ProcessEvent(fn, &p); }
        if (on)
            if (UFunction* fn = CachedFn(AH::Fn_PrimSetCustomDepthStencil))
            { P_SetCustomDepthStencil p{ stencil }; mesh->ProcessEvent(fn, &p); }
    }

    struct WeaponRgbSlotState
    {
        UObject* mesh = nullptr;
        int32_t  index = 0;
        UObject* original = nullptr;
        UObject* mid = nullptr;
        bool     forcedParent = false;
        std::vector<UE::FName> realColorParams;
    };

    UObject* g_weaponRgbWeapon = nullptr;
    UObject* g_weaponRgbParent = nullptr;
    std::vector<WeaponRgbSlotState> g_weaponRgbSlots;
    std::vector<UObject*> g_weaponRgbMeshes;

    const char* kWeaponRgbColorParams[] = {
        "EmissiveColor", "Emissive_Color", "Emissive Color",
        "Color", "BaseColor", "Base Color", "Tint", "TintColor",
        "Tint Color", "Simple Tint", "SelectionColor", "Color_Overlay",
        "Color_Mult", "Diffuse", "Albedo", "MainColor", "MaterialColor",
        "PrimaryColor", "EmissionColor", "EmissiveTint", "Emissive Tint",
        "GlowColor"
    };
    const char* kWeaponRgbScalarParams[] = {
        "EmissiveStrength", "EmissiveIntensity", "Emissive_Intensity",
        "EmissivePower", "EmissiveMultiplier", "Emissive Power", "Emissive",
        "Glow", "Glowing", "Power", "Intensity", "EdgeGlow", "Emission Depth"
    };
    const char* kWeaponRgbOpacityParams[] = {
        "Opacity", "Alpha"
    };
    const char* kWeaponRgbForcedColorParams[] = {
        "Color", "BaseColor", "EmissiveColor", "TintColor", "MaterialColor", "SelectionColor"
    };
    const char* kWeaponRgbForcedScalarParams[] = {
        "EmissiveStrength", "EmissiveIntensity", "Emissive"
    };

    void PushUniqueFName(std::vector<UE::FName>& out, const UE::FName& name)
    {
        for (const UE::FName& existing : out)
            if (existing.ComparisonIndex == name.ComparisonIndex && existing.Number == name.Number)
                return;
        out.push_back(name);
    }

    bool ResolveWeaponRgbParamNames(std::vector<UE::FName>& colorOut, std::vector<UE::FName>& scalarOut)
    {
        static std::vector<UE::FName> color;
        static std::vector<UE::FName> scalar;
        static bool resolved = false;
        if (!resolved)
        {
            for (const char* n : kWeaponRgbColorParams)
            {
                UE::FName f{};
                if (UE::TryGetFName(n, f))
                    PushUniqueFName(color, f);
            }
            for (const char* n : kWeaponRgbScalarParams)
            {
                UE::FName f{};
                if (UE::TryGetFName(n, f))
                    PushUniqueFName(scalar, f);
            }
            if (!color.empty())
            {
                resolved = true;
                LOG("WeaponRGB params resolved: %zu colour, %zu scalar", color.size(), scalar.size());
            }
        }
        colorOut = color;
        scalarOut = scalar;
        return !color.empty();
    }

    bool ResolveWeaponRgbOpacityParamNames(std::vector<UE::FName>& opacityOut)
    {
        static std::vector<UE::FName> opacity;
        static bool resolved = false;
        if (!resolved)
        {
            for (const char* n : kWeaponRgbOpacityParams)
            {
                UE::FName f{};
                if (UE::TryGetFName(n, f))
                    PushUniqueFName(opacity, f);
            }
            resolved = !opacity.empty();
        }
        opacityOut = opacity;
        return !opacity.empty();
    }

    void ResolveWeaponRgbForcedParamNames(std::vector<UE::FName>& colorOut, std::vector<UE::FName>& scalarOut)
    {
        static std::vector<UE::FName> color;
        static std::vector<UE::FName> scalar;
        static bool resolved = false;
        if (!resolved)
        {
            for (const char* n : kWeaponRgbForcedColorParams)
            {
                UE::FName f{};
                if (UE::TryGetFName(n, f))
                    PushUniqueFName(color, f);
            }
            for (const char* n : kWeaponRgbForcedScalarParams)
            {
                UE::FName f{};
                if (UE::TryGetFName(n, f))
                    PushUniqueFName(scalar, f);
            }
            resolved = true;
        }
        colorOut = color;
        scalarOut = scalar;
    }

    bool IsUsableMaterialPtr(UObject* mat)
    {
        return Mem::IsReadable(mat, sizeof(void*));
    }

    UObject* ResolveWeaponRgbParentMaterial()
    {
        static UObject* cached = nullptr;
        if (IsUsableMaterialPtr(cached))
            return cached;

        const char* candidates[] = {
            AH::Obj_WeaponRgbScannerSelectMaterial,
            AH::Obj_WeaponRgbGameEmissiveMaterial,
            AH::Obj_WeaponRgbTurretEmissiveMaterial,
            AH::Obj_WeaponRgbBasicShapeMaterial,
            AH::Obj_WeaponRgbDebugMaterial,
            AH::Obj_WeaponRgbDefaultMaterial,
            AH::Obj_WeaponRgbWorldGridMaterial
        };
        static bool loggedCandidates = false;
        for (const char* name : candidates)
        {
            UObject* mat = CachedObject(name);
            bool usable = IsUsableMaterialPtr(mat);
            if (!loggedCandidates && mat)
            {
                LOG("WeaponRGB: parent candidate %s -> %p usable=%d strict=%d",
                    name, (void*)mat, usable ? 1 : 0, Mem::IsReadable(mat, 0x30) ? 1 : 0);
            }
            if (usable)
            {
                cached = mat;
                loggedCandidates = true;
                return cached;
            }
        }
        loggedCandidates = true;
        return nullptr;
    }

    int GetMeshMaterialCount(UObject* mesh)
    {
        UFunction* fn = CachedFn(AH::Fn_PrimGetNumMaterials);
        if (!Mem::IsReadable(mesh, 0x30) || !fn)
            return 0;
        struct { int32_t Ret; } p{};
        mesh->ProcessEvent(fn, &p);
        if (p.Ret < 0) return 0;
        if (p.Ret > 32) return 32;
        return p.Ret;
    }

    UObject* GetMeshMaterialSlot(UObject* mesh, int32_t slot)
    {
        UFunction* fn = CachedFn(AH::Fn_PrimGetMaterial);
        if (!Mem::IsReadable(mesh, 0x30) || !fn)
            return nullptr;
        struct { int32_t ElementIndex; uint8_t Pad[4]; UObject* Ret; } p{};
        p.ElementIndex = slot;
        mesh->ProcessEvent(fn, &p);
        return Mem::IsReadable(p.Ret, 0x30) ? p.Ret : nullptr;
    }

    void PushUniqueObject(std::vector<UObject*>& out, UObject* obj)
    {
        if (!Mem::IsReadable(obj, 0x30))
            return;
        for (UObject* existing : out)
            if (existing == obj)
                return;
        out.push_back(obj);
    }

    UClass* ResolveMeshComponentClass()
    {
        static UClass* cls = nullptr;
        if (!Mem::IsReadable(cls, 0x30))
            cls = FindObjectFast(AH::Cls_MeshComponent);
        return Mem::IsReadable(cls, 0x30) ? cls : nullptr;
    }

    bool IsRenderableMeshComponent(UObject* obj)
    {
        if (!Mem::IsReadable(obj, 0x30))
            return false;
        UClass* meshCls = ResolveMeshComponentClass();
        if (!meshCls)
            return false;
        try
        {
            if (!obj->IsA(meshCls))
                return false;
        }
        catch (...) { return false; }
        try { return GetMeshMaterialCount(obj) > 0; }
        catch (...) { return false; }
    }

    void AddWeaponRgbMeshComponent(std::vector<UObject*>& out, UObject* obj)
    {
        if (IsRenderableMeshComponent(obj))
            PushUniqueObject(out, obj);
    }

    void AddKnownWeaponMeshComponent(std::vector<UObject*>& out, UObject* obj)
    {
        if (!Mem::IsReadable(obj, 0x30))
            return;
        try
        {
            if (GetMeshMaterialCount(obj) > 0)
                PushUniqueObject(out, obj);
        }
        catch (...) {}
    }

    void CollectWeaponRgbMeshComponents(UObject* weapon, std::vector<UObject*>& out)
    {
        out.clear();
        if (!Mem::IsReadable(weapon, 0x30))
            return;

        uint8_t* b = reinterpret_cast<uint8_t*>(weapon);
        if (Mem::IsReadable(b + AH::Weapon_Mesh, sizeof(void*)))
            AddKnownWeaponMeshComponent(out, *reinterpret_cast<UObject**>(b + AH::Weapon_Mesh));

        UFunction* fn = CachedFn(AH::Fn_ActorGetComponentsByClass);
        UClass* meshCls = ResolveMeshComponentClass();
        if (fn && meshCls)
        {
            try
            {
                P_ActorGetComponentsByClass p{};
                p.ComponentClass = meshCls;
                weapon->ProcessEvent(fn, &p);
                int count = p.ReturnValue.Count;
                if (count < 0) count = 0;
                if (count > 64) count = 64;
                if (Mem::IsReadable(p.ReturnValue.Data, (size_t)count * sizeof(UObject*)))
                    for (int i = 0; i < count; ++i)
                        AddWeaponRgbMeshComponent(out, p.ReturnValue.Data[i]);
            }
            catch (...) {}
        }

        // Fallback for BP-owned mesh component fields not returned by the actor helper.
        for (int off = 0; off + 8 <= 0x1000 && out.size() < 16; off += 8)
        {
            if (!Mem::IsReadable(b + off, sizeof(void*)))
                continue;
            AddWeaponRgbMeshComponent(out, *reinterpret_cast<UObject**>(b + off));
        }
    }

    bool SetMeshMaterialSlot(UObject* mesh, int32_t slot, UObject* material)
    {
        UFunction* fn = CachedFn(AH::Fn_PrimSetMaterial);
        if (!Mem::IsReadable(mesh, 0x30) || !Mem::IsReadable(material, 0x30) || !fn)
            return false;
        P_PrimSetMaterial p{};
        p.ElementIndex = slot;
        p.Material = material;
        mesh->ProcessEvent(fn, &p);
        return true;
    }

    UObject* CreateWeaponRgbMID(UObject* mesh, int32_t slot, UObject* parent)
    {
        UFunction* fn = CachedFn(AH::Fn_PrimCreateMIDFromMaterial);
        if (!Mem::IsReadable(mesh, 0x30) || !IsUsableMaterialPtr(parent) || !fn)
            return nullptr;
        P_PrimCreateMIDFromMaterial p{};
        p.ElementIndex = slot;
        p.Parent = parent;
        mesh->ProcessEvent(fn, &p);
        return Mem::IsReadable(p.ReturnValue, 0x30) ? p.ReturnValue : nullptr;
    }

    UObject* CreateWeaponRgbOriginalMID(UObject* mesh, int32_t slot)
    {
        UFunction* fn = CachedFn(AH::Fn_PrimCreateMID);
        if (!Mem::IsReadable(mesh, 0x30) || !fn)
            return nullptr;
        P_PrimCreateMID p{};
        p.ElementIndex = slot;
        mesh->ProcessEvent(fn, &p);
        return Mem::IsReadable(p.ReturnValue, 0x30) ? p.ReturnValue : nullptr;
    }

    void SetMIDVectorParam(UObject* mid, const UE::FName& name, const FLinearColor& value)
    {
        UFunction* fn = CachedFn(AH::Fn_MIDSetVectorParam);
        if (!Mem::IsReadable(mid, 0x30) || !fn)
            return;
        P_MIDSetVectorParam p{ name, value };
        mid->ProcessEvent(fn, &p);
    }

    void SetMIDScalarParam(UObject* mid, const UE::FName& name, float value)
    {
        UFunction* fn = CachedFn(AH::Fn_MIDSetScalarParam);
        if (!Mem::IsReadable(mid, 0x30) || !fn)
            return;
        P_MIDSetScalarParam p{ name, value };
        mid->ProcessEvent(fn, &p);
    }

    void CollectMaterialVectorParamNames(UObject* mat, std::vector<UE::FName>& out)
    {
        if (!Mem::IsReadable(mat, AH::Mat_VectorParameterValues + 0x10))
            return;
        static UClass* miCls = nullptr;
        if (!Mem::IsReadable(miCls, 0x30))
            miCls = FindObjectFast(AH::Cls_MaterialInstance);
        if (miCls && !mat->IsA(miCls))
            return;

        uint8_t* b = reinterpret_cast<uint8_t*>(mat);
        uint8_t* data = *reinterpret_cast<uint8_t**>(b + AH::Mat_VectorParameterValues);
        int32_t count = *reinterpret_cast<int32_t*>(b + AH::Mat_VectorParameterValues + 8);
        if (count <= 0 || count > 64)
            return;
        if (!Mem::IsReadable(data, (size_t)count * AH::VectorParamValue_Stride))
            return;

        for (int k = 0; k < count; ++k)
        {
            UE::FName nm = *reinterpret_cast<UE::FName*>(data + (size_t)k * AH::VectorParamValue_Stride + AH::VectorParamValue_NameOff);
            PushUniqueFName(out, nm);
        }
    }

    void RestoreWeaponRgbMaterials()
    {
        int restored = 0;
        for (const WeaponRgbSlotState& slot : g_weaponRgbSlots)
        {
            if (Mem::IsReadable(slot.mesh, 0x30) &&
                Mem::IsReadable(slot.original, 0x30) &&
                SetMeshMaterialSlot(slot.mesh, slot.index, slot.original))
                ++restored;
        }
        if (!g_weaponRgbSlots.empty())
            LOG("WeaponRGB: restored %d/%zu original material slot(s)", restored, g_weaponRgbSlots.size());
        g_weaponRgbSlots.clear();
        g_weaponRgbMeshes.clear();
        g_weaponRgbWeapon = nullptr;
        g_weaponRgbParent = nullptr;
    }

    bool EnsureWeaponRgbMaterialOverride(UObject* weapon, const std::vector<UObject*>& meshes, UObject* forcedParent)
    {
        if (!Mem::IsReadable(weapon, 0x30) || meshes.empty())
            return false;
        if (weapon == g_weaponRgbWeapon && forcedParent == g_weaponRgbParent && !g_weaponRgbSlots.empty())
        {
            bool midsReadable = true;
            for (const WeaponRgbSlotState& slot : g_weaponRgbSlots)
            {
                if (!Mem::IsReadable(slot.mesh, 0x30) || !Mem::IsReadable(slot.mid, 0x30))
                {
                    midsReadable = false;
                    break;
                }
            }
            if (midsReadable)
                return true;
        }

        RestoreWeaponRgbMaterials();

        std::vector<UE::FName> guessedColorParams, ignoredScalarParams;
        ResolveWeaponRgbParamNames(guessedColorParams, ignoredScalarParams);
        constexpr size_t kMaxWeaponRgbSlots = 16;
        constexpr size_t kMaxWeaponRgbParamsPerSlot = 6;

        std::vector<WeaponRgbSlotState> slots;
        slots.reserve(meshes.size() * 2);
        for (UObject* mesh : meshes)
        {
            if (slots.size() >= kMaxWeaponRgbSlots)
                break;
            if (!Mem::IsReadable(mesh, 0x30))
                continue;

            int count = GetMeshMaterialCount(mesh);
            if (count <= 0)
                continue;

            for (int32_t i = 0; i < count; ++i)
            {
                if (slots.size() >= kMaxWeaponRgbSlots)
                    break;
                UObject* original = GetMeshMaterialSlot(mesh, i);
                if (!Mem::IsReadable(original, 0x30))
                    continue;

                std::vector<UE::FName> realColorParams;
                try { CollectMaterialVectorParamNames(original, realColorParams); } catch (...) {}

                bool forceSlot = realColorParams.empty() && IsUsableMaterialPtr(forcedParent);
                UObject* mid = forceSlot ? CreateWeaponRgbMID(mesh, i, forcedParent)
                                         : CreateWeaponRgbOriginalMID(mesh, i);
                if (!Mem::IsReadable(mid, 0x30))
                {
                    SetMeshMaterialSlot(mesh, i, original);
                    continue;
                }

                if (realColorParams.empty())
                {
                    try { CollectMaterialVectorParamNames(mid, realColorParams); } catch (...) {}
                }
                // Set parameters on this slot's MID directly. Real instance names
                // take priority; a small guessed fallback covers parent parameters
                // that are not present in the instance override array.
                for (const UE::FName& name : guessedColorParams)
                    PushUniqueFName(realColorParams, name);
                if (realColorParams.size() > kMaxWeaponRgbParamsPerSlot)
                    realColorParams.resize(kMaxWeaponRgbParamsPerSlot);

                WeaponRgbSlotState slot{};
                slot.mesh = mesh;
                slot.index = i;
                slot.original = original;
                slot.mid = mid;
                slot.forcedParent = forceSlot;
                slot.realColorParams.swap(realColorParams);
                slots.push_back(slot);
            }
        }

        if (slots.empty())
            return false;

        g_weaponRgbWeapon = weapon;
        g_weaponRgbParent = forcedParent;
        g_weaponRgbMeshes = meshes;
        g_weaponRgbSlots.swap(slots);

        int forced = 0;
        for (const WeaponRgbSlotState& slot : g_weaponRgbSlots)
            if (slot.forcedParent)
                ++forced;
        std::string parentName;
        try { if (forcedParent) parentName = forcedParent->GetFullName(); } catch (...) {}
        LOG("WeaponRGB: override active weapon=%p meshes=%zu slots=%zu forced=%d parent=%p %s",
            (void*)weapon, g_weaponRgbMeshes.size(), g_weaponRgbSlots.size(), forced, (void*)forcedParent, parentName.c_str());
        return true;
    }

    bool ApplyWeaponRgbOverrideColor(const FVector& color)
    {
        std::vector<UE::FName> forcedColorNames, forcedScalarNames;
        ResolveWeaponRgbForcedParamNames(forcedColorNames, forcedScalarNames);

        bool hasAnyVectorTarget = false;
        bool hasForcedSlot = false;
        for (const WeaponRgbSlotState& slot : g_weaponRgbSlots)
        {
            if (!slot.realColorParams.empty())
                hasAnyVectorTarget = true;
            if (slot.forcedParent)
                hasForcedSlot = true;
        }
        if (!hasAnyVectorTarget)
            hasAnyVectorTarget = hasForcedSlot && !forcedColorNames.empty();
        if (!hasAnyVectorTarget)
            return false;

        FLinearColor linear{ color.X, color.Y, color.Z, 1.0f };
        constexpr float kGlowPower = 8.0f;

        for (const WeaponRgbSlotState& slot : g_weaponRgbSlots)
        {
            if (!Mem::IsReadable(slot.mid, 0x30))
                continue;
            for (const UE::FName& n : slot.realColorParams)
                SetMIDVectorParam(slot.mid, n, linear);
            if (slot.forcedParent)
            {
                for (const UE::FName& n : forcedColorNames)
                    SetMIDVectorParam(slot.mid, n, linear);
                for (const UE::FName& n : forcedScalarNames)
                    SetMIDScalarParam(slot.mid, n, kGlowPower);
            }
        }
        return true;
    }

    UObject* EnemyMesh(UObject* ai)
    {
        uint8_t* b = reinterpret_cast<uint8_t*>(ai);
        if (!Mem::IsReadable(b + AH::Char_Mesh, sizeof(void*)))
            return nullptr;
        UObject* mesh = *reinterpret_cast<UObject**>(b + AH::Char_Mesh);
        return Mem::IsReadable(mesh, 0x30) ? mesh : nullptr;
    }

    // Read the mesh's material instances' OWN vector-parameter names (live FNames).
    // This is the CONFIDENT recolor: instead of guessing names (BaseColor/Tint/...)
    // that the material may not expose, we set whatever colour params it actually has.
    void CollectMeshVectorParamNames(UObject* mesh, std::vector<UE::FName>& out)
    {
        UFunction* numFn = CachedFn(AH::Fn_PrimGetNumMaterials);
        UFunction* getFn = CachedFn(AH::Fn_PrimGetMaterial);
        if (!Mem::IsReadable(mesh, 0x30) || !numFn || !getFn) return;
        static UClass* miCls = nullptr;
        if (!Mem::IsReadable(miCls, 0x30)) miCls = FindObjectFast(AH::Cls_MaterialInstance);

        struct { int32_t Ret; } np{};
        mesh->ProcessEvent(numFn, &np);
        int n = np.Ret;
        if (n < 0) n = 0; if (n > 16) n = 16;
        for (int i = 0; i < n; ++i)
        {
            struct { int32_t ElementIndex; uint8_t pad[4]; void* Ret; } gp{};
            gp.ElementIndex = i;
            mesh->ProcessEvent(getFn, &gp);
            UObject* mat = static_cast<UObject*>(gp.Ret);
            if (!Mem::IsReadable(mat, AH::Mat_VectorParameterValues + 0x10)) continue;
            if (miCls && !mat->IsA(miCls)) continue; // only material INSTANCES carry the override arrays
            uint8_t* b = reinterpret_cast<uint8_t*>(mat);
            uint8_t* data = *reinterpret_cast<uint8_t**>(b + AH::Mat_VectorParameterValues);
            int32_t count = *reinterpret_cast<int32_t*>(b + AH::Mat_VectorParameterValues + 8);
            if (count <= 0 || count > 64) continue;
            if (!Mem::IsReadable(data, (size_t)count * AH::VectorParamValue_Stride)) continue;
            for (int k = 0; k < count; ++k)
            {
                UE::FName nm = *reinterpret_cast<UE::FName*>(data + (size_t)k * AH::VectorParamValue_Stride + AH::VectorParamValue_NameOff);
                bool dup = false;
                for (const UE::FName& e : out) if (e.ComparisonIndex == nm.ComparisonIndex) { dup = true; break; }
                if (!dup) out.push_back(nm);
            }
        }
    }

    void ApplyChamsToMesh(UObject* mesh, const FVector& color, float emissive, bool throughWalls,
                          const std::vector<UE::FName>& colorNames, const std::vector<UE::FName>& emissiveNames)
    {
        // CONFIDENT path: set the material's REAL vector params (read from the instance),
        // so the recolour actually lands even when our guessed names don't match.
        std::vector<UE::FName> realNames;
        try { CollectMeshVectorParamNames(mesh, realNames); } catch (...) {}
        for (const UE::FName& n : realNames)     SetMeshVectorParam(mesh, n, color);
        // Plus the common guessed names (harmless if absent) as a belt-and-suspenders.
        for (const UE::FName& n : colorNames)    SetMeshVectorParam(mesh, n, color);
        for (const UE::FName& n : emissiveNames) SetMeshScalarParam(mesh, n, emissive);
        SetMeshCustomDepth(mesh, throughWalls, 252);
    }

    void RestoreChams()
    {
        std::vector<UE::FName> colorNames, emissiveNames;
        ResolveChamsParamNames(colorNames, emissiveNames);
        FVector white{ 1.0f, 1.0f, 1.0f };
        int restored = 0;
        for (auto& kv : g_chamsMeshes)
        {
            UObject* mesh = kv.first;
            if (!Mem::IsReadable(mesh, 0x30)) continue;
            try
            {
                for (const UE::FName& n : colorNames)    SetMeshVectorParam(mesh, n, white);
                for (const UE::FName& n : emissiveNames) SetMeshScalarParam(mesh, n, 0.0f);
                SetMeshCustomDepth(mesh, false, 0);
                ++restored;
            }
            catch (...) {}
        }
        g_chamsMeshes.clear();
        LOG("Chams off: restored %d mesh(es)", restored);
    }

    void ApplyChams()
    {
        auto& st = Features::Get();
        static bool was = false;

        if (!st.chamsEnabled)
        {
            if (was) { RestoreChams(); was = false; }
            return;
        }
        was = true;

        static ULONGLONG lastMs = 0;
        ULONGLONG now = GetTickCount64();
        if (now - lastMs < 90) return;
        lastMs = now;

        std::vector<UE::FName> colorNames, emissiveNames;
        if (!ResolveChamsParamNames(colorNames, emissiveNames))
            return; // name index not built yet -- wait, don't thrash

        FVector color{ st.chamsColor[0], st.chamsColor[1], st.chamsColor[2] };
        if (st.chamsRainbow)
            HueToRgb((float)(now % 4000) / 4000.0f, color.X, color.Y, color.Z);

        // Nearest-first (the cache is distance-sorted), capped so a big crowd never
        // stalls the game thread.
        std::vector<UObject*> targets = CollectAllCachedAi(kMaxCachedAiActors);
        int processed = 0;
        for (UObject* ai : targets)
        {
            if (processed >= 40) break;
            if (!AiUsable(ai)) continue;
            UObject* mesh = EnemyMesh(ai);
            if (!mesh) continue;
            try
            {
                ApplyChamsToMesh(mesh, color, st.chamsEmissive, st.chamsThroughWalls, colorNames, emissiveNames);
                g_chamsMeshes[mesh] = true;
                ++processed;
            }
            catch (...) {}
        }

        // Prune dead meshes occasionally so the tracking map can't grow forever.
        if (g_chamsMeshes.size() > 512)
        {
            for (auto it = g_chamsMeshes.begin(); it != g_chamsMeshes.end(); )
            {
                if (Mem::IsReadable(it->first, 0x30)) ++it;
                else it = g_chamsMeshes.erase(it);
            }
        }
    }

    // ---- visual game-thread pump (mirror of the AI pump) ------------------
    std::atomic<bool> g_visualPumpInFlight{ false };

    UObject* GetCurrentWeaponObject(UObject* pawn); // fwd: defined later, used by weapon RGB

    // RGB / recolour the equipped weapon with one MID per material slot. Setup is
    // paid only when the weapon changes; colour updates address each MID directly.
    // This avoids MeshComponent.SetVectorParameterValueOnMaterials, whose nested
    // mesh x parameter x material loops caused severe frame-time jitter on the AK
    // and shotgun. Exact original material pointers are retained and restored.
    void ApplyWeaponRgb()
    {
        auto& st = Features::Get();
        static bool was = false;
        static bool haveAppliedColor = false;
        static FVector lastAppliedColor{};
        static UObject* appliedWeapon = nullptr;

        if (!st.weaponRgb)
        {
            if (was)
            {
                RestoreWeaponRgbMaterials();
                was = false;
                haveAppliedColor = false;
                appliedWeapon = nullptr;
            }
            return;
        }
        was = true;

        // 12 Hz is visually smooth enough for a rainbow while keeping game-thread
        // ProcessEvent traffic bounded on multi-material firearms.
        static ULONGLONG lastMs = 0;
        ULONGLONG now = GetTickCount64();
        if (now - lastMs < 85) return;
        lastMs = now;

        UObject* pawn = GetLocalPawn();
        UObject* weapon = pawn ? GetCurrentWeaponObject(pawn) : nullptr;
        if (!Mem::IsReadable(weapon, 0x30))
            return; // holstered / scripted state -> nothing to recolour this tick

        if (weapon != g_weaponRgbWeapon || g_weaponRgbSlots.empty())
        {
            std::vector<UObject*> meshes;
            try { CollectWeaponRgbMeshComponents(weapon, meshes); } catch (...) {}
            if (!EnsureWeaponRgbMaterialOverride(weapon, meshes, nullptr))
                return;
            haveAppliedColor = false;
            appliedWeapon = weapon;
        }

        FVector color{ st.weaponRgbColor[0], st.weaponRgbColor[1], st.weaponRgbColor[2] };
        if (st.weaponRgbRainbow)
            HueToRgb((float)(now % 3000) / 3000.0f, color.X, color.Y, color.Z);

        if (!st.weaponRgbRainbow && haveAppliedColor && appliedWeapon == weapon &&
            fabsf(color.X - lastAppliedColor.X) < 0.001f &&
            fabsf(color.Y - lastAppliedColor.Y) < 0.001f &&
            fabsf(color.Z - lastAppliedColor.Z) < 0.001f)
            return; // fixed colour already applied; zero recurring game-thread work

        if (ApplyWeaponRgbOverrideColor(color))
        {
            lastAppliedColor = color;
            haveAppliedColor = true;
            appliedWeapon = weapon;
        }
    }

    void DrainVisualGameThreadWork()
    {
        ApplyWorldTint();  // light colours
        ApplyChams();      // enemy model recolour
        ApplyWeaponRgb();  // equipped-weapon recolour / rainbow
    }

    // Render thread: schedule ONE bounded visual pump at a time onto the game
    // thread (same mechanism + safety as the AI pump). A short grace window after
    // the toggles turn off lets the falling-edge restore (reset lights / un-cham)
    // actually run once.
    void ScheduleVisualGameThreadWork()
    {
        auto& st = Features::Get();
        bool active = st.chamsEnabled || st.worldTint || st.weaponRgb;

        static ULONGLONG lastActiveMs = 0;
        ULONGLONG now = GetTickCount64();
        if (active) lastActiveMs = now;
        if (!active && now - lastActiveMs > 1500)
            return;

        if (g_visualPumpInFlight.load())
            return;
        if (!InstallProcessEventHook())
            return; // no game thread yet -- never touch render objects from Present

        static ULONGLONG lastQueueMs = 0;
        if (now - lastQueueMs < 60)
            return;
        lastQueueMs = now;

        g_visualPumpInFlight = true;
        QueueGameThread([]()
        {
            try { DrainVisualGameThreadWork(); }
            catch (...) {}
            g_visualPumpInFlight = false;
        });
    }

    // ---- world: global time dilation --------------------------------------
    bool SetTimeDilation(float scale)
    {
        UObject* gs = CachedObject(AH::Obj_GameplayStatics);     // no-scan (name index)
        UFunction* fn = CachedFn(AH::Fn_SetGlobalTimeDilation);  // prewarmed on the worker thread
        UObject* world = GetWorld();
        if (!gs || !fn || !world) return false;
        struct { UObject* ctx; float dil; } p{ world, scale };
        gs->ProcessEvent(fn, &p);
        return true;
    }

    void ApplyTimeDilation()
    {
        auto& st = Features::Get();
        if (st.bulletTime)
            return; // bullet time owns global time dilation while active
        static bool  wasOn = false;
        static float lastApplied = -1.0f;
        static ULONGLONG lastMs = 0;
        if (!st.timeDilation)
        {
            if (wasOn) { SetTimeDilation(1.0f); wasOn = false; lastApplied = 1.0f; LOG("Time dilation reset to 1.0"); }
            return;
        }
        ULONGLONG now = GetTickCount64();
        if (st.timeScale != lastApplied || now - lastMs > 1000)
        {
            if (SetTimeDilation(st.timeScale)) { lastApplied = st.timeScale; wasOn = true; }
            lastMs = now;
        }
    }

    void RefreshFlyStreaming(UObject* pawn, bool force)
    {
        if (!Features::Get().flyStreamingAssist)
            return;

        static ULONGLONG lastUpdateMs = 0;
        static ULONGLONG lastLogMs = 0;
        ULONGLONG nowMs = GetTickCount64();
        if (!force && nowMs - lastUpdateMs < 1000)
            return;

        lastUpdateMs = nowMs;
        ULONGLONG startMs = GetTickCount64();
        bool invalidated = InvalidateStreaming(pawn);
        bool enabled = EnableLevelStreamingUpdate();
        ULONGLONG elapsedMs = GetTickCount64() - startMs;

        if (elapsedMs > 50)
            LOG("Fly streaming assist slow: %llums", elapsedMs);

        if (force || nowMs - lastLogMs > 5000)
        {
            LOG("Fly streaming assist: invalidate=%s updateVolumes=%s pawn=%p",
                invalidated ? "yes" : "no",
                enabled ? "yes" : "no",
                (void*)pawn);
            lastLogMs = nowMs;
        }
    }

    FVector Add(const FVector& a, const FVector& b)
    {
        return { a.X + b.X, a.Y + b.Y, a.Z + b.Z };
    }

    FVector Scale(const FVector& v, float s)
    {
        return { v.X * s, v.Y * s, v.Z * s };
    }

    float Length(const FVector& v)
    {
        return sqrtf(v.X * v.X + v.Y * v.Y + v.Z * v.Z);
    }

    FVector Normalize(const FVector& v)
    {
        float len = Length(v);
        if (len <= 0.001f) return {};
        return Scale(v, 1.0f / len);
    }

    // ---- GAS attribute helpers (FGameplayAttributeData) -------------------
    inline bool SetAttr(uint8_t* set, int attrOff, float val)
    {
        if (!Mem::IsReadable(set + attrOff + AH::Attr_CurrentValue, 4)) return false;
        *reinterpret_cast<float*>(set + attrOff + AH::Attr_BaseValue)    = val;
        *reinterpret_cast<float*>(set + attrOff + AH::Attr_CurrentValue) = val;
        return true;
    }
    inline bool ReadAttr(uint8_t* set, int attrOff, float& base, float& current)
    {
        if (!Mem::IsReadable(set + attrOff + AH::Attr_CurrentValue, 4)) return false;
        base = *reinterpret_cast<float*>(set + attrOff + AH::Attr_BaseValue);
        current = *reinterpret_cast<float*>(set + attrOff + AH::Attr_CurrentValue);
        return true;
    }
    inline float GetAttrCur(uint8_t* set, int attrOff)
    {
        if (!Mem::IsReadable(set + attrOff + AH::Attr_CurrentValue, 4)) return 0.f;
        return *reinterpret_cast<float*>(set + attrOff + AH::Attr_CurrentValue);
    }

    bool IsStaleGodMultiplier(float base, float current)
    {
        return base <= 0.001f || current <= 0.001f;
    }

    bool IsStaleOneHitMultiplier(float base, float current)
    {
        return base >= 900.0f || current >= 900.0f;
    }

    bool RestoreAttrBackup(AttrBackup& backup, const char* label)
    {
        if (!backup.valid)
            return false;

        if (Mem::IsReadable(backup.set + backup.attrOff + AH::Attr_CurrentValue, 4))
        {
            *reinterpret_cast<float*>(backup.set + backup.attrOff + AH::Attr_BaseValue) = backup.base;
            *reinterpret_cast<float*>(backup.set + backup.attrOff + AH::Attr_CurrentValue) = backup.current;
            LOG("%s restored to base=%.3f current=%.3f", label, backup.base, backup.current);
            backup = {};
            return true;
        }

        LOG("%s restore skipped: target no longer readable", label);
        backup = {};
        return false;
    }

    void CaptureAttrBackup(AttrBackup& backup, uint8_t* set, int attrOff, const char* label)
    {
        if (!set)
            return;

        if (backup.valid && backup.set != set)
            RestoreAttrBackup(backup, label);

        if (backup.valid)
            return;

        float base = 0.0f;
        float current = 0.0f;
        if (ReadAttr(set, attrOff, base, current))
        {
            backup.set = set;
            backup.attrOff = attrOff;
            backup.valid = true;
            backup.base = base;
            backup.current = current;
            LOG("%s backup captured base=%.3f current=%.3f", label, base, current);
        }
    }

    void CaptureDamageAttrBackup(AttrBackup& backup, uint8_t* set, int attrOff, const char* label, bool oneHit)
    {
        if (!set)
            return;

        if (backup.valid && backup.set != set)
            RestoreAttrBackup(backup, label);

        if (backup.valid)
            return;

        float base = 0.0f;
        float current = 0.0f;
        if (!ReadAttr(set, attrOff, base, current))
            return;

        bool staleCheatValue = oneHit ? IsStaleOneHitMultiplier(base, current) : IsStaleGodMultiplier(base, current);
        backup.set = set;
        backup.attrOff = attrOff;
        backup.valid = true;
        backup.base = staleCheatValue ? kNormalDamageMultiplier : base;
        backup.current = staleCheatValue ? kNormalDamageMultiplier : current;

        if (staleCheatValue)
        {
            LOG("%s backup sanitized from stale cheat value base=%.3f current=%.3f -> %.3f",
                label, base, current, kNormalDamageMultiplier);
        }
        else
        {
            LOG("%s backup captured base=%.3f current=%.3f", label, base, current);
        }
    }

    bool NormalizeDisabledDamageAttr(uint8_t* set, int attrOff, const char* label, bool oneHit)
    {
        float base = 0.0f;
        float current = 0.0f;
        if (!ReadAttr(set, attrOff, base, current))
            return false;

        bool staleCheatValue = oneHit ? IsStaleOneHitMultiplier(base, current) : IsStaleGodMultiplier(base, current);
        if (!staleCheatValue)
            return false;

        if (SetAttr(set, attrOff, kNormalDamageMultiplier))
        {
            LOG("%s disabled-state normalized stale cheat value base=%.3f current=%.3f -> %.3f",
                label, base, current, kNormalDamageMultiplier);
            return true;
        }
        return false;
    }

    bool ApplyLookInputBlock(UObject* pc, bool block, bool logResult)
    {
        if (!pc) return false;

        UFunction* setLook = CachedFn(AH::Fn_SetIgnoreLookInput);
        if (!setLook) return false;

        P_SetIgnoreLookInput look{ block };
        pc->ProcessEvent(setLook, &look);
        if (logResult)
            LOG("Game look input %s", block ? "blocked" : "unblocked");
        return true;
    }

    bool ApplyMoveInputBlock(UObject* pc, bool block, bool logResult)
    {
        if (!pc) return false;

        UFunction* setMove = CachedFn(AH::Fn_SetIgnoreMoveInput);
        if (!setMove) return false;

        P_SetIgnoreMoveInput move{ block };
        pc->ProcessEvent(setMove, &move);
        if (logResult)
            LOG("Game move input %s", block ? "blocked" : "unblocked");
        return true;
    }

    void UpdateGameInputBlock()
    {
        // VANILLA-STATE RULE: with nothing enabled, the menu must not touch the game.
        // We never call SetIgnore*Input(true) ourselves, so we must NOT poke the
        // input-ignore counters either. The old code decremented them every second
        // unconditionally -- that mutated vanilla state and could fight the game's
        // own input locks during an ability cast / cutscene (the "shock / V ability
        // stopped working" report). Now we only clear ONCE on the falling edge of
        // fly/noclip, purely defensively, and otherwise leave input completely alone.
        auto& st = Features::Get();
        static bool wasFreeFly = false;
        bool freeFly = st.flyHack || st.noclip;
        if (wasFreeFly && !freeFly)
        {
            if (UObject* pc = GetPlayerController())
            {
                ApplyLookInputBlock(pc, false, false);
                ApplyMoveInputBlock(pc, false, false);
            }
        }
        wasFreeFly = freeFly;
    }

    bool ApplyMinecraftFly(UObject* pawn, const FVector& currentLoc, float dt)
    {
        auto& st = Features::Get();
        if (!(st.flyHack || st.noclip) || G::menuOpen.load())
            return false;

        FRotator rot{};
        if (!GetControlRotation(rot))
            return false;

        constexpr float kDegToRad = 0.01745329251994329577f;
        float yaw = rot.Yaw * kDegToRad;
        FVector forward{ cosf(yaw), sinf(yaw), 0.0f };
        FVector right{ -sinf(yaw), cosf(yaw), 0.0f };

        FVector input{};
        if (KeyDown('W')) input = Add(input, forward);
        if (KeyDown('S')) input = Add(input, Scale(forward, -1.0f));
        if (KeyDown('D')) input = Add(input, right);
        if (KeyDown('A')) input = Add(input, Scale(right, -1.0f));
        if (KeyDown(VK_SPACE)) input.Z += 1.0f;
        if (KeyDown(VK_SHIFT)) input.Z -= 1.0f;

        input = Normalize(input);
        if (Length(input) <= 0.001f)
            return false;

        float mult = st.speedMult > 0.1f ? st.speedMult : 1.0f;
        float flySpeed = 600.0f * mult;
        FVector next = Add(currentLoc, Scale(input, flySpeed * dt));
        if (SetActorLocation(pawn, next, false))
        {
            g_lastLoc = next;
            RefreshFlyStreaming(pawn, false);
            return true;
        }
        return false;
    }

    bool InvokeTakeWeapon(UObject* pawn, UObject* asset)
    {
        if (!pawn || !asset)
            return false;

        if (UFunction* instant = CachedFn(AH::Fn_InstantTakeWeapon))
        {
            P_WeaponDataAsset p{ asset };
            pawn->ProcessEvent(instant, &p);
            return true;
        }

        if (UFunction* take = CachedFn(AH::Fn_TakeWeapon))
        {
            P_TakeWeapon p{};
            p.WeaponItemDataAsset = asset;
            p.bInstant = true;
            pawn->ProcessEvent(take, &p);
            return true;
        }

        return false;
    }

    bool EquipWeapon(UObject* pawn, UObject* asset)
    {
        UFunction* equip = CachedFn(AH::Fn_EquipWeaponByDataAsset);
        if (!pawn || !asset || !equip)
            return false;

        P_WeaponDataAsset p{ asset };
        pawn->ProcessEvent(equip, &p);
        return true;
    }

    bool WriteInt32Field(void* base, int offset, int32_t value)
    {
        if (!base)
            return false;

        uint8_t* bytes = reinterpret_cast<uint8_t*>(base);
        if (!Mem::IsReadable(bytes + offset, sizeof(value)))
            return false;

        *reinterpret_cast<int32_t*>(bytes + offset) = value;
        return true;
    }

    bool ReadInt32Field(void* base, int offset, int32_t& value)
    {
        if (!base)
            return false;

        uint8_t* bytes = reinterpret_cast<uint8_t*>(base);
        if (!Mem::IsReadable(bytes + offset, sizeof(value)))
            return false;

        value = *reinterpret_cast<int32_t*>(bytes + offset);
        return true;
    }

    bool WriteFloatField(void* base, int offset, float value)
    {
        if (!base)
            return false;

        uint8_t* bytes = reinterpret_cast<uint8_t*>(base);
        if (!Mem::IsReadable(bytes + offset, sizeof(value)))
            return false;

        *reinterpret_cast<float*>(bytes + offset) = value;
        return true;
    }

    bool ReadFloatField(void* base, int offset, float& value)
    {
        if (!base)
            return false;

        uint8_t* bytes = reinterpret_cast<uint8_t*>(base);
        if (!Mem::IsReadable(bytes + offset, sizeof(value)))
            return false;

        value = *reinterpret_cast<float*>(bytes + offset);
        return true;
    }

    bool WriteBoolField(void* base, int offset, bool value)
    {
        if (!base)
            return false;

        uint8_t* bytes = reinterpret_cast<uint8_t*>(base);
        if (!Mem::IsReadable(bytes + offset, sizeof(value)))
            return false;

        *reinterpret_cast<bool*>(bytes + offset) = value;
        return true;
    }

    bool ReadBoolField(void* base, int offset, bool& value)
    {
        if (!base)
            return false;

        uint8_t* bytes = reinterpret_cast<uint8_t*>(base);
        if (!Mem::IsReadable(bytes + offset, sizeof(value)))
            return false;

        value = *reinterpret_cast<bool*>(bytes + offset);
        return true;
    }

    bool WriteUInt8Field(void* base, int offset, uint8_t value)
    {
        if (!base)
            return false;

        uint8_t* bytes = reinterpret_cast<uint8_t*>(base);
        if (!Mem::IsReadable(bytes + offset, sizeof(value)))
            return false;

        *reinterpret_cast<uint8_t*>(bytes + offset) = value;
        return true;
    }

    bool ReadUInt8Field(void* base, int offset, uint8_t& value)
    {
        if (!base)
            return false;

        uint8_t* bytes = reinterpret_cast<uint8_t*>(base);
        if (!Mem::IsReadable(bytes + offset, sizeof(value)))
            return false;

        value = *reinterpret_cast<uint8_t*>(bytes + offset);
        return true;
    }

    bool WritePtrField(void* base, int offset, void* value)
    {
        if (!base)
            return false;

        uint8_t* bytes = reinterpret_cast<uint8_t*>(base);
        if (!Mem::IsReadable(bytes + offset, sizeof(value)))
            return false;

        *reinterpret_cast<void**>(bytes + offset) = value;
        return true;
    }

    void* ReadPtrField(void* base, int offset)
    {
        if (!base)
            return nullptr;

        uint8_t* bytes = reinterpret_cast<uint8_t*>(base);
        if (!Mem::IsReadable(bytes + offset, sizeof(void*)))
            return nullptr;

        void* ptr = *reinterpret_cast<void**>(bytes + offset);
        return Mem::LooksLikePtr(ptr) ? ptr : nullptr;
    }

    void CaptureMovementBackup(uint8_t* mv, bool needWalk, bool needFly, bool needMode)
    {
        if (!mv)
            return;

        if (g_movementBackup.mv && g_movementBackup.mv != mv)
        {
            if (g_movementBackup.walkValid)
                WriteFloatField(g_movementBackup.mv, AH::Move_MaxWalkSpeed, g_movementBackup.walkSpeed);
            if (g_movementBackup.flyValid)
                WriteFloatField(g_movementBackup.mv, AH::Move_MaxFlySpeed, g_movementBackup.flySpeed);
            if (g_movementBackup.modeValid)
                WriteUInt8Field(g_movementBackup.mv, AH::Move_MovementMode, g_movementBackup.mode);
            g_movementBackup = {};
        }

        g_movementBackup.mv = mv;
        if (needWalk && !g_movementBackup.walkValid)
            g_movementBackup.walkValid = ReadFloatField(mv, AH::Move_MaxWalkSpeed, g_movementBackup.walkSpeed);
        if (needFly && !g_movementBackup.flyValid)
            g_movementBackup.flyValid = ReadFloatField(mv, AH::Move_MaxFlySpeed, g_movementBackup.flySpeed);
        if (needMode && !g_movementBackup.modeValid)
            g_movementBackup.modeValid = ReadUInt8Field(mv, AH::Move_MovementMode, g_movementBackup.mode);
    }

    void RestoreMovementWalk()
    {
        if (g_movementBackup.walkValid && WriteFloatField(g_movementBackup.mv, AH::Move_MaxWalkSpeed, g_movementBackup.walkSpeed))
            LOG("Speed restored: MaxWalkSpeed=%.1f", g_movementBackup.walkSpeed);
        g_movementBackup.walkValid = false;
    }

    void RestoreMovementFly()
    {
        if (g_movementBackup.flyValid && WriteFloatField(g_movementBackup.mv, AH::Move_MaxFlySpeed, g_movementBackup.flySpeed))
            LOG("Fly restored: MaxFlySpeed=%.1f", g_movementBackup.flySpeed);
        if (g_movementBackup.modeValid && WriteUInt8Field(g_movementBackup.mv, AH::Move_MovementMode, g_movementBackup.mode))
            LOG("Fly restored: MovementMode=%u", (unsigned)g_movementBackup.mode);
        g_movementBackup.flyValid = false;
        g_movementBackup.modeValid = false;
        if (!g_movementBackup.walkValid)
            g_movementBackup = {};
    }

    bool ApplyInventoryIgnoreOverWeight(UObject* inventory, bool ignore)
    {
        UFunction* fn = CachedFn(AH::Fn_AHInventory_SetIgnoreOverWeight);
        if (!inventory || !fn)
            return false;

        P_BoolParam p{ ignore };
        inventory->ProcessEvent(fn, &p);
        return true;
    }

    void CaptureInventoryBackup(UObject* inventory)
    {
        if (!inventory)
            return;

        if (g_inventoryBackup.inventory && g_inventoryBackup.inventory != inventory)
        {
            if (g_inventoryBackup.ammoCountValid)
                WriteInt32Field(g_inventoryBackup.inventory, AH::Inventory_AmmoCount, g_inventoryBackup.ammoCount);
            if (g_inventoryBackup.infiniteAmmoCountValid)
                WriteInt32Field(g_inventoryBackup.inventory, AH::Inventory_InfiniteAmmoCount, g_inventoryBackup.infiniteAmmoCount);
            g_inventoryBackup = {};
        }

        g_inventoryBackup.inventory = inventory;
        if (!g_inventoryBackup.ammoCountValid)
            g_inventoryBackup.ammoCountValid = ReadInt32Field(inventory, AH::Inventory_AmmoCount, g_inventoryBackup.ammoCount);
        if (!g_inventoryBackup.infiniteAmmoCountValid)
            g_inventoryBackup.infiniteAmmoCountValid = ReadInt32Field(inventory, AH::Inventory_InfiniteAmmoCount, g_inventoryBackup.infiniteAmmoCount);
    }

    void RestoreInventoryBackup()
    {
        if (!g_inventoryBackup.inventory)
            return;

        if (g_inventoryBackup.ammoCountValid)
            WriteInt32Field(g_inventoryBackup.inventory, AH::Inventory_AmmoCount, g_inventoryBackup.ammoCount);
        if (g_inventoryBackup.infiniteAmmoCountValid)
            WriteInt32Field(g_inventoryBackup.inventory, AH::Inventory_InfiniteAmmoCount, g_inventoryBackup.infiniteAmmoCount);
        if (ApplyInventoryIgnoreOverWeight(g_inventoryBackup.inventory, false))
            LOG("InfiniteAmmo inventory overweight bypass disabled");
        LOG("InfiniteAmmo inventory state restored");
        g_inventoryBackup = {};
    }

    void RestoreWeaponAmmoBackup()
    {
        if (!g_weaponAmmoBackup.weapon)
            return;

        if (g_weaponAmmoBackup.ammoSizeValid)
            WriteInt32Field(g_weaponAmmoBackup.weapon, AH::Weapon_AmmoSize, g_weaponAmmoBackup.ammoSize);
        if (g_weaponAmmoBackup.startAmmoValid)
            WriteInt32Field(g_weaponAmmoBackup.weapon, AH::Weapon_StartAmmoCount, g_weaponAmmoBackup.startAmmo);

        if (g_weaponAmmoBackup.barrel)
        {
            if (g_weaponAmmoBackup.shootingBlockedValid)
                WriteBoolField(g_weaponAmmoBackup.barrel, AH::EBBarrel_ShootingBlocked, g_weaponAmmoBackup.shootingBlocked);
            if (g_weaponAmmoBackup.cycleUnlimitedValid)
                WriteBoolField(g_weaponAmmoBackup.barrel, AH::EBBarrel_CycleAmmoUnlimited, g_weaponAmmoBackup.cycleUnlimited);
            if (g_weaponAmmoBackup.cycleCountValid)
                WriteInt32Field(g_weaponAmmoBackup.barrel, AH::EBBarrel_CycleAmmoCount, g_weaponAmmoBackup.cycleCount);
            if (g_weaponAmmoBackup.cyclePosValid)
                WriteInt32Field(g_weaponAmmoBackup.barrel, AH::EBBarrel_CycleAmmoPos, g_weaponAmmoBackup.cyclePos);
            if (g_weaponAmmoBackup.loadNextValid)
                WriteBoolField(g_weaponAmmoBackup.barrel, AH::EBBarrel_LoadNext, g_weaponAmmoBackup.loadNext);
            if (g_weaponAmmoBackup.chamberedValid)
                WritePtrField(g_weaponAmmoBackup.barrel, AH::EBBarrel_ChamberedBullet, g_weaponAmmoBackup.chambered);
        }

        LOG("InfiniteAmmo weapon state restored weapon=%p barrel=%p", (void*)g_weaponAmmoBackup.weapon, g_weaponAmmoBackup.barrel);
        g_weaponAmmoBackup = {};
    }

    void CaptureWeaponAmmoBackup(UObject* weapon, void* barrel)
    {
        if (!weapon)
            return;

        if (g_weaponAmmoBackup.weapon && g_weaponAmmoBackup.weapon != weapon)
            RestoreWeaponAmmoBackup();

        g_weaponAmmoBackup.weapon = weapon;
        g_weaponAmmoBackup.barrel = barrel;

        if (!g_weaponAmmoBackup.ammoSizeValid)
            g_weaponAmmoBackup.ammoSizeValid = ReadInt32Field(weapon, AH::Weapon_AmmoSize, g_weaponAmmoBackup.ammoSize);
        if (!g_weaponAmmoBackup.startAmmoValid)
            g_weaponAmmoBackup.startAmmoValid = ReadInt32Field(weapon, AH::Weapon_StartAmmoCount, g_weaponAmmoBackup.startAmmo);

        if (barrel)
        {
            if (!g_weaponAmmoBackup.shootingBlockedValid)
                g_weaponAmmoBackup.shootingBlockedValid = ReadBoolField(barrel, AH::EBBarrel_ShootingBlocked, g_weaponAmmoBackup.shootingBlocked);
            if (!g_weaponAmmoBackup.cycleUnlimitedValid)
                g_weaponAmmoBackup.cycleUnlimitedValid = ReadBoolField(barrel, AH::EBBarrel_CycleAmmoUnlimited, g_weaponAmmoBackup.cycleUnlimited);
            if (!g_weaponAmmoBackup.cycleCountValid)
                g_weaponAmmoBackup.cycleCountValid = ReadInt32Field(barrel, AH::EBBarrel_CycleAmmoCount, g_weaponAmmoBackup.cycleCount);
            if (!g_weaponAmmoBackup.cyclePosValid)
                g_weaponAmmoBackup.cyclePosValid = ReadInt32Field(barrel, AH::EBBarrel_CycleAmmoPos, g_weaponAmmoBackup.cyclePos);
            if (!g_weaponAmmoBackup.loadNextValid)
                g_weaponAmmoBackup.loadNextValid = ReadBoolField(barrel, AH::EBBarrel_LoadNext, g_weaponAmmoBackup.loadNext);
            if (!g_weaponAmmoBackup.chamberedValid)
            {
                uint8_t* bytes = reinterpret_cast<uint8_t*>(barrel);
                if (Mem::IsReadable(bytes + AH::EBBarrel_ChamberedBullet, sizeof(void*)))
                {
                    g_weaponAmmoBackup.chambered = *reinterpret_cast<void**>(bytes + AH::EBBarrel_ChamberedBullet);
                    g_weaponAmmoBackup.chamberedValid = true;
                }
            }
        }
    }

    UObject* GetCurrentWeaponObject(UObject* pawn)
    {
        if (!pawn)
            return nullptr;

        if (UFunction* fn = CachedFn(AH::Fn_GetCurrentWeapon))
        {
            P_GetCurrentWeapon p{};
            pawn->ProcessEvent(fn, &p);
            if (Mem::LooksLikePtr(p.ReturnValue))
                return static_cast<UObject*>(p.ReturnValue);
        }

        // Fallback: the getter returns null while holstered / mid-swap / in scripted
        // states, which silently broke weapon RGB + the weapon diagnostic capture.
        // Resolve the equipped weapon straight from the pawn's reflected field by
        // NAME (offset-independent, no guessed offset). Try the known field names.
        static const char* kWeaponFields[] = { "CurrentWeapon", "WeaponInHands", "EquippedWeapon", "CurrentSlotWeapon" };
        for (const char* name : kWeaponFields)
            if (UObject* w = Reflect::ReadNamedObjectProperty(pawn, name))
                return w;
        return nullptr;
    }

    UObject* GetInventoryPlayer(UObject* pawn)
    {
        if (!pawn)
            return nullptr;

        void* inv = ReadPtrField(pawn, AH::Char_InventoryPlayer);
        if (inv)
            return static_cast<UObject*>(inv);

        if (UFunction* fn = CachedFn(AH::Fn_GetInventoryPlayer))
        {
            P_ObjectReturn p{};
            pawn->ProcessEvent(fn, &p);
            if (Mem::LooksLikePtr(p.ReturnValue))
                return static_cast<UObject*>(p.ReturnValue);
        }

        return nullptr;
    }

    int QueryInventoryItemCount(UObject* inventory, UObject* asset)
    {
        UFunction* fn = CachedFn(AH::Fn_AHInventory_GetItemsCount);
        if (!inventory || !asset || !fn)
            return -1;

        P_InventoryGetItemsCount p{};
        p.InItemDataAsset = asset;
        inventory->ProcessEvent(fn, &p);
        return p.ReturnValue;
    }

    int QueryWeaponLoadedAmmo(UObject* weapon)
    {
        if (!weapon)
            return -1;

        if (UFunction* fn = CachedFn(AH::Fn_BaseWeapon_GetAmmoCount))
        {
            P_AmmoCount p{};
            p.bCountChambered = true;
            weapon->ProcessEvent(fn, &p);
            return p.ReturnValue;
        }

        if (void* barrel = ReadPtrField(weapon, AH::RangeWeapon_Barrel))
        {
            if (UFunction* fn = CachedFn(AH::Fn_EBBarrel_GetAmmoCount))
            {
                P_AmmoCount p{};
                p.bCountChambered = true;
                static_cast<UObject*>(barrel)->ProcessEvent(fn, &p);
                return p.ReturnValue;
            }
        }

        return -1;
    }

    int QueryBarrelLoadedAmmo(UObject* barrel)
    {
        UFunction* fn = CachedFn(AH::Fn_EBBarrel_GetAmmoCount);
        if (!barrel || !fn)
            return -1;

        P_AmmoCount p{};
        p.bCountChambered = true;
        barrel->ProcessEvent(fn, &p);
        return p.ReturnValue;
    }

    int QueryWeaponReserveAmmo(UObject* pawn, UObject* weapon)
    {
        if (pawn)
        {
            if (UFunction* fn = CachedFn(AH::Fn_GetCurrentWeaponInventoryAmmoCount))
            {
                P_IntReturn p{};
                pawn->ProcessEvent(fn, &p);
                return p.ReturnValue;
            }

            if (weapon)
            {
                if (UFunction* fn = CachedFn(AH::Fn_GetWeaponInventoryAmmoCount))
                {
                    P_WeaponInventoryAmmoCount p{};
                    p.Weapon = weapon;
                    pawn->ProcessEvent(fn, &p);
                    return p.ReturnValue;
                }
            }
        }

        if (weapon)
        {
            if (UFunction* fn = CachedFn(AH::Fn_BaseWeapon_GetAmmoInPossession))
            {
                P_IntReturn p{};
                weapon->ProcessEvent(fn, &p);
                return p.ReturnValue;
            }
        }

        return -1;
    }

    void* ResolveBulletClass(UObject* weapon, void* barrel)
    {
        if (void* bullet = ReadPtrField(weapon, AH::RangeWeapon_Bullet))
            return bullet;

        if (!barrel)
            return nullptr;

        uint8_t* bytes = reinterpret_cast<uint8_t*>(barrel);
        if (!Mem::IsReadable(bytes + AH::EBBarrel_Ammo, sizeof(TArray<void*>)))
            return nullptr;

        auto* ammo = reinterpret_cast<TArray<void*>*>(bytes + AH::EBBarrel_Ammo);
        if (ammo->Count <= 0 || ammo->Count > 64 || !Mem::IsReadable(ammo->Data, sizeof(void*)))
            return nullptr;

        void* bullet = ammo->Data[0];
        return Mem::LooksLikePtr(bullet) ? bullet : nullptr;
    }

    // =======================================================================
    //  DEBUG DIAGNOSTICS  --  universal read-only game-state capture
    // -----------------------------------------------------------------------
    //  Button mode writes a full timestamped bundle. Toggle mode creates
    //  togglelogsN, writes an initial full snapshot, then streams lightweight
    //  snapshots plus ProcessEvent traces for AI follow / move / material calls.
    // =======================================================================
    enum class DiagTraceKind
    {
        None,
        MoveToActor,
        MoveToLocation,
        SimpleMoveToActor,
        CreateMoveToProxy,
        AITaskMoveTo,
        SetBBFollowLocation,
        SetBBTargetPtr,
        SetBBTargetEnemy,
        SetBBBool,
        SetBBFloat,
        BBSetObject,
        BBSetBool,
        BBSetFloat,
        BBSetVector,
        MeshVector,
        MeshScalar
    };

    struct DiagTraceFn
    {
        UFunction* fn = nullptr;
        const char* label = "";
        DiagTraceKind kind = DiagTraceKind::None;
    };

    struct P_MoveToLocationDiag
    {
        FVector Dest;
        float AcceptanceRadius;
        bool bStopOnOverlap;
        bool bUsePathfinding;
        bool bProjectDestinationToNavigation;
        bool bCanStrafe;
        uint8_t _pad0[4];
        void* FilterClass;
        bool bAllowPartialPath;
        uint8_t ReturnValue;
        uint8_t _pad1[6];
    };
    struct P_CreateMoveToProxyDiag
    {
        void* WorldContextObject;
        void* Pawn;
        FVector Destination;
        uint8_t _pad0[4];
        void* TargetActor;
        float AcceptanceRadius;
        bool bStopOnOverlap;
        uint8_t _pad1[3];
        void* ReturnValue;
    };
    struct P_AITaskMoveToDiag
    {
        void* Controller;
        FVector GoalLocation;
        uint8_t _pad0[4];
        void* GoalActor;
        float AcceptanceRadius;
        uint8_t StopOnOverlap;
        uint8_t AcceptPartialPath;
        bool bUsePathfinding;
        bool bLockAILogic;
        bool bUseContinuousGoalTracking;
        uint8_t ProjectGoalOnNavigation;
        uint8_t _pad1[6];
        void* ReturnValue;
    };
    struct P_SimpleMoveToActorDiag { void* Controller; void* Goal; };
    struct P_SetBBBoolDiag { bool bValue; };
    struct P_SetBBFloatDiag { FName KeyName; float Value; };
    struct P_SetBBObjectDiag { FName KeyName; void* ObjectValue; };
    struct P_SetBBVectorDiag { FName KeyName; FVector Value; };
    struct P_BBKeyBoolDiag { FName KeyName; bool Value; uint8_t _pad0[3]; };
    struct P_BBKeyFloatDiag { FName KeyName; float Value; };
    struct P_BBKeyObjectDiag { FName KeyName; void* Value; };
    struct P_BBKeyVectorDiag { FName KeyName; FVector Value; };
    struct BBEntryRaw { FName EntryName; UObject* KeyType; uint8_t bInstanceSynced; uint8_t _pad0[7]; };
    static_assert(sizeof(P_MoveToLocationDiag) == 0x28, "MoveToLocation params must match SDK");
    static_assert(sizeof(P_CreateMoveToProxyDiag) == 0x38, "CreateMoveToProxyObject params must match SDK");
    static_assert(sizeof(P_AITaskMoveToDiag) == 0x38, "AITask_MoveTo.AIMoveTo params must match SDK");
    static_assert(sizeof(BBEntryRaw) == 0x18, "FBlackboardEntry must match SDK");

    std::mutex          g_diagMutex;
    std::ofstream       g_diagTraceFile;
    std::string         g_diagToggleDir;
    std::string         g_diagLastDir;
    std::vector<DiagTraceFn> g_diagTraceFns;
    std::atomic<bool>   g_diagTraceActive{ false };
    std::atomic<bool>   g_diagSnapshotInFlight{ false };
    bool                g_diagToggleWasOn = false;
    int                 g_diagSnapshotSeq = 0;
    ULONGLONG           g_diagLastSnapshotMs = 0;

    // Universal ProcessEvent trace target: when set, EVERY UFunction dispatched on
    // this actor (or its controller) is logged to the trace file with its full name
    // + raw param bytes -- the tool for discovering an NPC's native movement driver
    // (e.g. Larisa) to detour. Resolved to a live pointer from the Debug tab.
    std::atomic<UObject*> g_traceTarget{ nullptr };
    std::atomic<UObject*> g_traceTargetCtrl{ nullptr };
    std::atomic<bool>     g_targetedDumpInFlight{ false };
    std::atomic<bool>     g_liveTargetInFlight{ false };
    std::string           g_traceTargetName;            // guarded by g_diagMutex (display only)

    std::string PtrHex(const void* p)
    {
        std::ostringstream os;
        os << "0x" << std::uppercase << std::hex << (uintptr_t)p;
        return os.str();
    }

    // ---- detour-hook enablement: code address -> {ptr, module, base, rva} -----
    // Emits an address with the owning loaded module + its module-relative RVA, so
    // any logged function/vtable address is stable across runs and directly usable
    // to build a detour (MinHook on module_base+rva) or an AOB signature. "module":
    // null means the address is not inside a loaded image (heap / JIT / dynamic stub).
    void WriteCodeAddrJson(std::ostream& os, const void* addr)
    {
        if (!addr) { os << "null"; return; }
        os << "{\"ptr\":\"" << PtrHex(addr) << "\"";
        HMODULE mod = nullptr;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCSTR>(addr), &mod) && mod)
        {
            char path[MAX_PATH]{};
            const char* base = "";
            if (GetModuleFileNameA(mod, path, MAX_PATH) > 0)
            {
                const char* slash = strrchr(path, '\\');
                base = slash ? slash + 1 : path;
            }
            uintptr_t rva = reinterpret_cast<uintptr_t>(addr) - reinterpret_cast<uintptr_t>(mod);
            os << ",\"module\":\"" << JsonEscape(base) << "\",\"module_base\":\"" << PtrHex(mod)
               << "\",\"rva\":\"0x" << std::hex << rva << std::dec << "\"";
        }
        else
        {
            os << ",\"module\":null";
        }
        os << "}";
    }

    // Dump the first `count` vtable slots of an object as code addresses. The vtable
    // is *(void***)obj. These are the NATIVE virtual functions (AActor::Tick, the
    // movement/scene component virtuals, the AI controller, Mercuna component, etc.)
    // -- i.e. the detour targets for low-level, non-UFunction native code. Each entry
    // is a {ptr, module, rva} so it can be hooked or signature-matched directly.
    void WriteVTableJson(std::ostream& os, UObject* obj, int count)
    {
        os << "[";
        if (!Mem::IsReadable(obj, sizeof(void*))) { os << "]"; return; }
        void** vt = *reinterpret_cast<void***>(obj);
        if (!Mem::IsReadable(vt, sizeof(void*))) { os << "]"; return; }
        bool first = true;
        for (int i = 0; i < count; ++i)
        {
            if (!Mem::IsReadable(vt + i, sizeof(void*))) break;
            void* fn = vt[i];
            if (!Mem::IsReadable(fn, 1)) break; // stop at the first non-code slot
            if (!first) os << ",";
            first = false;
            os << "{\"index\":" << i << ",\"fn\":"; WriteCodeAddrJson(os, fn); os << "}";
        }
        os << "]";
    }

    // Defined later (needs SafeClassName/WriteObjectJson); forward-declared so the
    // main snapshot can dump component vtables too.
    void WriteActorComponentsForDetour(std::ostream& os, UE::UObject* actor);

    std::string LocalTimestampForName()
    {
        SYSTEMTIME st{};
        GetLocalTime(&st);
        char buf[64]{};
        sprintf_s(buf, "%04u%02u%02u_%02u%02u%02u_%03u",
            st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
        return buf;
    }

    std::string LocalTimestampIso()
    {
        SYSTEMTIME st{};
        GetLocalTime(&st);
        char buf[64]{};
        sprintf_s(buf, "%04u-%02u-%02uT%02u:%02u:%02u.%03u",
            st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
        return buf;
    }

    std::string PathJoin(const std::string& a, const std::string& b)
    {
        if (a.empty()) return b;
        char last = a[a.size() - 1];
        if (last == '\\' || last == '/') return a + b;
        return a + "\\" + b;
    }

    bool EnsureDir(const std::string& path)
    {
        if (path.empty()) return false;
        if (CreateDirectoryA(path.c_str(), nullptr))
            return true;
        return GetLastError() == ERROR_ALREADY_EXISTS;
    }

    std::string DiagnosticsRootDir()
    {
        char exePath[MAX_PATH]{};
        DWORD len = GetModuleFileNameA(nullptr, exePath, MAX_PATH);
        std::string base;
        if (len > 0 && len < MAX_PATH)
        {
            char* slash = strrchr(exePath, '\\');
            char* slash2 = strrchr(exePath, '/');
            if (slash2 && (!slash || slash2 > slash)) slash = slash2;
            if (slash)
            {
                slash[1] = '\0';
                base = exePath;
            }
        }
        if (base.empty())
        {
            char tmp[MAX_PATH]{};
            if (GetTempPathA(MAX_PATH, tmp) > 0) base = tmp;
        }
        std::string root = PathJoin(base.empty() ? "." : base, "AtomicHeartMenu_diagnostics");
        EnsureDir(root);
        return root;
    }

    std::string CreateManualDiagnosticsDir()
    {
        std::string root = DiagnosticsRootDir();
        for (int i = 1; i < 1000; ++i)
        {
            std::ostringstream name;
            name << "snapshot_" << LocalTimestampForName() << "_" << i;
            std::string dir = PathJoin(root, name.str());
            if (CreateDirectoryA(dir.c_str(), nullptr))
                return dir;
        }
        return {};
    }

    std::string CreateToggleDiagnosticsDir()
    {
        std::string root = DiagnosticsRootDir();
        for (int i = 1; i < 10000; ++i)
        {
            std::ostringstream name;
            name << "togglelogs" << i;
            std::string dir = PathJoin(root, name.str());
            if (CreateDirectoryA(dir.c_str(), nullptr))
                return dir;
        }
        return {};
    }

    bool WriteTextFile(const std::string& path, const std::string& text)
    {
        std::ofstream f(path, std::ios::out | std::ios::trunc | std::ios::binary);
        if (!f) return false;
        f << text;
        return true;
    }

    std::string LowerCopy(std::string s)
    {
        for (char& c : s)
            c = (char)std::tolower((unsigned char)c);
        return s;
    }

    bool ContainsAnyTerm(const std::string& text)
    {
        std::string s = LowerCopy(text);
        static const char* terms[] = {
            "follow", "escort", "bodyguard", "companion", "larisa", "larissa",
            "quest", "boss", "robot", "move", "blackboard", "behavior", "behaviour",
            "weapon", "material", "mesh", "rgb", "ai"
        };
        for (const char* t : terms)
            if (s.find(t) != std::string::npos)
                return true;
        return false;
    }

    std::string SafeFNameToString(const FName& name)
    {
        try { return name.ToString(); } catch (...) { return {}; }
    }

    bool ReadFNameAt(void* base, int offset, FName& out)
    {
        if (!base) return false;
        uint8_t* b = reinterpret_cast<uint8_t*>(base);
        if (!Mem::IsReadable(b + offset, sizeof(FName)))
            return false;
        out = *reinterpret_cast<FName*>(b + offset);
        return true;
    }

    UObject* ReadUObjectAt(void* base, int offset)
    {
        void* p = ReadPtrField(base, offset);
        return Mem::IsReadable(p, 0x30) ? static_cast<UObject*>(p) : nullptr;
    }

    std::string SafeObjectName(UObject* o)
    {
        if (!Mem::IsReadable(o, 0x30)) return {};
        try { return o->GetName(); } catch (...) { return {}; }
    }

    std::string SafeObjectFullName(UObject* o)
    {
        if (!Mem::IsReadable(o, 0x30)) return {};
        try { return o->GetFullName(); } catch (...) { return {}; }
    }

    std::string SafeClassName(UObject* o)
    {
        if (!Mem::IsReadable(o, 0x30)) return {};
        try
        {
            UObject* cls = o->Class();
            return Mem::IsReadable(cls, 0x30) ? cls->GetName() : std::string{};
        }
        catch (...) { return {}; }
    }

    void TrackAiDeathEventFromProcessEvent(void* obj, void* fn, void* params)
    {
        if (!fn)
            return;
        // Hot ProcessEvent path: use cached death UFunction pointers. Refresh at most
        // once a second so subsystem death events with params are caught without doing
        // global object lookups on every dispatch.
        static ULONGLONG lastResolveMs = 0;
        ULONGLONG nowResolve = GetTickCount64();
        if (nowResolve - lastResolveMs > 1000)
        {
            lastResolveMs = nowResolve;
            ResolveHookTwinDeathPipelineFns();
        }
        void* fK2Death = g_fnHookK2OnDeath.load(std::memory_order_relaxed);
        void* fK2Load = g_fnHookK2OnLoadDeathState.load(std::memory_order_relaxed);
        void* fLoadDead = g_fnHookLoadDeadState.load(std::memory_order_relaxed);
        void* fSendDeath = g_fnHookSendDeathEvent.load(std::memory_order_relaxed);
        void* fSendLoad = g_fnHookSendLoadDeathStateEvent.load(std::memory_order_relaxed);
        void* fDied = g_fnHookSendCharacterDiedEvent.load(std::memory_order_relaxed);
        void* fDestroyOwner = g_fnHookDestroyOwnerCharacter.load(std::memory_order_relaxed);

        UObject* actor = nullptr;
        const char* reason = nullptr;
        if (fn == fK2Death || fn == fK2Load || fn == fLoadDead)
        {
            actor = Mem::IsReadable(obj, 0x30) ? static_cast<UObject*>(obj) : nullptr;
            reason = (fn == fK2Death) ? "K2_OnDeath" : (fn == fK2Load ? "K2_OnLoadDeathState" : "LoadDeadState");
        }
        else if ((fn == fSendDeath || fn == fSendLoad) && Mem::IsReadable(params, sizeof(P_HookDeathEventOwner)))
        {
            actor = reinterpret_cast<UObject*>(reinterpret_cast<P_HookDeathEventOwner*>(params)->EventOwnerCharacter);
            reason = (fn == fSendDeath) ? "SendDeathEvent" : "SendLoadDeathStateEvent";
        }
        else if (fn == fDied && Mem::IsReadable(params, sizeof(P_HookGameplayCharacterDied)))
        {
            actor = reinterpret_cast<UObject*>(reinterpret_cast<P_HookGameplayCharacterDied*>(params)->DeadCharacter);
            reason = "SendCharacterDiedEvent";
        }
        else if (fn == fDestroyOwner)
        {
            actor = HookTwinOwnerFromDeathAbility(obj);
            reason = "DestroyOwnerCharacter";
        }
        else
        {
            std::string name = SafeObjectFullName(static_cast<UObject*>(fn));
            std::string lower = LowerCopy(name);
            if (lower.find("k2_ondeath") != std::string::npos || lower.find(".ondeath") != std::string::npos)
            {
                actor = Mem::IsReadable(obj, 0x30) ? static_cast<UObject*>(obj) : nullptr;
                reason = "death-name-fallback";
            }
        }

        if (!actor || !AiUsable(actor) || IsHookBodyguard(actor))
            return;
        MarkAiDeathTombstone(actor, reason);
    }

    void WriteVectorJson(std::ostream& os, const FVector& v)
    {
        os << "{\"x\":" << v.X << ",\"y\":" << v.Y << ",\"z\":" << v.Z << "}";
    }

    void WriteRotatorJson(std::ostream& os, const FRotator& r)
    {
        os << "{\"pitch\":" << r.Pitch << ",\"yaw\":" << r.Yaw << ",\"roll\":" << r.Roll << "}";
    }

    void WriteFNameJson(std::ostream& os, const FName& n)
    {
        os << "{\"index\":" << n.ComparisonIndex << ",\"number\":" << n.Number
           << ",\"name\":\"" << JsonEscape(SafeFNameToString(n)) << "\"}";
    }

    void WriteObjectJson(std::ostream& os, UObject* o)
    {
        if (!Mem::IsReadable(o, 0x30))
        {
            os << "null";
            return;
        }
        int idx = -1;
        try { idx = o->Index(); } catch (...) {}
        os << "{\"ptr\":\"" << PtrHex(o) << "\",\"index\":" << idx
           << ",\"name\":\"" << JsonEscape(SafeObjectName(o)) << "\""
           << ",\"class\":\"" << JsonEscape(SafeClassName(o)) << "\""
           << ",\"full\":\"" << JsonEscape(SafeObjectFullName(o)) << "\"}";
    }

    void WritePtrFieldJson(std::ostream& os, const char* name, void* base, int offset, bool& first)
    {
        if (!first) os << ",";
        first = false;
        os << "\"" << name << "\":";
        WriteObjectJson(os, ReadUObjectAt(base, offset));
    }

    void DumpHexWindow(std::ostream& hex, const char* label, const void* ptr, size_t requested)
    {
        if (!hex) return;
        hex << "\n[" << label << "] " << PtrHex(ptr) << " requested=0x"
            << std::hex << requested << std::dec << "\n";
        if (!ptr)
        {
            hex << "  null\n";
            return;
        }
        size_t n = requested;
        while (n > 0 && !Mem::IsReadable(ptr, n))
            n /= 2;
        if (n == 0)
        {
            hex << "  unreadable\n";
            return;
        }
        const uint8_t* b = reinterpret_cast<const uint8_t*>(ptr);
        for (size_t i = 0; i < n; i += 16)
        {
            hex << "  +" << std::setw(4) << std::setfill('0') << std::hex << i << ": ";
            for (size_t j = 0; j < 16 && i + j < n; ++j)
                hex << std::setw(2) << (int)b[i + j] << " ";
            hex << std::dec << std::setfill(' ') << "\n";
        }
    }

    void WritePointerScanJson(std::ostream& os, void* object, int maxBytes, int maxHits)
    {
        os << "[";
        if (!Mem::IsReadable(object, 0x30))
        {
            os << "]";
            return;
        }
        bool first = true;
        int hits = 0;
        uint8_t* b = reinterpret_cast<uint8_t*>(object);
        for (int off = 0; off + 8 <= maxBytes && hits < maxHits; off += 8)
        {
            if (!Mem::IsReadable(b + off, sizeof(void*)))
                continue;
            void* raw = *reinterpret_cast<void**>(b + off);
            UObject* u = Mem::IsReadable(raw, 0x30) ? static_cast<UObject*>(raw) : nullptr;
            if (!u)
                continue;
            std::string cls = SafeClassName(u);
            std::string nm = SafeObjectName(u);
            if (cls.empty() && nm.empty())
                continue;
            if (!first) os << ",";
            first = false;
            ++hits;
            os << "{\"offset\":\"0x" << std::hex << off << std::dec << "\",\"object\":";
            WriteObjectJson(os, u);
            os << "}";
        }
        os << "]";
    }

    void AddTraceFn(std::vector<DiagTraceFn>& out, const char* fullName, const char* label, DiagTraceKind kind)
    {
        UFunction* fn = CachedFn(fullName);
        if (Mem::IsReadable(fn, 0x30))
            out.push_back({ fn, label, kind });
    }

    void ResolveDiagnosticTraceFns()
    {
        std::vector<DiagTraceFn> fns;
        AddTraceFn(fns, "Function /Script/AIModule.AIController.MoveToActor", "AIModule.AIController.MoveToActor", DiagTraceKind::MoveToActor);
        AddTraceFn(fns, "Function /Script/AIModule.AIController.MoveToLocation", "AIModule.AIController.MoveToLocation", DiagTraceKind::MoveToLocation);
        AddTraceFn(fns, "Function /Script/AIModule.AIBlueprintHelperLibrary.SimpleMoveToActor", "AIBlueprintHelper.SimpleMoveToActor", DiagTraceKind::SimpleMoveToActor);
        AddTraceFn(fns, "Function /Script/AIModule.AIBlueprintHelperLibrary.CreateMoveToProxyObject", "AIBlueprintHelper.CreateMoveToProxyObject", DiagTraceKind::CreateMoveToProxy);
        AddTraceFn(fns, "Function /Script/AIModule.AITask_MoveTo.AIMoveTo", "AITask_MoveTo.AIMoveTo", DiagTraceKind::AITaskMoveTo);
        AddTraceFn(fns, "Function /Script/AtomicHeart.AHAIController.SetBlackboardFollowLocation", "AHAIController.SetBlackboardFollowLocation", DiagTraceKind::SetBBFollowLocation);
        AddTraceFn(fns, "Function /Script/AtomicHeart.AHAIController.SetBlackboardTargetAlly", "AHAIController.SetBlackboardTargetAlly", DiagTraceKind::SetBBTargetPtr);
        AddTraceFn(fns, "Function /Script/AtomicHeart.AHAIController.SetBlackboardTargetObject", "AHAIController.SetBlackboardTargetObject", DiagTraceKind::SetBBTargetPtr);
        AddTraceFn(fns, "Function /Script/AtomicHeart.AHAIController.SetBlackboardTargetEnemy", "AHAIController.SetBlackboardTargetEnemy", DiagTraceKind::SetBBTargetEnemy);
        AddTraceFn(fns, "Function /Script/AtomicHeart.AHAIController.SetBlackboardIsAggressive", "AHAIController.SetBlackboardIsAggressive", DiagTraceKind::SetBBBool);
        AddTraceFn(fns, "Function /Script/AtomicHeart.AHAIController.SetBlackboardValueAsFloat", "AHAIController.SetBlackboardValueAsFloat", DiagTraceKind::SetBBFloat);
        AddTraceFn(fns, "Function /Script/AtomicHeart.AHAIController.SetBlackboardValueAsObject", "AHAIController.SetBlackboardValueAsObject", DiagTraceKind::BBSetObject);
        AddTraceFn(fns, "Function /Script/AtomicHeart.AHAIController.SetFollowLocationSpeed", "AHAIController.SetFollowLocationSpeed", DiagTraceKind::None);
        AddTraceFn(fns, "Function /Script/AIModule.BlackboardComponent.SetValueAsObject", "Blackboard.SetValueAsObject", DiagTraceKind::BBSetObject);
        AddTraceFn(fns, "Function /Script/AIModule.BlackboardComponent.SetValueAsBool", "Blackboard.SetValueAsBool", DiagTraceKind::BBSetBool);
        AddTraceFn(fns, "Function /Script/AIModule.BlackboardComponent.SetValueAsFloat", "Blackboard.SetValueAsFloat", DiagTraceKind::BBSetFloat);
        AddTraceFn(fns, "Function /Script/AIModule.BlackboardComponent.SetValueAsVector", "Blackboard.SetValueAsVector", DiagTraceKind::BBSetVector);
        AddTraceFn(fns, "Function /Script/Engine.MeshComponent.SetVectorParameterValueOnMaterials", "Mesh.SetVectorParameterValueOnMaterials", DiagTraceKind::MeshVector);
        AddTraceFn(fns, "Function /Script/Engine.MeshComponent.SetScalarParameterValueOnMaterials", "Mesh.SetScalarParameterValueOnMaterials", DiagTraceKind::MeshScalar);

        std::lock_guard<std::mutex> lk(g_diagMutex);
        g_diagTraceFns.swap(fns);
    }

    void WriteTraceParams(std::ostream& os, DiagTraceKind kind, void* params)
    {
        os << "{";
        if (!params)
        {
            os << "\"params_ptr\":null}";
            return;
        }
        os << "\"params_ptr\":\"" << PtrHex(params) << "\"";
        switch (kind)
        {
        case DiagTraceKind::MoveToActor:
            if (Mem::IsReadable(params, sizeof(P_MoveToActor)))
            {
                auto* p = reinterpret_cast<P_MoveToActor*>(params);
                os << ",\"goal\":"; WriteObjectJson(os, static_cast<UObject*>(p->Goal));
                os << ",\"acceptance_radius\":" << p->AcceptanceRadius
                   << ",\"stop_on_overlap\":" << (p->bStopOnOverlap ? "true" : "false")
                   << ",\"use_pathfinding\":" << (p->bUsePathfinding ? "true" : "false")
                   << ",\"can_strafe\":" << (p->bCanStrafe ? "true" : "false")
                   << ",\"allow_partial\":" << (p->bAllowPartialPath ? "true" : "false")
                   << ",\"return_value\":" << (int)p->ReturnValue;
            }
            break;
        case DiagTraceKind::MoveToLocation:
            if (Mem::IsReadable(params, sizeof(P_MoveToLocationDiag)))
            {
                auto* p = reinterpret_cast<P_MoveToLocationDiag*>(params);
                os << ",\"dest\":"; WriteVectorJson(os, p->Dest);
                os << ",\"acceptance_radius\":" << p->AcceptanceRadius
                   << ",\"stop_on_overlap\":" << (p->bStopOnOverlap ? "true" : "false")
                   << ",\"use_pathfinding\":" << (p->bUsePathfinding ? "true" : "false")
                   << ",\"project_to_nav\":" << (p->bProjectDestinationToNavigation ? "true" : "false")
                   << ",\"can_strafe\":" << (p->bCanStrafe ? "true" : "false")
                   << ",\"allow_partial\":" << (p->bAllowPartialPath ? "true" : "false")
                   << ",\"return_value\":" << (int)p->ReturnValue;
            }
            break;
        case DiagTraceKind::SimpleMoveToActor:
            if (Mem::IsReadable(params, sizeof(P_SimpleMoveToActorDiag)))
            {
                auto* p = reinterpret_cast<P_SimpleMoveToActorDiag*>(params);
                os << ",\"controller\":"; WriteObjectJson(os, static_cast<UObject*>(p->Controller));
                os << ",\"goal\":"; WriteObjectJson(os, static_cast<UObject*>(p->Goal));
            }
            break;
        case DiagTraceKind::CreateMoveToProxy:
            if (Mem::IsReadable(params, sizeof(P_CreateMoveToProxyDiag)))
            {
                auto* p = reinterpret_cast<P_CreateMoveToProxyDiag*>(params);
                os << ",\"pawn\":"; WriteObjectJson(os, static_cast<UObject*>(p->Pawn));
                os << ",\"destination\":"; WriteVectorJson(os, p->Destination);
                os << ",\"target_actor\":"; WriteObjectJson(os, static_cast<UObject*>(p->TargetActor));
                os << ",\"acceptance_radius\":" << p->AcceptanceRadius
                   << ",\"stop_on_overlap\":" << (p->bStopOnOverlap ? "true" : "false");
            }
            break;
        case DiagTraceKind::AITaskMoveTo:
            if (Mem::IsReadable(params, sizeof(P_AITaskMoveToDiag)))
            {
                auto* p = reinterpret_cast<P_AITaskMoveToDiag*>(params);
                os << ",\"controller\":"; WriteObjectJson(os, static_cast<UObject*>(p->Controller));
                os << ",\"goal_location\":"; WriteVectorJson(os, p->GoalLocation);
                os << ",\"goal_actor\":"; WriteObjectJson(os, static_cast<UObject*>(p->GoalActor));
                os << ",\"acceptance_radius\":" << p->AcceptanceRadius
                   << ",\"use_pathfinding\":" << (p->bUsePathfinding ? "true" : "false")
                   << ",\"lock_ai_logic\":" << (p->bLockAILogic ? "true" : "false")
                   << ",\"continuous_goal_tracking\":" << (p->bUseContinuousGoalTracking ? "true" : "false");
            }
            break;
        case DiagTraceKind::SetBBFollowLocation:
            if (Mem::IsReadable(params, sizeof(P_SetBlackboardFollowLocation)))
            {
                auto* p = reinterpret_cast<P_SetBlackboardFollowLocation*>(params);
                os << ",\"location\":"; WriteVectorJson(os, p->Location);
            }
            break;
        case DiagTraceKind::SetBBTargetPtr:
            if (Mem::IsReadable(params, sizeof(void*)))
            {
                void* target = *reinterpret_cast<void**>(params);
                os << ",\"target\":"; WriteObjectJson(os, static_cast<UObject*>(target));
            }
            break;
        case DiagTraceKind::SetBBTargetEnemy:
            if (Mem::IsReadable(params, sizeof(P_SetBlackboardTargetEnemy)))
            {
                auto* p = reinterpret_cast<P_SetBlackboardTargetEnemy*>(params);
                os << ",\"target\":"; WriteObjectJson(os, static_cast<UObject*>(p->NewTarget));
                os << ",\"force_update\":" << (p->bForceUpdate ? "true" : "false");
            }
            break;
        case DiagTraceKind::SetBBBool:
            if (Mem::IsReadable(params, sizeof(P_SetBBBoolDiag)))
            {
                auto* p = reinterpret_cast<P_SetBBBoolDiag*>(params);
                os << ",\"value\":" << (p->bValue ? "true" : "false");
            }
            break;
        case DiagTraceKind::SetBBFloat:
            if (Mem::IsReadable(params, sizeof(P_SetBBFloatDiag)))
            {
                auto* p = reinterpret_cast<P_SetBBFloatDiag*>(params);
                os << ",\"key\":"; WriteFNameJson(os, p->KeyName);
                os << ",\"value\":" << p->Value;
            }
            break;
        case DiagTraceKind::BBSetObject:
            if (Mem::IsReadable(params, sizeof(P_BBKeyObjectDiag)))
            {
                auto* p = reinterpret_cast<P_BBKeyObjectDiag*>(params);
                os << ",\"key\":"; WriteFNameJson(os, p->KeyName);
                os << ",\"value\":"; WriteObjectJson(os, static_cast<UObject*>(p->Value));
            }
            break;
        case DiagTraceKind::BBSetBool:
            if (Mem::IsReadable(params, sizeof(P_BBKeyBoolDiag)))
            {
                auto* p = reinterpret_cast<P_BBKeyBoolDiag*>(params);
                os << ",\"key\":"; WriteFNameJson(os, p->KeyName);
                os << ",\"value\":" << (p->Value ? "true" : "false");
            }
            break;
        case DiagTraceKind::BBSetFloat:
            if (Mem::IsReadable(params, sizeof(P_BBKeyFloatDiag)))
            {
                auto* p = reinterpret_cast<P_BBKeyFloatDiag*>(params);
                os << ",\"key\":"; WriteFNameJson(os, p->KeyName);
                os << ",\"value\":" << p->Value;
            }
            break;
        case DiagTraceKind::BBSetVector:
            if (Mem::IsReadable(params, sizeof(P_BBKeyVectorDiag)))
            {
                auto* p = reinterpret_cast<P_BBKeyVectorDiag*>(params);
                os << ",\"key\":"; WriteFNameJson(os, p->KeyName);
                os << ",\"value\":"; WriteVectorJson(os, p->Value);
            }
            break;
        case DiagTraceKind::MeshVector:
            if (Mem::IsReadable(params, sizeof(P_MeshSetVectorParam)))
            {
                auto* p = reinterpret_cast<P_MeshSetVectorParam*>(params);
                os << ",\"param\":"; WriteFNameJson(os, p->Param);
                os << ",\"value\":"; WriteVectorJson(os, p->Value);
            }
            break;
        case DiagTraceKind::MeshScalar:
            if (Mem::IsReadable(params, sizeof(P_MeshSetScalarParam)))
            {
                auto* p = reinterpret_cast<P_MeshSetScalarParam*>(params);
                os << ",\"param\":"; WriteFNameJson(os, p->Param);
                os << ",\"value\":" << p->Value;
            }
            break;
        default:
            break;
        }
        os << "}";
    }

    // Log EVERY UFunction dispatched on the chosen trace target (or its controller):
    // full function name + raw param bytes. Throttled per-function so a per-frame
    // Tick/anim-notify doesn't flood the file while still capturing each distinct
    // function and how its args evolve -- exactly what's needed to find the native
    // driver of an NPC's walk for a detour hook.
    void WriteTargetTraceEntry(void* obj, void* fn, void* params)
    {
        UObject* fnObj = static_cast<UObject*>(fn);
        {
            // per-function throttle (~5 Hz) under the diag lock
            static std::unordered_map<void*, ULONGLONG> lastMs;
            std::lock_guard<std::mutex> lk(g_diagMutex);
            if (!g_diagTraceFile)
                return;
            ULONGLONG now = GetTickCount64();
            auto it = lastMs.find(fn);
            if (it != lastMs.end() && now - it->second < 200)
                return;
            lastMs[fn] = now;
        }

        std::ostringstream os;
        os << "{\"t_ms\":" << GetTickCount64()
           << ",\"thread\":" << GetCurrentThreadId()
           << ",\"kind\":\"target\""
           << ",\"object\":"; WriteObjectJson(os, static_cast<UObject*>(obj));
        os << ",\"function\":\"" << JsonEscape(SafeObjectFullName(fnObj)) << "\"";
        // Detour payload: the UFunction ptr + native exec pointer (module+rva) + flags.
        // is_native=true => detour exec_native directly; false (BP) => detour
        // ProcessEvent and filter on this ufunction ptr.
        os << ",\"ufunction\":"; WriteObjectJson(os, fnObj);
        uint32_t funcFlags = 0;
        if (Mem::IsReadable(reinterpret_cast<uint8_t*>(fn) + Offsets::O_UFunction_FunctionFlags, 4))
            funcFlags = *reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(fn) + Offsets::O_UFunction_FunctionFlags);
        void* execNative = nullptr;
        if (Mem::IsReadable(reinterpret_cast<uint8_t*>(fn) + Offsets::O_UFunction_ExecFunction, sizeof(void*)))
            execNative = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(fn) + Offsets::O_UFunction_ExecFunction);
        os << ",\"func_flags\":\"0x" << std::hex << funcFlags << std::dec << "\"";
        os << ",\"is_native\":" << ((funcFlags & Offsets::FUNC_Native) ? "true" : "false");
        os << ",\"exec_native\":"; WriteCodeAddrJson(os, execNative);
        int paramSize = 0;
        if (Mem::IsReadable(reinterpret_cast<uint8_t*>(fn) + Offsets::O_UStruct_PropertiesSize, 4))
            paramSize = *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(fn) + Offsets::O_UStruct_PropertiesSize);
        if (paramSize < 0) paramSize = 0;
        if (paramSize > 256) paramSize = 256;
        os << ",\"param_size\":" << paramSize << ",\"params_hex\":\"";
        if (params && paramSize > 0 && Mem::IsReadable(params, (size_t)paramSize))
        {
            const uint8_t* b = reinterpret_cast<const uint8_t*>(params);
            char hx[4];
            for (int i = 0; i < paramSize; ++i) { sprintf_s(hx, "%02X", b[i]); os << hx; }
        }
        os << "\"}";

        std::lock_guard<std::mutex> lk(g_diagMutex);
        if (g_diagTraceFile) { g_diagTraceFile << os.str() << "\n"; g_diagTraceFile.flush(); }
    }

    void DiagnosticTraceProcessEvent(void* obj, void* fn, void* params)
    {
        if (!g_diagTraceActive.load() || !Mem::IsReadable(fn, 0x30))
            return;

        // (A) universal target trace -- every function on the tracked actor/controller.
        UObject* tgt = g_traceTarget.load();
        if (tgt && (obj == tgt || obj == g_traceTargetCtrl.load()))
        {
            try { WriteTargetTraceEntry(obj, fn, params); } catch (...) {}
        }

        // (B) curated known-function trace (movement / blackboard / material).
        DiagTraceFn hit{};
        {
            std::lock_guard<std::mutex> lk(g_diagMutex);
            for (const DiagTraceFn& e : g_diagTraceFns)
                if (e.fn == fn) { hit = e; break; }
            if (!hit.fn || !g_diagTraceFile)
                return;
        }

        std::ostringstream os;
        os << "{\"t_ms\":" << GetTickCount64()
           << ",\"thread\":" << GetCurrentThreadId()
           << ",\"function\":\"" << JsonEscape(hit.label) << "\""
           << ",\"object\":"; WriteObjectJson(os, static_cast<UObject*>(obj));
        os << ",\"params\":";
        WriteTraceParams(os, hit.kind, params);
        os << "}";

        std::lock_guard<std::mutex> lk(g_diagMutex);
        if (g_diagTraceFile)
        {
            g_diagTraceFile << os.str() << "\n";
            g_diagTraceFile.flush();
        }
    }

    void WriteMaterialParamArrayJson(std::ostream& os, UObject* mat, bool vectorParams)
    {
        os << "[";
        if (!Mem::IsReadable(mat, 0x30))
        {
            os << "]";
            return;
        }
        const int arrayOff = vectorParams ? AH::Mat_VectorParameterValues : AH::Mat_ScalarParameterValues;
        const int stride   = vectorParams ? AH::VectorParamValue_Stride : AH::ScalarParamValue_Stride;
        const int valueOff = vectorParams ? AH::VectorParamValue_ValueOff : AH::ScalarParamValue_ValueOff;
        uint8_t* b = reinterpret_cast<uint8_t*>(mat);
        if (!Mem::IsReadable(b + arrayOff, sizeof(TArray<void*>)))
        {
            os << "]";
            return;
        }
        uint8_t* data = *reinterpret_cast<uint8_t**>(b + arrayOff);
        int32_t count = *reinterpret_cast<int32_t*>(b + arrayOff + 8);
        if (count <= 0 || count > 128 || !Mem::IsReadable(data, (size_t)count * stride))
        {
            os << "]";
            return;
        }
        for (int i = 0; i < count; ++i)
        {
            if (i) os << ",";
            uint8_t* entry = data + (size_t)i * stride;
            FName nm = *reinterpret_cast<FName*>(entry);
            os << "{\"name\":"; WriteFNameJson(os, nm);
            if (vectorParams)
            {
                FLinearColor c = *reinterpret_cast<FLinearColor*>(entry + valueOff);
                os << ",\"value\":{\"r\":" << c.R << ",\"g\":" << c.G << ",\"b\":" << c.B << ",\"a\":" << c.A << "}";
            }
            else
            {
                float v = *reinterpret_cast<float*>(entry + valueOff);
                os << ",\"value\":" << v;
            }
            os << "}";
        }
        os << "]";
    }

    void WriteMeshMaterialsJson(std::ostream& os, UObject* mesh)
    {
        os << "{\"mesh\":";
        WriteObjectJson(os, mesh);
        os << ",\"materials\":[";
        UFunction* numFn = CachedFn(AH::Fn_PrimGetNumMaterials);
        UFunction* getFn = CachedFn(AH::Fn_PrimGetMaterial);
        if (Mem::IsReadable(mesh, 0x30) && Mem::IsReadable(numFn, 0x30) && Mem::IsReadable(getFn, 0x30))
        {
            struct { int32_t Ret; } np{};
            mesh->ProcessEvent(numFn, &np);
            int n = np.Ret;
            if (n < 0) n = 0;
            if (n > 32) n = 32;
            for (int i = 0; i < n; ++i)
            {
                if (i) os << ",";
                struct { int32_t ElementIndex; uint8_t pad[4]; void* Ret; } gp{};
                gp.ElementIndex = i;
                mesh->ProcessEvent(getFn, &gp);
                UObject* mat = static_cast<UObject*>(gp.Ret);
                os << "{\"slot\":" << i << ",\"material\":";
                WriteObjectJson(os, mat);
                os << ",\"scalar_params\":";
                WriteMaterialParamArrayJson(os, mat, false);
                os << ",\"vector_params\":";
                WriteMaterialParamArrayJson(os, mat, true);
                os << "}";
            }
        }
        os << "]}";
    }

    bool BBGetObject(UObject* bb, const FName& key, UObject*& out, const char* fnName)
    {
        UFunction* fn = CachedObjectClassFn(bb, fnName);
        if (!Mem::IsReadable(bb, 0x30) || !Mem::IsReadable(fn, 0x30))
            return false;
        P_BBKeyObjectDiag p{};
        p.KeyName = key;
        bb->ProcessEvent(fn, &p);
        out = Mem::IsReadable(p.Value, 0x30) ? static_cast<UObject*>(p.Value) : nullptr;
        return true;
    }

    bool BBGetVector(UObject* bb, const FName& key, FVector& out)
    {
        UFunction* fn = CachedObjectClassFn(bb, "GetValueAsVector");
        if (!Mem::IsReadable(bb, 0x30) || !Mem::IsReadable(fn, 0x30))
            return false;
        P_BBKeyVectorDiag p{};
        p.KeyName = key;
        bb->ProcessEvent(fn, &p);
        out = p.Value;
        return true;
    }

    bool BBGetBool(UObject* bb, const FName& key, bool& out)
    {
        UFunction* fn = CachedObjectClassFn(bb, "GetValueAsBool");
        if (!Mem::IsReadable(bb, 0x30) || !Mem::IsReadable(fn, 0x30))
            return false;
        P_BBKeyBoolDiag p{};
        p.KeyName = key;
        bb->ProcessEvent(fn, &p);
        out = p.Value;
        return true;
    }

    bool BBGetFloat(UObject* bb, const FName& key, float& out)
    {
        UFunction* fn = CachedObjectClassFn(bb, "GetValueAsFloat");
        if (!Mem::IsReadable(bb, 0x30) || !Mem::IsReadable(fn, 0x30))
            return false;
        P_BBKeyFloatDiag p{};
        p.KeyName = key;
        bb->ProcessEvent(fn, &p);
        out = p.Value;
        return true;
    }

    bool BBGetIntLike(UObject* bb, const FName& key, int& out, const char* fnName)
    {
        UFunction* fn = CachedObjectClassFn(bb, fnName);
        if (!Mem::IsReadable(bb, 0x30) || !Mem::IsReadable(fn, 0x30))
            return false;
        struct { FName KeyName; int32_t Value; } p{};
        p.KeyName = key;
        bb->ProcessEvent(fn, &p);
        out = p.Value;
        return true;
    }

    bool BBGetName(UObject* bb, const FName& key, FName& out)
    {
        UFunction* fn = CachedObjectClassFn(bb, "GetValueAsName");
        if (!Mem::IsReadable(bb, 0x30) || !Mem::IsReadable(fn, 0x30))
            return false;
        struct { FName KeyName; FName Value; } p{};
        p.KeyName = key;
        bb->ProcessEvent(fn, &p);
        out = p.Value;
        return true;
    }

    bool BBIsVectorSet(UObject* bb, const FName& key, bool& out)
    {
        UFunction* fn = CachedObjectClassFn(bb, "IsVectorValueSet");
        if (!Mem::IsReadable(bb, 0x30) || !Mem::IsReadable(fn, 0x30))
            return false;
        P_BBKeyBoolDiag p{};
        p.KeyName = key;
        bb->ProcessEvent(fn, &p);
        out = p.Value;
        return true;
    }

    void CollectBlackboardEntries(UObject* data, std::vector<BBEntryRaw>& out, int depth)
    {
        if (!Mem::IsReadable(data, 0x58) || depth > 4 || out.size() >= 160)
            return;
        UObject* parent = ReadUObjectAt(data, AH::BBData_Parent);
        if (parent && parent != data)
            CollectBlackboardEntries(parent, out, depth + 1);

        uint8_t* b = reinterpret_cast<uint8_t*>(data);
        if (!Mem::IsReadable(b + AH::BBData_Keys, sizeof(TArray<BBEntryRaw>)))
            return;
        auto* arr = reinterpret_cast<TArray<BBEntryRaw>*>(b + AH::BBData_Keys);
        int count = arr->Count;
        if (count <= 0 || count > 256 || !Mem::IsReadable(arr->Data, (size_t)count * sizeof(BBEntryRaw)))
            return;
        for (int i = 0; i < count && out.size() < 160; ++i)
            out.push_back(arr->Data[i]);
    }

    void WriteBlackboardJson(std::ostream& os, UObject* bb)
    {
        os << "{\"component\":";
        WriteObjectJson(os, bb);
        if (!Mem::IsReadable(bb, 0x30))
        {
            os << "}";
            return;
        }

        UObject* asset = ReadUObjectAt(bb, AH::BBComp_Asset);
        os << ",\"brain_comp\":"; WriteObjectJson(os, ReadUObjectAt(bb, AH::BBComp_BrainComp));
        os << ",\"default_asset\":"; WriteObjectJson(os, ReadUObjectAt(bb, AH::BBComp_DefaultAsset));
        os << ",\"asset\":"; WriteObjectJson(os, asset);

        int keyInstances = -1;
        uint8_t* b = reinterpret_cast<uint8_t*>(bb);
        if (Mem::IsReadable(b + AH::BBComp_KeyInstances, sizeof(TArray<void*>)))
            keyInstances = reinterpret_cast<TArray<void*>*>(b + AH::BBComp_KeyInstances)->Count;
        os << ",\"key_instances_count\":" << keyInstances;

        std::vector<BBEntryRaw> keys;
        CollectBlackboardEntries(asset, keys, 0);
        os << ",\"keys\":[";
        for (size_t i = 0; i < keys.size(); ++i)
        {
            if (i) os << ",";
            const BBEntryRaw& e = keys[i];
            std::string type = SafeObjectName(e.KeyType);
            std::string typeLower = LowerCopy(type);
            os << "{\"name\":"; WriteFNameJson(os, e.EntryName);
            os << ",\"key_type\":"; WriteObjectJson(os, e.KeyType);
            os << ",\"value\":";
            bool wroteValue = false;
            try
            {
                if (typeLower.find("object") != std::string::npos)
                {
                    UObject* v = nullptr;
                    if (BBGetObject(bb, e.EntryName, v, "GetValueAsObject"))
                    {
                        WriteObjectJson(os, v);
                        wroteValue = true;
                    }
                }
                else if (typeLower.find("class") != std::string::npos)
                {
                    UObject* v = nullptr;
                    if (BBGetObject(bb, e.EntryName, v, "GetValueAsClass"))
                    {
                        WriteObjectJson(os, v);
                        wroteValue = true;
                    }
                }
                else if (typeLower.find("vector") != std::string::npos)
                {
                    FVector v{};
                    bool set = false;
                    if (BBGetVector(bb, e.EntryName, v))
                    {
                        os << "{\"vector\":";
                        WriteVectorJson(os, v);
                        if (BBIsVectorSet(bb, e.EntryName, set))
                            os << ",\"is_set\":" << (set ? "true" : "false");
                        os << "}";
                        wroteValue = true;
                    }
                }
                else if (typeLower.find("bool") != std::string::npos)
                {
                    bool v = false;
                    if (BBGetBool(bb, e.EntryName, v))
                    {
                        os << (v ? "true" : "false");
                        wroteValue = true;
                    }
                }
                else if (typeLower.find("float") != std::string::npos)
                {
                    float v = 0.0f;
                    if (BBGetFloat(bb, e.EntryName, v))
                    {
                        os << v;
                        wroteValue = true;
                    }
                }
                else if (typeLower.find("int") != std::string::npos || typeLower.find("enum") != std::string::npos)
                {
                    int v = 0;
                    const char* fnName = (typeLower.find("enum") != std::string::npos) ? "GetValueAsEnum" : "GetValueAsInt";
                    if (BBGetIntLike(bb, e.EntryName, v, fnName))
                    {
                        os << v;
                        wroteValue = true;
                    }
                }
                else if (typeLower.find("name") != std::string::npos)
                {
                    FName v{};
                    if (BBGetName(bb, e.EntryName, v))
                    {
                        WriteFNameJson(os, v);
                        wroteValue = true;
                    }
                }
            }
            catch (...) {}
            if (!wroteValue)
                os << "null";
            os << "}";
        }
        os << "]}";
    }

    void WriteAiControllerJson(std::ostream& os, UObject* ctrl)
    {
        os << "{\"object\":";
        WriteObjectJson(os, ctrl);
        if (!Mem::IsReadable(ctrl, 0x30))
        {
            os << "}";
            return;
        }
        os << ",\"pawn\":"; WriteObjectJson(os, ReadUObjectAt(ctrl, Offsets::O_BaseController_Pawn));
        os << ",\"path_following\":"; WriteObjectJson(os, ReadUObjectAt(ctrl, AH::AICtrl_PathFollowing));
        os << ",\"brain_comp\":"; WriteObjectJson(os, ReadUObjectAt(ctrl, AH::AICtrl_BrainComp));
        os << ",\"blackboard_ptr\":"; WriteObjectJson(os, ReadUObjectAt(ctrl, AH::AICtrl_Blackboard));

        uint8_t team = 0;
        os << ",\"team_byte_raw\":";
        if (ReadUInt8Field(ctrl, AH::AICtrl_TeamID, team)) os << (int)team; else os << "null";

        uint8_t moveStatus = 255;
        if (UFunction* fn = CachedObjectClassFn(ctrl, "GetMoveStatus"))
        {
            if (Mem::IsReadable(fn, 0x30))
            {
                struct { uint8_t ReturnValue; } p{};
                ctrl->ProcessEvent(fn, &p);
                moveStatus = p.ReturnValue;
            }
        }
        os << ",\"move_status\":" << (int)moveStatus;

        UObject* path = ReadUObjectAt(ctrl, AH::AICtrl_PathFollowing);
        os << ",\"path_details\":{\"movement_comp\":";
        WriteObjectJson(os, ReadUObjectAt(path, AH::PathFollow_MovementComp));
        os << ",\"nav_data\":";
        WriteObjectJson(os, ReadUObjectAt(path, AH::PathFollow_NavData));
        if (Mem::IsReadable(path, 0x30))
        {
            if (UFunction* fn = CachedObjectClassFn(path, "GetPathActionType"))
            {
                if (Mem::IsReadable(fn, 0x30))
                {
                    struct { uint8_t ReturnValue; } p{};
                    path->ProcessEvent(fn, &p);
                    os << ",\"path_action_type\":" << (int)p.ReturnValue;
                }
            }
            if (UFunction* fn = CachedObjectClassFn(path, "GetPathDestination"))
            {
                if (Mem::IsReadable(fn, 0x30))
                {
                    struct { FVector ReturnValue; } p{};
                    path->ProcessEvent(fn, &p);
                    os << ",\"path_destination\":";
                    WriteVectorJson(os, p.ReturnValue);
                }
            }
        }
        os << "}";

        struct KeyField { const char* name; int off; };
        static const KeyField keyFields[] = {
            { "SelfActor", AH::AICtrl_Key_SelfActor },
            { "TargetEnemy", AH::AICtrl_Key_TargetEnemy },
            { "TargetAlly", AH::AICtrl_Key_TargetAlly },
            { "TargetObject", AH::AICtrl_Key_TargetObject },
            { "CurrentWaypoint", AH::AICtrl_Key_CurrentWaypoint },
            { "Duration", AH::AICtrl_Key_Duration },
            { "FollowLocation", AH::AICtrl_Key_FollowLocation },
            { "ForceFollowLocation", AH::AICtrl_Key_ForceFollowLoc },
            { "IsAggressive", AH::AICtrl_Key_IsAggressive },
            { "CanReachFollowLocation", AH::AICtrl_Key_CanReachFollowLoc },
            { "BehaviorState", AH::AICtrl_Key_BehaviorState },
            { "AcceptableRadius", AH::AICtrl_Key_AcceptableRadius },
        };
        os << ",\"ah_key_names\":{";
        for (int i = 0; i < (int)(sizeof(keyFields) / sizeof(keyFields[0])); ++i)
        {
            if (i) os << ",";
            FName nm{};
            os << "\"" << keyFields[i].name << "\":";
            if (ReadFNameAt(ctrl, keyFields[i].off, nm)) WriteFNameJson(os, nm); else os << "null";
        }
        os << "},\"blackboard\":";
        WriteBlackboardJson(os, ReadUObjectAt(ctrl, AH::AICtrl_Blackboard));
        os << "}";
    }

    void WriteMovementJson(std::ostream& os, UObject* pawn)
    {
        UObject* mv = ReadUObjectAt(pawn, AH::Char_CharacterMovement);
        os << "{\"component\":";
        WriteObjectJson(os, mv);
        if (Mem::IsReadable(mv, 0x30))
        {
            float f = 0.0f;
            uint8_t mode = 0;
            if (ReadFloatField(mv, AH::Move_MaxWalkSpeed, f)) os << ",\"max_walk_speed\":" << f;
            if (ReadFloatField(mv, AH::Move_MaxFlySpeed, f)) os << ",\"max_fly_speed\":" << f;
            if (ReadFloatField(mv, AH::Move_GravityScale, f)) os << ",\"gravity_scale\":" << f;
            if (ReadFloatField(mv, AH::Move_JumpZVelocity, f)) os << ",\"jump_z_velocity\":" << f;
            if (ReadFloatField(mv, AH::Move_AirControl, f)) os << ",\"air_control\":" << f;
            if (ReadUInt8Field(mv, AH::Move_MovementMode, mode)) os << ",\"movement_mode\":" << (int)mode;
        }
        os << "}";
    }

    void WriteAttributesJson(std::ostream& os, UObject* ch)
    {
        os << "{";
        UObject* set = ReadUObjectAt(ch, AH::Char_AttributeSet);
        os << "\"set\":"; WriteObjectJson(os, set);
        float cur = 0.0f, mx = 0.0f;
        if (ReadCharacterHealth(ch, cur, mx))
            os << ",\"health\":" << cur << ",\"max_health\":" << mx;
        float v = 0.0f;
        if (ReadFloatField(set, AH::Set_Stamina + AH::Attr_CurrentValue, v)) os << ",\"stamina\":" << v;
        if (ReadFloatField(set, AH::Set_MaxStamina + AH::Attr_CurrentValue, v)) os << ",\"max_stamina\":" << v;
        if (ReadFloatField(set, AH::Set_Energy + AH::Attr_CurrentValue, v)) os << ",\"energy\":" << v;
        if (ReadFloatField(set, AH::Set_MaxEnergy + AH::Attr_CurrentValue, v)) os << ",\"max_energy\":" << v;
        os << "}";
    }

    void WriteAiJson(std::ostream& os, std::ostream& hex, UObject* ai, UObject* player, const FVector& playerLoc, bool includeMaterials)
    {
        os << "{\"actor\":";
        WriteObjectJson(os, ai);
        if (!Mem::IsReadable(ai, 0x30))
        {
            os << "}";
            return;
        }
        FVector loc{};
        if (ReadActorLocationFast(ai, loc))
        {
            os << ",\"location\":"; WriteVectorJson(os, loc);
            if (Mem::IsReadable(player, 0x30))
                os << ",\"distance_m\":" << DistanceMetres(loc, playerLoc);
        }
        float cur = 0.0f, mx = 0.0f;
        if (ReadCharacterHealth(ai, cur, mx))
            os << ",\"health\":" << cur << ",\"max_health\":" << mx;

        bool bv = false;
        os << ",\"flags\":{";
        bool first = true;
        if (ReadBoolField(ai, AH::Char_bIsDead, bv)) { os << "\"b_is_dead\":" << (bv ? "true" : "false"); first = false; }
        if (ReadBoolField(ai, AH::AICh_bIsAlwaysAggressive, bv)) { if (!first) os << ","; os << "\"b_always_aggressive\":" << (bv ? "true" : "false"); first = false; }
        if (ReadBoolField(ai, AH::AICh_bIsPassive, bv)) { if (!first) os << ","; os << "\"b_passive\":" << (bv ? "true" : "false"); first = false; }
        if (ReadBoolField(ai, AH::AICh_bPassiveButWithSenses, bv)) { if (!first) os << ","; os << "\"b_passive_with_senses\":" << (bv ? "true" : "false"); first = false; }
        if (ReadBoolField(ai, AH::AICh_bActorTickEnabled, bv)) { if (!first) os << ","; os << "\"b_actor_tick_enabled\":" << (bv ? "true" : "false"); first = false; }
        if (ReadBoolField(ai, AH::AICh_bMeshTickEnabled, bv)) { if (!first) os << ","; os << "\"b_mesh_tick_enabled\":" << (bv ? "true" : "false"); first = false; }
        os << "}";

        uint8_t team = 0;
        os << ",\"team_id\":";
        if (ReadAiTeamId(ai, team)) os << (int)team; else os << "null";
        os << ",\"cached_target_enemy\":"; WriteObjectJson(os, ReadUObjectAt(ai, AH::AICh_CachedTargetEnemy));
        os << ",\"last_sensed_character\":"; WriteObjectJson(os, ReadUObjectAt(ai, AH::AICh_LastSensedCharacter));
        os << ",\"owner_character\":"; WriteObjectJson(os, ReadUObjectAt(ai, AH::AICh_OwnerCharacter));
        os << ",\"raw_pawn_controller\":"; WriteObjectJson(os, ReadUObjectAt(ai, Offsets::O_Pawn_Controller));
        os << ",\"controller\":";
        UObject* aiCtrl = GetAiController(ai);
        WriteAiControllerJson(os, aiCtrl);
        os << ",\"movement\":";
        WriteMovementJson(os, ai);
        os << ",\"mesh\":";
        UObject* mesh = EnemyMesh(ai);
        if (includeMaterials) WriteMeshMaterialsJson(os, mesh); else WriteObjectJson(os, mesh);
        os << ",\"pointer_scan\":";
        WritePointerScanJson(os, ai, 0x700, 48);
        // NOTE: full named-field reflection of AI actors/controllers is intentionally
        // NOT done here -- it would add ~3-4ms PER object on the game thread. Use the
        // worker-thread "Dump targeted actor" / live target dump for that (zero
        // game-thread cost). The player pawn + weapon ARE reflected inline (few
        // objects, RGB-relevant), which stays cheap.
        os << "}";

        DumpHexWindow(hex, "ai_actor", ai, 0x300);
        UObject* ctrl = GetAiController(ai);
        DumpHexWindow(hex, "ai_controller", ctrl, 0x300);
        DumpHexWindow(hex, "ai_blackboard", ReadUObjectAt(ctrl, AH::AICtrl_Blackboard), 0x200);
    }

    void WriteWeaponJson(std::ostream& os, std::ostream& hex, UObject* pawn)
    {
        UObject* weapon = Mem::IsReadable(pawn, 0x30) ? GetCurrentWeaponObject(pawn) : nullptr;
        os << "{\"weapon\":";
        WriteObjectJson(os, weapon);
        if (!Mem::IsReadable(weapon, 0x30))
        {
            os << "}";
            return;
        }
        UObject* mesh = ReadUObjectAt(weapon, AH::Weapon_Mesh);
        UObject* itemData = ReadUObjectAt(weapon, AH::Weapon_ItemDataAsset);
        UObject* barrel = static_cast<UObject*>(ReadPtrField(weapon, AH::RangeWeapon_Barrel));
        os << ",\"item_data_asset\":"; WriteObjectJson(os, itemData);
        os << ",\"mesh_field_0x2B8\":"; WriteObjectJson(os, mesh);
        os << ",\"barrel\":"; WriteObjectJson(os, barrel);
        os << ",\"bullet_class\":"; WriteObjectJson(os, static_cast<UObject*>(ResolveBulletClass(weapon, barrel)));
        os << ",\"loaded_ammo\":" << QueryWeaponLoadedAmmo(weapon);
        os << ",\"reserve_ammo\":" << QueryWeaponReserveAmmo(pawn, weapon);
        int32_t iv = 0;
        if (ReadInt32Field(weapon, AH::Weapon_AmmoSize, iv)) os << ",\"ammo_size_field\":" << iv;
        if (ReadInt32Field(weapon, AH::Weapon_StartAmmoCount, iv)) os << ",\"start_ammo_field\":" << iv;
        os << ",\"materials\":";
        WriteMeshMaterialsJson(os, mesh);
        os << ",\"pointer_scan\":";
        WritePointerScanJson(os, weapon, 0xC00, 96);
        os << ",\"reflection\":";
        Reflect::DumpObjectJson(os, weapon);
        os << ",\"vtable\":"; WriteVTableJson(os, weapon, 48);
        os << ",\"mesh_reflection\":";
        Reflect::DumpObjectJson(os, mesh);
        if (mesh) { os << ",\"mesh_vtable\":"; WriteVTableJson(os, mesh, 48); }
        os << "}";
        DumpHexWindow(hex, "weapon", weapon, 0x400);
        DumpHexWindow(hex, "weapon_mesh", mesh, 0x300);
        DumpHexWindow(hex, "weapon_barrel", barrel, 0x300);
    }

    void WriteWorldJson(std::ostream& os, bool full, UObject* player, const FVector& playerLoc)
    {
        std::vector<UObject*> actors;
        try { CollectLevelActors(actors); } catch (...) {}
        std::unordered_map<std::string, int> classCounts;
        int located = 0;
        struct ActorRow { UObject* actor; FVector loc; float dist; bool haveLoc; bool interesting; };
        std::vector<ActorRow> rows;
        rows.reserve((std::min)((int)actors.size(), full ? 512 : 160));
        for (UObject* a : actors)
        {
            if (!Mem::IsReadable(a, 0x30))
                continue;
            std::string cls = SafeClassName(a);
            if (!cls.empty()) ++classCounts[cls];
            FVector loc{};
            bool haveLoc = ReadActorLocationFast(a, loc);
            if (haveLoc) ++located;
            std::string fullName = SafeObjectFullName(a);
            bool interesting = ContainsAnyTerm(fullName);
            float d = (haveLoc && Mem::IsReadable(player, 0x30)) ? DistanceMetres(loc, playerLoc) : 3.4e38f;
            if (interesting || (haveLoc && d < (full ? 200.0f : 80.0f)))
                rows.push_back({ a, loc, d, haveLoc, interesting });
        }
        std::sort(rows.begin(), rows.end(), [](const ActorRow& a, const ActorRow& b)
        {
            if (a.interesting != b.interesting) return a.interesting > b.interesting;
            return a.dist < b.dist;
        });
        const size_t maxRows = full ? 320u : 96u;
        if (rows.size() > maxRows) rows.resize(maxRows);

        std::vector<std::pair<std::string, int>> hist(classCounts.begin(), classCounts.end());
        std::sort(hist.begin(), hist.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
        if (hist.size() > 96) hist.resize(96);

        os << "{\"world\":"; WriteObjectJson(os, GetWorld());
        os << ",\"actor_count_loaded_levels\":" << actors.size()
           << ",\"actors_with_location\":" << located
           << ",\"class_histogram\":[";
        for (size_t i = 0; i < hist.size(); ++i)
        {
            if (i) os << ",";
            os << "{\"class\":\"" << JsonEscape(hist[i].first) << "\",\"count\":" << hist[i].second << "}";
        }
        os << "],\"near_or_interesting_actors\":[";
        for (size_t i = 0; i < rows.size(); ++i)
        {
            if (i) os << ",";
            os << "{\"actor\":";
            WriteObjectJson(os, rows[i].actor);
            os << ",\"interesting\":" << (rows[i].interesting ? "true" : "false");
            if (rows[i].haveLoc)
            {
                os << ",\"location\":";
                WriteVectorJson(os, rows[i].loc);
                os << ",\"distance_m\":" << rows[i].dist;
            }
            os << "}";
        }
        os << "]}";
    }

    void WriteGObjectMatches(const std::string& dir)
    {
        std::ofstream f(PathJoin(dir, "gobjects_matching_follow_ai_weapon.txt"), std::ios::out | std::ios::trunc | std::ios::binary);
        if (!f) return;
        int n = NumObjects();
        int hits = 0;
        f << "Terms: follow escort bodyguard companion larisa quest boss robot move blackboard behavior weapon material mesh rgb ai\n";
        for (int i = 0; i < n && hits < 2500; ++i)
        {
            UObject* o = GetObjectByIndex(i);
            if (!Mem::IsReadable(o, 0x30))
                continue;
            std::string full = SafeObjectFullName(o);
            if (!ContainsAnyTerm(full))
                continue;
            f << i << "\t" << PtrHex(o) << "\t" << full << "\n";
            ++hits;
        }
        f << "hits_written=" << hits << " total_objects=" << n << "\n";
    }

    void WriteDiagnosticsManifest(const std::string& dir, const char* mode)
    {
        std::ostringstream os;
        os << "{\n"
           << "  \"schema\": \"AtomicHeartMenu.diagnostics.v1\",\n"
           << "  \"mode\": \"" << mode << "\",\n"
           << "  \"created_local\": \"" << JsonEscape(LocalTimestampIso()) << "\",\n"
           << "  \"directory\": \"" << JsonEscape(dir) << "\",\n"
           << "  \"sdk_ready\": " << (G::sdkReady.load() ? "true" : "false") << ",\n"
           << "  \"notes\": \"Read-only diagnostics: AI/world/player/weapon/material snapshots plus optional ProcessEvent trace.\"\n"
           << "}\n";
        WriteTextFile(PathJoin(dir, "manifest.json"), os.str());
    }

    // ---- off-thread snapshot writer ---------------------------------------
    // The snapshot is BUILT in memory on the game thread (so the ProcessEvent
    // reads inside it are safe), but the heavy disk write + the full 360k-object
    // GObjects match scan run here, on a detached worker thread (raw reads + file
    // IO only). That is the fix for "the game freezes every time it saves the
    // log": the game thread no longer does the multi-hundred-ms scan/disk work.
    struct DiagWriteJob
    {
        std::string jsonPath, hexPath, gobjDir, json, hex;
        bool full = false;
    };
    DWORD WINAPI DiagWriteThread(LPVOID param)
    {
        DiagWriteJob* job = reinterpret_cast<DiagWriteJob*>(param);
        try
        {
            WriteTextFile(job->jsonPath, job->json);
            if (!job->hex.empty()) WriteTextFile(job->hexPath, job->hex);
            if (job->full) { try { WriteGObjectMatches(job->gobjDir); } catch (...) {} }
            LOG("Diagnostics: wrote %s (%zu bytes, off-thread)", job->jsonPath.c_str(), job->json.size());
        }
        catch (...) { LOG("Diagnostics: off-thread write exception (ignored)"); }
        delete job;
        g_diagSnapshotInFlight = false; // ownership ends here
        return 0;
    }
    bool OffloadDiagWrite(DiagWriteJob* job)
    {
        HANDLE t = CreateThread(nullptr, 0, DiagWriteThread, job, 0, nullptr);
        if (!t) { LOG("Diagnostics: write-thread spawn failed err=%lu", GetLastError()); delete job; return false; }
        CloseHandle(t);
        return true;
    }

    // Returns true if a worker thread took ownership (it will clear the in-flight
    // flag); false means the caller must release it.
    bool WriteDiagnosticSnapshotNow(const std::string& dir, const std::string& stem, bool full)
    {
        std::ostringstream os;
        std::ostringstream hex;

        UObject* world = GetWorld();
        UObject* gi = GetGameInstance();
        UObject* lp = GetLocalPlayer();
        UObject* pc = GetPlayerController();
        UObject* pawn = GetLocalPawn();
        FVector playerLoc{};
        bool havePlayerLoc = ReadLocalPawnLocationFast(playerLoc, &pawn);

        os << "{\n";
        os << "\"schema\":\"AtomicHeartMenu.diagnostics.v1\",";
        os << "\"snapshot\":\"" << JsonEscape(stem) << "\",";
        os << "\"full\":" << (full ? "true" : "false") << ",";
        os << "\"time_local\":\"" << JsonEscape(LocalTimestampIso()) << "\",";
        os << "\"tick_ms\":" << GetTickCount64() << ",";
        os << "\"thread\":" << GetCurrentThreadId() << ",";
        os << "\"sdk_ready\":" << (G::sdkReady.load() ? "true" : "false") << ",";
        os << "\"engine\":{\"gworld\":"; WriteObjectJson(os, world);
        os << ",\"game_instance\":"; WriteObjectJson(os, gi);
        os << ",\"local_player\":"; WriteObjectJson(os, lp);
        os << ",\"player_controller\":"; WriteObjectJson(os, pc);
        os << ",\"process_event_target\":"; WriteCodeAddrJson(os, g_peTarget); // universal hook point (module+rva)
        os << "},";

        os << "\"player\":{\"pawn\":"; WriteObjectJson(os, pawn);
        if (havePlayerLoc) { os << ",\"location\":"; WriteVectorJson(os, playerLoc); }
        os << ",\"attributes\":"; WriteAttributesJson(os, pawn);
        os << ",\"movement\":"; WriteMovementJson(os, pawn);
        os << ",\"pointer_scan\":"; WritePointerScanJson(os, pawn, 0x2000, 96);
        os << ",\"reflection\":"; Reflect::DumpObjectJson(os, pawn);
        os << ",\"vtable\":"; WriteVTableJson(os, pawn, 48);
        os << ",\"components\":["; WriteActorComponentsForDetour(os, pawn); os << "]";
        os << "},";

        os << "\"weapon\":";
        WriteWeaponJson(os, hex, pawn);
        os << ",";

        os << "\"world_snapshot\":";
        WriteWorldJson(os, full, pawn, playerLoc);
        os << ",";

        std::vector<AiCachedActor> cached = CopyAiSnapshot();
        std::vector<UObject*> aiList;
        aiList.reserve(cached.size() + 32);
        for (const AiCachedActor& e : cached)
            if (Mem::IsReadable(e.actor, 0x30))
                aiList.push_back(e.actor);
        {
            std::lock_guard<std::mutex> lk(g_squadMutex);
            for (UObject* s : g_spawnedAllies)
            {
                bool dup = false;
                for (UObject* e : aiList) if (e == s) { dup = true; break; }
                if (!dup && Mem::IsReadable(s, 0x30)) aiList.push_back(s);
            }
        }
        // ALWAYS capture the universal trace target (if it's an AHAICharacter, e.g.
        // Larisa) even when AI discovery is off -- this records its controller's LIVE
        // BLACKBOARD VALUES (FollowLocation / ForceFollowLocation / schedule keys)
        // each snapshot, which is exactly what reveals how the game drives its walk.
        if (UObject* tgt = g_traceTarget.load())
        {
            bool dup = false;
            for (UObject* e : aiList) if (e == tgt) { dup = true; break; }
            if (!dup && AiUsable(tgt))
                aiList.insert(aiList.begin(), tgt); // front so it's never dropped by the cap
        }
        const size_t maxAi = full ? 128u : 32u;
        if (aiList.size() > maxAi) aiList.resize(maxAi);
        os << "\"ai\":{\"cached_count\":" << cached.size()
           << ",\"squad_count\":" << g_spawnedAllyCount.load()
           << ",\"entries\":[";
        for (size_t i = 0; i < aiList.size(); ++i)
        {
            if (i) os << ",";
            WriteAiJson(os, hex, aiList[i], pawn, playerLoc, full && i < 8);
        }
        os << "]},";

        os << "\"function_pointers\":{";
        struct FnRow { const char* label; const char* name; };
        static const FnRow fnRows[] = {
            { "MoveToActor", "Function /Script/AIModule.AIController.MoveToActor" },
            { "MoveToLocation", "Function /Script/AIModule.AIController.MoveToLocation" },
            { "SimpleMoveToActor", "Function /Script/AIModule.AIBlueprintHelperLibrary.SimpleMoveToActor" },
            { "CreateMoveToProxyObject", "Function /Script/AIModule.AIBlueprintHelperLibrary.CreateMoveToProxyObject" },
            { "AITask_MoveTo", "Function /Script/AIModule.AITask_MoveTo.AIMoveTo" },
            { "SetBlackboardFollowLocation", "Function /Script/AtomicHeart.AHAIController.SetBlackboardFollowLocation" },
            { "SetBlackboardTargetAlly", "Function /Script/AtomicHeart.AHAIController.SetBlackboardTargetAlly" },
            { "SetFollowLocationSpeed", "Function /Script/AtomicHeart.AHAIController.SetFollowLocationSpeed" },
            { "MeshVectorParam", "Function /Script/Engine.MeshComponent.SetVectorParameterValueOnMaterials" },
            { "MeshScalarParam", "Function /Script/Engine.MeshComponent.SetScalarParameterValueOnMaterials" },
        };
        for (int i = 0; i < (int)(sizeof(fnRows) / sizeof(fnRows[0])); ++i)
        {
            if (i) os << ",";
            UFunction* fn = CachedFn(fnRows[i].name);
            os << "\"" << fnRows[i].label << "\":";
            WriteObjectJson(os, fn);
        }
        os << "}\n}\n";

        DumpHexWindow(hex, "world", world, 0x200);
        DumpHexWindow(hex, "player_controller", pc, 0x300);
        DumpHexWindow(hex, "player_pawn", pawn, 0x500);

        DiagWriteJob* job = new DiagWriteJob();
        job->jsonPath = PathJoin(dir, stem + ".json");
        job->hexPath  = PathJoin(dir, stem + "_hex_windows.txt");
        job->gobjDir  = dir;
        job->json     = os.str();
        job->hex      = hex.str();
        job->full     = full;
        return OffloadDiagWrite(job); // worker writes files + GObjects scan, then clears in-flight
    }

    void ScheduleDiagnosticSnapshot(const std::string& dir, const std::string& stem, bool full)
    {
        if (dir.empty())
            return;
        if (g_diagSnapshotInFlight.exchange(true))
        {
            LOG("Diagnostics: snapshot already in flight");
            return;
        }
        if (!InstallProcessEventHook())
        {
            g_diagSnapshotInFlight = false;
            LOG("Diagnostics: ProcessEvent hook unavailable; try again once in-game");
            return;
        }
        QueueGameThread([dir, stem, full]()
        {
            bool offloaded = false;
            try { offloaded = WriteDiagnosticSnapshotNow(dir, stem, full); }
            catch (...) { LOG("Diagnostics: snapshot exception (ignored)"); }
            // On success a worker thread owns the in-flight flag and clears it when
            // the disk write finishes; only release here if no worker was started.
            if (!offloaded) g_diagSnapshotInFlight = false;
        });
    }

    void StartDiagnosticsToggle()
    {
        std::string dir = CreateToggleDiagnosticsDir();
        if (dir.empty())
        {
            LOG("Diagnostics toggle: failed to create directory");
            return;
        }
        ResolveDiagnosticTraceFns();
        WriteDiagnosticsManifest(dir, "toggle");
        {
            std::lock_guard<std::mutex> lk(g_diagMutex);
            if (g_diagTraceFile) g_diagTraceFile.close();
            g_diagTraceFile.open(PathJoin(dir, "trace.jsonl"), std::ios::out | std::ios::trunc | std::ios::binary);
            g_diagToggleDir = dir;
            g_diagLastDir = dir;
            g_diagSnapshotSeq = 0;
            g_diagLastSnapshotMs = 0;
        }
        g_diagTraceActive = true;
        LOG("Diagnostics toggle ON. Saving to: %s", dir.c_str());
        ScheduleDiagnosticSnapshot(dir, "snapshot_0000_full", true);
    }

    void StopDiagnosticsToggle()
    {
        std::string dir;
        {
            std::lock_guard<std::mutex> lk(g_diagMutex);
            dir = g_diagToggleDir;
        }
        if (!dir.empty())
            ScheduleDiagnosticSnapshot(dir, "snapshot_stop_full", true);
        g_diagTraceActive = false;
        {
            std::lock_guard<std::mutex> lk(g_diagMutex);
            if (g_diagTraceFile)
            {
                g_diagTraceFile << "{\"event\":\"trace_stop\",\"t_ms\":" << GetTickCount64() << "}\n";
                g_diagTraceFile.flush();
                g_diagTraceFile.close();
            }
            g_diagToggleDir.clear();
        }
        LOG("Diagnostics toggle OFF. Last directory: %s", dir.c_str());
    }

    void RequestManualDiagnosticSnapshot()
    {
        if (!G::sdkReady.load())
        {
            LOG("Diagnostics: SDK not ready");
            return;
        }
        std::string dir = CreateManualDiagnosticsDir();
        if (dir.empty())
        {
            LOG("Diagnostics: failed to create manual snapshot directory");
            return;
        }
        WriteDiagnosticsManifest(dir, "manual");
        {
            std::lock_guard<std::mutex> lk(g_diagMutex);
            g_diagLastDir = dir;
        }
        LOG("Diagnostics: manual snapshot directory: %s", dir.c_str());
        ScheduleDiagnosticSnapshot(dir, "snapshot_full", true);
    }

    // ---- targeted discovery dump (worker thread; reflection only) ----------
    // Dump EVERY actor in the loaded levels whose class/name/full path contains a
    // substring (e.g. "Larisa", "ReasignTargetOnFollow"), with full named-field
    // reflection of the actor + its controller + movement component + hex windows.
    // Pure raw reads -> runs entirely on a detached worker thread, so it can NEVER
    // freeze the game. The primary "discover an unknown actor by name" tool.
    std::string SanitizeFileToken(const std::string& s)
    {
        std::string o;
        for (char c : s)
            o += (std::isalnum((unsigned char)c) ? c : '_');
        if (o.empty()) o = "target";
        if (o.size() > 40) o.resize(40);
        return o;
    }

    // Scan an actor's pointer fields for sub-objects (components) and dump each with
    // its vtable -> every subsystem ON the actor (movement, mesh, ability system, and
    // crucially the MERCUNA nav component that drives Larisa) becomes a detour target
    // from a single entry. Broad on purpose (any UObject ptr) -- more is better for
    // discovery; capped + deduped so it stays bounded.
    void WriteActorComponentsForDetour(std::ostream& os, UObject* actor)
    {
        if (!Mem::IsReadable(actor, 0x30)) return;
        uint8_t* b = reinterpret_cast<uint8_t*>(actor);
        std::vector<UObject*> seen;
        bool first = true;
        for (int off = 0; off + 8 <= 0x1500; off += 8)
        {
            if (!Mem::IsReadable(b + off, sizeof(void*))) continue;
            void* raw = *reinterpret_cast<void**>(b + off);
            UObject* u = Mem::IsReadable(raw, 0x30) ? static_cast<UObject*>(raw) : nullptr;
            if (!u) continue;
            if (SafeClassName(u).empty()) continue;
            void** vt = *reinterpret_cast<void***>(u);
            if (!Mem::IsReadable(vt, sizeof(void*))) continue;
            bool dup = false; for (UObject* s : seen) if (s == u) { dup = true; break; }
            if (dup) continue;
            seen.push_back(u);
            if (seen.size() > 64) break;
            if (!first) os << ",";
            first = false;
            os << "{\"offset\":\"0x" << std::hex << off << std::dec << "\",\"object\":";
            WriteObjectJson(os, u);
            os << ",\"vtable\":"; WriteVTableJson(os, u, 24);
            os << "}";
        }
    }

    void WriteTargetedActorDump(const std::string& dir, const std::string& substr)
    {
        std::string stem = "targeted_" + SanitizeFileToken(substr);
        std::ostringstream os, hex;
        std::vector<UObject*> actors;
        try { CollectLevelActors(actors); } catch (...) {}

        UObject* player = GetLocalPawn();
        FVector playerLoc{};
        bool havePlayerLoc = ReadActorLocationFast(player, playerLoc);

        std::string needle = LowerCopy(substr);
        os << "{\"schema\":\"AtomicHeartMenu.diagnostics.v1\",";
        os << "\"kind\":\"targeted\",\"target_substr\":\"" << JsonEscape(substr) << "\",";
        os << "\"time_local\":\"" << JsonEscape(LocalTimestampIso()) << "\",";
        os << "\"player\":"; WriteObjectJson(os, player);
        os << ",\"matches\":[";
        int matched = 0;
        for (UObject* a : actors)
        {
            if (matched >= 24) break;
            if (!Mem::IsReadable(a, 0x30)) continue;
            std::string full = LowerCopy(SafeObjectFullName(a));
            std::string cls = LowerCopy(SafeClassName(a));
            if (full.find(needle) == std::string::npos && cls.find(needle) == std::string::npos)
                continue;
            if (matched) os << ",";
            ++matched;
            os << "{\"actor\":"; WriteObjectJson(os, a);
            FVector loc{};
            if (ReadActorLocationFast(a, loc))
            {
                os << ",\"location\":"; WriteVectorJson(os, loc);
                if (havePlayerLoc) os << ",\"distance_m\":" << DistanceMetres(loc, playerLoc);
            }
            os << ",\"reflection\":"; Reflect::DumpObjectJson(os, a);
            os << ",\"vtable\":"; WriteVTableJson(os, a, 48); // native virtuals (Tick, etc.) to detour
            UObject* ctrl = ReadUObjectAt(a, Offsets::O_Pawn_Controller);
            os << ",\"controller\":"; WriteObjectJson(os, ctrl);
            if (ctrl)
            {
                os << ",\"controller_reflection\":"; Reflect::DumpObjectJson(os, ctrl);
                os << ",\"controller_vtable\":"; WriteVTableJson(os, ctrl, 48);
            }
            UObject* mv = ReadUObjectAt(a, AH::Char_CharacterMovement);
            os << ",\"movement\":"; WriteObjectJson(os, mv);
            if (mv)
            {
                os << ",\"movement_reflection\":"; Reflect::DumpObjectJson(os, mv);
                os << ",\"movement_vtable\":"; WriteVTableJson(os, mv, 64); // CMC virtuals
            }
            UObject* mesh = ReadUObjectAt(a, AH::Char_Mesh);
            os << ",\"mesh\":"; WriteObjectJson(os, mesh);
            if (mesh) { os << ",\"mesh_vtable\":"; WriteVTableJson(os, mesh, 48); }
            // Also dump every sub-object/component (offset + ref + vtable) so ANY
            // subsystem on the actor (incl. the Mercuna nav component) is detourable.
            os << ",\"components\":["; WriteActorComponentsForDetour(os, a); os << "]";
            os << "}";
            DumpHexWindow(hex, "targeted_actor", a, 0x800);
            DumpHexWindow(hex, "targeted_controller", ctrl, 0x400);
            DumpHexWindow(hex, "targeted_movement", mv, 0x300);
        }
        os << "],\"matched\":" << matched << "}\n";

        WriteTextFile(PathJoin(dir, stem + ".json"), os.str());
        WriteTextFile(PathJoin(dir, stem + "_hex_windows.txt"), hex.str());
        LOG("Targeted dump '%s': matched=%d actor(s) -> %s", substr.c_str(), matched, dir.c_str());
    }

    struct TargetedDumpJob { std::string dir, substr; };
    DWORD WINAPI TargetedDumpThread(LPVOID param)
    {
        TargetedDumpJob* job = reinterpret_cast<TargetedDumpJob*>(param);
        try { WriteTargetedActorDump(job->dir, job->substr); }
        catch (...) { LOG("Targeted dump: exception (ignored)"); }
        delete job;
        g_targetedDumpInFlight = false;
        return 0;
    }

    void ScheduleTargetedDump(const std::string& substr)
    {
        if (!G::sdkReady.load()) { LOG("Targeted dump: SDK not ready"); return; }
        if (substr.empty()) { LOG("Targeted dump: empty name"); return; }
        if (g_targetedDumpInFlight.exchange(true)) { LOG("Targeted dump already running"); return; }
        std::string dir = CreateManualDiagnosticsDir();
        if (dir.empty()) { g_targetedDumpInFlight = false; LOG("Targeted dump: dir create failed"); return; }
        WriteDiagnosticsManifest(dir, "targeted");
        { std::lock_guard<std::mutex> lk(g_diagMutex); g_diagLastDir = dir; }
        TargetedDumpJob* job = new TargetedDumpJob{ dir, substr };
        HANDLE t = CreateThread(nullptr, 0, TargetedDumpThread, job, 0, nullptr);
        if (!t) { delete job; g_targetedDumpInFlight = false; LOG("Targeted dump: thread spawn failed"); return; }
        CloseHandle(t);
        LOG("Targeted dump '%s' started -> %s", substr.c_str(), dir.c_str());
    }

    // Resolve the nearest actor matching `substr` and latch it (+ its controller)
    // as the universal ProcessEvent trace target. Empty clears it.
    void SetDiagTraceTargetByName(const std::string& substr)
    {
        if (substr.empty())
        {
            g_traceTarget = nullptr; g_traceTargetCtrl = nullptr;
            { std::lock_guard<std::mutex> lk(g_diagMutex); g_traceTargetName.clear(); }
            LOG("Trace target cleared");
            return;
        }
        std::vector<UObject*> actors;
        try { CollectLevelActors(actors); } catch (...) {}
        FVector pl{};
        bool havePl = ReadLocalPawnLocationFast(pl);
        std::string needle = LowerCopy(substr);
        UObject* best = nullptr; float bestD = 3.4e38f;
        for (UObject* a : actors)
        {
            if (!Mem::IsReadable(a, 0x30)) continue;
            std::string full = LowerCopy(SafeObjectFullName(a));
            std::string cls = LowerCopy(SafeClassName(a));
            if (full.find(needle) == std::string::npos && cls.find(needle) == std::string::npos)
                continue;
            FVector loc{}; float d = 0.0f;
            if (ReadActorLocationFast(a, loc) && havePl) d = DistanceMetres(loc, pl);
            if (d < bestD) { bestD = d; best = a; }
        }
        if (best)
        {
            g_traceTarget = best;
            g_traceTargetCtrl = ReadUObjectAt(best, Offsets::O_Pawn_Controller);
            std::string full = SafeObjectFullName(best);
            { std::lock_guard<std::mutex> lk(g_diagMutex); g_traceTargetName = full; }
            LOG("Trace target set: %s ctrl=%p dist=%.1fm", full.c_str(), (void*)g_traceTargetCtrl.load(), bestD);
        }
        else
        {
            LOG("Trace target '%s': no match in loaded levels", substr.c_str());
        }
    }

    // Live time-series: while live capture is on AND a trace target is set, dump the
    // target's FULL reflection (+ controller + movement) once per cycle on a worker
    // thread, so we capture how its fields change as it walks (idle -> moving delta).
    // Pure raw reads on a detached thread -> never freezes the game.
    struct LiveTargetJob { std::string path; UObject* target; UObject* ctrl; };
    DWORD WINAPI LiveTargetThread(LPVOID param)
    {
        LiveTargetJob* j = reinterpret_cast<LiveTargetJob*>(param);
        try
        {
            std::ostringstream os;
            os << "{\"kind\":\"live_target\",\"t_ms\":" << GetTickCount64() << ",\"actor\":";
            WriteObjectJson(os, j->target);
            FVector loc{};
            if (ReadActorLocationFast(j->target, loc)) { os << ",\"location\":"; WriteVectorJson(os, loc); }
            os << ",\"reflection\":"; Reflect::DumpObjectJson(os, j->target);
            os << ",\"controller\":"; WriteObjectJson(os, j->ctrl);
            if (j->ctrl) { os << ",\"controller_reflection\":"; Reflect::DumpObjectJson(os, j->ctrl); }
            UObject* mv = ReadUObjectAt(j->target, AH::Char_CharacterMovement);
            os << ",\"movement\":"; WriteObjectJson(os, mv);
            if (mv) { os << ",\"movement_reflection\":"; Reflect::DumpObjectJson(os, mv); }
            os << "}\n";
            WriteTextFile(j->path, os.str());
        }
        catch (...) { LOG("Live target dump: exception (ignored)"); }
        delete j;
        g_liveTargetInFlight = false;
        return 0;
    }
    void ScheduleLiveTargetDump(const std::string& dir, int seq)
    {
        UObject* tgt = g_traceTarget.load();
        if (!Mem::IsReadable(tgt, 0x30))
            return;
        if (g_liveTargetInFlight.exchange(true))
            return; // previous one still writing
        std::ostringstream stem;
        stem << "target_" << std::setw(4) << std::setfill('0') << seq << ".json";
        LiveTargetJob* job = new LiveTargetJob{ PathJoin(dir, stem.str()), tgt, g_traceTargetCtrl.load() };
        HANDLE t = CreateThread(nullptr, 0, LiveTargetThread, job, 0, nullptr);
        if (!t) { delete job; g_liveTargetInFlight = false; return; }
        CloseHandle(t);
    }

    bool HookTwinForensicsRelevantObject(void* obj)
    {
        UObject* u = Mem::IsReadable(obj, 0x30) ? static_cast<UObject*>(obj) : nullptr;
        if (!u) return false;
        if (IsHookTwinBodyguard(u)) return true;
        // Controller dispatches are just as important as pawn dispatches.
        UObject* pawn = ReadUObjectAt(u, Offsets::O_BaseController_Pawn);
        if (pawn && IsHookTwinBodyguard(pawn)) return true;
        // Ability/task/subobject dispatches often carry CachedAIOwner.
        UObject* owner = HookTwinOwnerFromDeathAbility(u);
        if (owner && IsHookTwinBodyguard(owner)) return true;
        // Outer-chain fallback for transient ability tasks/actions spawned under the ability.
        UObject* outer = nullptr;
        try { outer = u->Outer(); } catch (...) { outer = nullptr; }
        for (int d = 0; outer && d < 16; ++d)
        {
            if (IsHookTwinBodyguard(outer)) return true;
            UObject* oo = HookTwinOwnerFromDeathAbility(outer);
            if (oo && IsHookTwinBodyguard(oo)) return true;
            try { outer = outer->Outer(); } catch (...) { break; }
        }
        return false;
    }

    bool HookTwinForensicsSuspiciousFunctionName(const std::string& s)
    {
        std::string n = LowerCopy(s);
        static const char* needles[] = {
            "fightstaging", "staging", "death", "dead", "qte", "ability", "montage",
            "anim", "pose", "ragdoll", "target", "blackboard", "aggressive", "passive",
            "movement", "moveto", "path", "behavior", "behaviour", "state", "team", "attitude",
            "loaddeath", "destroyowner", "conditionalaction", "timerrelatedaction"
        };
        for (const char* k : needles)
            if (n.find(k) != std::string::npos) return true;
        return false;
    }

    void HookTwinForensicsProcessEvent(void* obj, void* fn, void* params)
    {
        UObject* fnObj = static_cast<UObject*>(fn);
        std::string fnFull = SafeObjectFullName(fnObj);
        const bool relevantObj = HookTwinForensicsRelevantObject(obj);
        const bool suspiciousFn = HookTwinForensicsSuspiciousFunctionName(fnFull);
        if (!relevantObj && !suspiciousFn)
            return;

        uint64_t n = g_hookTwinForensicsEvents.fetch_add(1, std::memory_order_relaxed) + 1;
        std::ostringstream os;
        os << "{\"seq\":" << n
           << ",\"t_ms\":" << GetTickCount64()
           << ",\"thread\":" << GetCurrentThreadId()
           << ",\"relevant_object\":" << (relevantObj ? "true" : "false")
           << ",\"suspicious_function\":" << (suspiciousFn ? "true" : "false")
           << ",\"object\":"; WriteObjectJson(os, static_cast<UObject*>(obj));
        os << ",\"controller_pawn\":"; WriteObjectJson(os, ReadUObjectAt(static_cast<UObject*>(obj), Offsets::O_BaseController_Pawn));
        os << ",\"ability_owner_guess\":"; WriteObjectJson(os, HookTwinOwnerFromDeathAbility(obj));
        os << ",\"function\":\"" << JsonEscape(fnFull) << "\"";
        os << ",\"ufunction\":"; WriteObjectJson(os, fnObj);
        uint32_t funcFlags = 0;
        if (Mem::IsReadable(reinterpret_cast<uint8_t*>(fn) + Offsets::O_UFunction_FunctionFlags, 4))
            funcFlags = *reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(fn) + Offsets::O_UFunction_FunctionFlags);
        void* execNative = nullptr;
        if (Mem::IsReadable(reinterpret_cast<uint8_t*>(fn) + Offsets::O_UFunction_ExecFunction, sizeof(void*)))
            execNative = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(fn) + Offsets::O_UFunction_ExecFunction);
        os << ",\"func_flags\":\"0x" << std::hex << funcFlags << std::dec << "\"";
        os << ",\"exec_native\":"; WriteCodeAddrJson(os, execNative);
        int paramSize = 0;
        if (Mem::IsReadable(reinterpret_cast<uint8_t*>(fn) + Offsets::O_UStruct_PropertiesSize, 4))
            paramSize = *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(fn) + Offsets::O_UStruct_PropertiesSize);
        if (paramSize < 0) paramSize = 0;
        if (paramSize > 512) paramSize = 512;
        os << ",\"param_size\":" << paramSize << ",\"params_hex\":\"";
        if (params && paramSize > 0 && Mem::IsReadable(params, (size_t)paramSize))
        {
            const uint8_t* b = reinterpret_cast<const uint8_t*>(params);
            char hx[4];
            for (int i = 0; i < paramSize; ++i) { sprintf_s(hx, "%02X", b[i]); os << hx; }
        }
        os << "\"}";

        std::lock_guard<std::mutex> lk(g_diagMutex);
        if (g_hookTwinForensicsFile)
        {
            g_hookTwinForensicsFile << os.str() << "\n";
            g_hookTwinForensicsFile.flush();
        }
    }

    struct HookTwinForensicsWriteJob { std::string path, json, hex; };
    DWORD WINAPI HookTwinForensicsWriteThread(LPVOID param)
    {
        HookTwinForensicsWriteJob* job = reinterpret_cast<HookTwinForensicsWriteJob*>(param);
        try
        {
            WriteTextFile(job->path, job->json);
            if (!job->hex.empty()) WriteTextFile(job->path + "_hex.txt", job->hex);
        }
        catch (...) { LOG("Hook Twin forensics: write exception (ignored)"); }
        delete job;
        g_hookTwinForensicsSnapshotInFlight = false;
        return 0;
    }

    void ScheduleHookTwinForensicsSnapshot(const std::string& dir, int seq, bool full)
    {
        if (dir.empty() || g_hookTwinForensicsSnapshotInFlight.exchange(true))
            return;
        QueueGameThread([dir, seq, full]()
        {
            bool spawned = false;
            try
            {
                UObject* player = GetLocalPawn();
                FVector playerLoc{}; ReadLocalPawnLocationFast(playerLoc, &player);
                std::vector<UObject*> guards = HookBodyguardSnapshot();
                std::ostringstream os, hex;
                os << "{\"schema\":\"AtomicHeartMenu.hook_twin_forensics.v1\""
                   << ",\"seq\":" << seq
                   << ",\"full\":" << (full ? "true" : "false")
                   << ",\"time_local\":\"" << JsonEscape(LocalTimestampIso()) << "\""
                   << ",\"t_ms\":" << GetTickCount64()
                   << ",\"events\":" << g_hookTwinForensicsEvents.load(std::memory_order_relaxed)
                   << ",\"hook_mode\":" << (g_hookBodyguardMode.load(std::memory_order_relaxed) ? "true" : "false")
                   << ",\"player\":"; WriteObjectJson(os, player);
                os << ",\"function_ptrs\":{\"staging_ufn\":"; WriteObjectJson(os, static_cast<UObject*>(g_fnHookTryFightStaging.load(std::memory_order_relaxed)));
                os << ",\"staging_native\":"; WriteCodeAddrJson(os, g_fnHookTryFightStagingNative.load(std::memory_order_relaxed));
                os << ",\"k2_death\":"; WriteObjectJson(os, static_cast<UObject*>(g_fnHookK2OnDeath.load(std::memory_order_relaxed)));
                os << ",\"load_dead_state\":"; WriteObjectJson(os, static_cast<UObject*>(g_fnHookLoadDeadState.load(std::memory_order_relaxed)));
                os << ",\"qte\":"; WriteObjectJson(os, static_cast<UObject*>(g_fnHookStartVersusQTE.load(std::memory_order_relaxed)));
                os << "},\"counters\":{\"death_blocks\":" << g_hookTwinDeathPipelineBlocks.load(std::memory_order_relaxed)
                   << ",\"qte_blocks\":" << g_hookTwinQtePipelineBlocks.load(std::memory_order_relaxed)
                   << ",\"fstaging_selections\":" << g_hookTwinFightStagingSelections.load(std::memory_order_relaxed)
                   << ",\"fstaging_blocks\":" << g_hookTwinFightStagingBlocks.load(std::memory_order_relaxed)
                   << ",\"containers\":" << g_hookTwinFightStagingContainerLogs.load(std::memory_order_relaxed)
                   << "},\"guards\":[";
                for (size_t i = 0; i < guards.size(); ++i)
                {
                    if (i) os << ",";
                    UObject* g = guards[i];
                    os << "{\"ai\":";
                    WriteAiJson(os, hex, g, player, playerLoc, false);
                    os << ",\"fight_staging_selected\":"; WriteObjectJson(os, static_cast<UObject*>(GetFightStagingSelectedObject(g)));
                    if (full && i < 2)
                    {
                        os << ",\"reflection\":"; Reflect::DumpObjectJson(os, g);
                        UObject* ctrl = ReadUObjectAt(g, Offsets::O_Pawn_Controller);
                        os << ",\"controller_reflection\":"; Reflect::DumpObjectJson(os, ctrl);
                    }
                    os << "}";
                    DumpHexWindow(hex, "hook_guard", g, 0x900);
                    DumpHexWindow(hex, "hook_guard_ctrl", ReadUObjectAt(g, Offsets::O_Pawn_Controller), 0x500);
                }
                os << "]}\n";
                std::ostringstream name;
                name << "hook_twin_state_" << std::setw(5) << std::setfill('0') << seq << ".json";
                HookTwinForensicsWriteJob* job = new HookTwinForensicsWriteJob{ PathJoin(dir, name.str()), os.str(), hex.str() };
                HANDLE t = CreateThread(nullptr, 0, HookTwinForensicsWriteThread, job, 0, nullptr);
                if (t) { CloseHandle(t); spawned = true; }
                else delete job;
            }
            catch (...) { LOG("Hook Twin forensics: snapshot exception (ignored)"); }
            if (!spawned) g_hookTwinForensicsSnapshotInFlight = false;
        });
    }

    void StartHookTwinForensics()
    {
        std::string dir = CreateToggleDiagnosticsDir();
        if (dir.empty()) { LOG("Hook Twin forensics: failed to create directory"); return; }
        ResolveHookTwinDeathPipelineFns();
        EnsureHookTwinFightStagingNativeHook();
        EnsureHookTwinActionContainerFactory();
        WriteDiagnosticsManifest(dir, "hook-twin-forensics");
        {
            std::lock_guard<std::mutex> lk(g_diagMutex);
            if (g_hookTwinForensicsFile) g_hookTwinForensicsFile.close();
            g_hookTwinForensicsFile.open(PathJoin(dir, "hook_twin_events.jsonl"), std::ios::out | std::ios::trunc | std::ios::binary);
            g_hookTwinForensicsDir = dir;
            g_diagLastDir = dir;
        }
        g_hookTwinForensicsEvents = 0;
        g_hookTwinForensicsSeq = 0;
        g_hookTwinForensicsLastSnapshotMs = 0;
        g_hookTwinForensicsActive = true;
        LOG("Hook Twin FORENSICS ON. Dumping ProcessEvent/state to: %s", dir.c_str());
        ScheduleHookTwinForensicsSnapshot(dir, 0, true);
    }

    void StopHookTwinForensics()
    {
        std::string dir;
        { std::lock_guard<std::mutex> lk(g_diagMutex); dir = g_hookTwinForensicsDir; }
        if (!dir.empty())
            ScheduleHookTwinForensicsSnapshot(dir, 99999, true);
        g_hookTwinForensicsActive = false;
        {
            std::lock_guard<std::mutex> lk(g_diagMutex);
            if (g_hookTwinForensicsFile)
            {
                g_hookTwinForensicsFile << "{\"event\":\"forensics_stop\",\"t_ms\":" << GetTickCount64() << "}\n";
                g_hookTwinForensicsFile.flush();
                g_hookTwinForensicsFile.close();
            }
            g_hookTwinForensicsDir.clear();
        }
        LOG("Hook Twin FORENSICS OFF. Last directory: %s", dir.c_str());
    }

    void UpdateHookTwinForensics()
    {
        bool want = Features::Get().hookTwinForensics;
        bool was = g_hookTwinForensicsWasOn.load(std::memory_order_relaxed);
        if (want && !was) StartHookTwinForensics();
        else if (!want && was) StopHookTwinForensics();
        g_hookTwinForensicsWasOn = want;
        if (!want || !g_hookTwinForensicsActive.load(std::memory_order_relaxed))
            return;
        ULONGLONG now = GetTickCount64();
        if (now - g_hookTwinForensicsLastSnapshotMs.load(std::memory_order_relaxed) < 500)
            return;
        g_hookTwinForensicsLastSnapshotMs = now;
        std::string dir;
        { std::lock_guard<std::mutex> lk(g_diagMutex); dir = g_hookTwinForensicsDir; }
        int seq = ++g_hookTwinForensicsSeq;
        ScheduleHookTwinForensicsSnapshot(dir, seq, (seq % 10) == 0);
    }

    void UpdateDebugDiagnostics()
    {
        bool want = Features::Get().debugLiveDump;
        if (want && !g_diagToggleWasOn)
            StartDiagnosticsToggle();
        else if (!want && g_diagToggleWasOn)
            StopDiagnosticsToggle();
        g_diagToggleWasOn = want;

        if (!want || g_diagToggleDir.empty())
            return;
        ULONGLONG now = GetTickCount64();
        if (now - g_diagLastSnapshotMs < 1000)
            return;
        g_diagLastSnapshotMs = now;
        std::string dir;
        int seq = 0;
        {
            std::lock_guard<std::mutex> lk(g_diagMutex);
            dir = g_diagToggleDir;
            seq = ++g_diagSnapshotSeq;
        }
        std::ostringstream stem;
        stem << "snapshot_" << std::setw(4) << std::setfill('0') << seq;
        ScheduleDiagnosticSnapshot(dir, stem.str(), false);
        // Plus a worker-thread full reflection of the trace target (if set), so the
        // consecutive logs capture its complete, unrestricted field state over time.
        ScheduleLiveTargetDump(dir, seq);
    }

    bool CallBarrelSetAmmo(UObject* barrel, void* bulletClass, int32_t count)
    {
        UFunction* fn = CachedFn(AH::Fn_EBBarrel_SetAmmo);
        if (!barrel || !bulletClass || !fn)
            return false;

        void* bullets[1] = { bulletClass };
        P_BarrelSetAmmo p{};
        p.Count = count;
        p.UnloadChambered = false;
        p.CancelShooting = false;
        p.ManualCharge = false;
        p.NewAmmo.Data = bullets;
        p.NewAmmo.Count = 1;
        p.NewAmmo.Max = 1;
        barrel->ProcessEvent(fn, &p);
        return true;
    }

    bool TopUpCurrentWeaponAmmo(UObject* weapon, bool forceSetAmmo, bool captureRestore, void** outBarrel, bool* outSetAmmo)
    {
        if (outBarrel) *outBarrel = nullptr;
        if (outSetAmmo) *outSetAmmo = false;
        if (!weapon)
            return false;

        void* barrel = ReadPtrField(weapon, AH::RangeWeapon_Barrel);
        if (outBarrel) *outBarrel = barrel;
        if (captureRestore)
            CaptureWeaponAmmoBackup(weapon, barrel);

        bool touched = false;
        touched |= WriteInt32Field(weapon, AH::Weapon_AmmoSize, 9999);
        touched |= WriteInt32Field(weapon, AH::Weapon_StartAmmoCount, 9999);

        if (!barrel)
            return touched;

        void* bulletClass = ResolveBulletClass(weapon, barrel);
        bool setAmmo = false;
        if (bulletClass && forceSetAmmo)
            setAmmo = CallBarrelSetAmmo(static_cast<UObject*>(barrel), bulletClass, 9999);

        touched |= setAmmo;
        touched |= WriteBoolField(barrel, AH::EBBarrel_ShootingBlocked, false);
        touched |= WriteBoolField(barrel, AH::EBBarrel_CycleAmmoUnlimited, true);
        touched |= WriteInt32Field(barrel, AH::EBBarrel_CycleAmmoCount, 9999);
        touched |= WriteInt32Field(barrel, AH::EBBarrel_CycleAmmoPos, 0);
        touched |= WriteBoolField(barrel, AH::EBBarrel_LoadNext, true);

        uint8_t* barrelBytes = reinterpret_cast<uint8_t*>(barrel);
        if (bulletClass && Mem::IsReadable(barrelBytes + AH::EBBarrel_ChamberedBullet, sizeof(void*)))
        {
            void** chambered = reinterpret_cast<void**>(barrelBytes + AH::EBBarrel_ChamberedBullet);
            if (!Mem::LooksLikePtr(*chambered))
            {
                *chambered = bulletClass;
                touched = true;
            }
        }

        if (outSetAmmo) *outSetAmmo = setAmmo;
        return touched;
    }

    bool AddAmmoAssetToInventory(UObject* inventory, const AmmoEntry& ammo, int ammoIndex, int32_t targetCount, bool force, int& resolvedAssets, int& addCalls)
    {
        UObject* asset = CachedObject(ammo.objectName);
        if (!inventory || !asset)
            return false;

        ++resolvedAssets;
        int current = QueryInventoryItemCount(inventory, asset);
        if (current >= targetCount)
            return true;

        static int assumedCounts[sizeof(kAmmo) / sizeof(kAmmo[0])] = {};
        static ULONGLONG lastAddMs[sizeof(kAmmo) / sizeof(kAmmo[0])] = {};
        ULONGLONG nowMs = GetTickCount64();
        if (!force && ammoIndex >= 0 && ammoIndex < (int)(sizeof(kAmmo) / sizeof(kAmmo[0])))
        {
            if (assumedCounts[ammoIndex] >= targetCount)
                return true;
            if (lastAddMs[ammoIndex] && nowMs - lastAddMs[ammoIndex] < 30000)
                return true;
        }

        // If the count function is unavailable, only add on forced refills.
        if (current < 0 && !force)
            return true;

        UFunction* add = CachedFn(AH::Fn_AHInventory_AddItemsToInventory);
        if (!add)
            return false;

        int32_t amount = current >= 0 ? (targetCount - current) : targetCount;
        if (amount <= 0)
            return true;

        P_InventoryItemCount p{};
        p.ItemDataAsset = asset;
        p.InCount = amount;
        inventory->ProcessEvent(add, &p);
        ++addCalls;
        if (ammoIndex >= 0 && ammoIndex < (int)(sizeof(kAmmo) / sizeof(kAmmo[0])))
        {
            assumedCounts[ammoIndex] = targetCount;
            lastAddMs[ammoIndex] = nowMs;
        }
        LOG("Ammo refill: added %d %s ammo (had=%d target=%d)", amount, ammo.label, current, targetCount);
        return true;
    }

    UObject* RefillAmmoInventory(UObject* pawn, bool force, bool captureRestore, int& resolvedAssets, int& addCalls)
    {
        resolvedAssets = 0;
        addCalls = 0;

        UObject* inventory = GetInventoryPlayer(pawn);
        if (!inventory)
            return nullptr;

        if (captureRestore)
            CaptureInventoryBackup(inventory);

        bool ignoreWeightChanged = ApplyInventoryIgnoreOverWeight(inventory, true);

        WriteInt32Field(inventory, AH::Inventory_AmmoCount, 9999);
        WriteInt32Field(inventory, AH::Inventory_InfiniteAmmoCount, 9999);

        constexpr int32_t kAmmoTarget = 500;
        for (int i = 0; i < (int)(sizeof(kAmmo) / sizeof(kAmmo[0])); ++i)
            AddAmmoAssetToInventory(inventory, kAmmo[i], i, kAmmoTarget, force, resolvedAssets, addCalls);

        if (ignoreWeightChanged && !captureRestore && !Features::Get().infiniteAmmo)
            ApplyInventoryIgnoreOverWeight(inventory, false);

        return inventory;
    }

    void ApplyInfiniteAmmo(UObject* pawn, UObject* weapon, bool force, bool captureRestore, const char* reason)
    {
        if (!pawn)
            return;

        static ULONGLONG lastInventoryMs = 0;
        static ULONGLONG lastWeaponMs = 0;
        static ULONGLONG lastLogMs = 0;

        ULONGLONG nowMs = GetTickCount64();
        bool measure = force || nowMs - lastLogMs > 15000;
        int reserveBefore = measure ? QueryWeaponReserveAmmo(pawn, weapon) : -1;
        int loadedBefore = measure ? QueryWeaponLoadedAmmo(weapon) : -1;

        bool refillInventory = force;
        UObject* inventory = nullptr;
        int resolvedAssets = 0;
        int addCalls = 0;
        if (refillInventory)
        {
            inventory = RefillAmmoInventory(pawn, force, captureRestore, resolvedAssets, addCalls);
            lastInventoryMs = nowMs;
        }

        bool topUpWeapon = force || nowMs - lastWeaponMs > 500;
        void* barrel = nullptr;
        bool setAmmo = false;
        bool weaponTouched = false;
        if (weapon && topUpWeapon)
        {
            weaponTouched = TopUpCurrentWeaponAmmo(weapon, force, captureRestore, &barrel, &setAmmo);
            lastWeaponMs = nowMs;
        }

        int reserveAfter = measure ? QueryWeaponReserveAmmo(pawn, weapon) : -1;
        int loadedAfter = measure ? QueryWeaponLoadedAmmo(weapon) : -1;
        bool shouldLog = force || addCalls > 0 || nowMs - lastLogMs > 15000;
        if (shouldLog)
        {
            LOG("InfiniteAmmo%s: pawn=%p weapon=%p inv=%p barrel=%p reserve=%d->%d loaded=%d->%d ammoAssets=%d addCalls=%d weaponTouched=%s setAmmo=%s",
                reason ? reason : "",
                (void*)pawn,
                (void*)weapon,
                (void*)inventory,
                barrel,
                reserveBefore,
                reserveAfter,
                loadedBefore,
                loadedAfter,
                resolvedAssets,
                addCalls,
                weaponTouched ? "yes" : "no",
                setAmmo ? "yes" : "no");
            lastLogMs = nowMs;
        }
    }

    bool GiveWeaponInternal(int index, bool equip)
    {
        if (index < 0 || index >= Features::WeaponCount())
        {
            LOG("GiveWeapon failed: invalid index=%d", index);
            return false;
        }

        UObject* pawn = GetLocalPawn();
        if (!pawn)
        {
            LOG("GiveWeapon failed: no local pawn");
            return false;
        }

        const auto& weapon = kWeapons[index];
        UObject* asset = CachedObject(weapon.objectName);
        if (!asset)
        {
            LOG("GiveWeapon failed: %s asset not loaded (%s)", weapon.label, weapon.objectName);
            return false;
        }

        ULONGLONG startMs = GetTickCount64();
        bool takeCalled = InvokeTakeWeapon(pawn, asset);
        bool equipCalled = equip ? EquipWeapon(pawn, asset) : false;
        bool ok = takeCalled || equipCalled;
        ULONGLONG elapsedMs = GetTickCount64() - startMs;

        LOG("GiveWeapon %s: asset=%p takeCalled=%s equip=%s/%s ok=%s time=%llums",
            weapon.label, (void*)asset,
            takeCalled ? "yes" : "no",
            equip ? "requested" : "no",
            equipCalled ? "called" : "not-called",
            ok ? "yes" : "no",
            elapsedMs);

        return ok;
    }

    void ProcessGiveAllQueue()
    {
        if (!g_giveAll.active)
            return;

        ULONGLONG nowMs = GetTickCount64();
        if (g_giveAll.lastStepMs && nowMs - g_giveAll.lastStepMs < 75)
            return;

        int count = Features::WeaponCount();
        if (g_giveAll.index >= count)
        {
            LOG("GiveAllWeapons complete: ok=%d/%d", g_giveAll.ok, count);
            g_giveAll = {};
            return;
        }

        int i = g_giveAll.index++;
        bool equip = g_giveAll.equipLast && i == count - 1;
        if (GiveWeaponInternal(i, equip))
            ++g_giveAll.ok;
        g_giveAll.lastStepMs = GetTickCount64();
    }

    bool HasPawnFeatureWork(const Features::State& st)
    {
        return st.godMode ||
            st.showCoords ||
            st.flyHack ||
            st.noclip ||
            st.customFov ||
            st.bulletTime ||
            st.customScale ||
            st.speedHack ||
            st.superJump ||
            st.lowGravity ||
            st.infiniteStamina ||
            st.infiniteEnergy ||
            st.infiniteAir ||
            st.oneHitKill ||
            st.infiniteAmmo ||
            g_giveAll.active ||
            g_incomingDamageBackup.valid ||
            g_healthBackup.valid ||
            g_instigatedDamageBackup.valid ||
            g_staminaBackup.valid ||
            g_energyBackup.valid ||
            g_airBackup.valid ||
            g_movementBackup.mv != nullptr ||
            g_inventoryBackup.inventory != nullptr ||
            g_weaponAmmoBackup.weapon != nullptr ||
            g_fovBackup.active;
    }
}

Features::State& Features::Get() { return g_state; }
UE::FVector Features::LastLocation() { return g_lastLoc; }
const Features::WeaponEntry* Features::WeaponList() { return kWeapons; }
int Features::WeaponCount() { return (int)(sizeof(kWeapons) / sizeof(kWeapons[0])); }

int Features::AiCachedCount() { return g_aiCachedCount.load(); }
int Features::AiPendingCount() { return g_aiPendingCount.load(); }

// The spawn dropdown is driven by the LIVE discovered enemy classes (deduped),
// so every entry is a loaded, configured, safe-to-spawn class. Refreshed ~1/sec.
const char* Features::AiSpawnModelName(int index)
{
    RefreshLiveModels();
    if (index < 0 || index >= (int)g_liveModels.size())
        return "(stand near enemies)";
    return g_liveModels[index].name.c_str();
}

int Features::AiSpawnModelCount()
{
    RefreshLiveModels();
    return (int)g_liveModels.size();
}

int Features::AiBossPresetCount()
{
    return BossPresetCount();
}

const char* Features::AiBossPresetName(int index)
{
    if (index < 0 || index >= BossPresetCount())
        return "(no boss preset)";
    return kBossSpawnPresets[index].label;
}

bool Features::AiSpawnBossPreset(int index)
{
    return QueueBossPresetSpawn(index, false);
}

bool Features::HookAiSpawnBossPreset(int index)
{
    SetHookBodyguardMode(true);
    if (!HookBodyguardModeActive())
        return false;
    return QueueBossPresetSpawn(index, true);
}

// Spawn the dropdown-selected live model as a streamed bodyguard (one at a time).
bool Features::AiSpawnModel(int index)
{
    if (!G::sdkReady.load()) { LOG("AiSpawnModel: SDK not ready"); return false; }
    RefreshLiveModels();
    if (index < 0 || index >= (int)g_liveModels.size())
    {
        LOG("AiSpawnModel: no live model at %d -- stand near the enemy type you want", index);
        return false;
    }
    UClass* cls = g_liveModels[index].cls;
    std::string label = g_liveModels[index].name;
    if (!Mem::IsReadable(cls, 0x30)) { LOG("AiSpawnModel: class no longer loaded"); return false; }
    if (!InstallProcessEventHook())
    {
        LOG("AiSpawnModel: ProcessEvent hook unavailable (try again once in-game)");
        return false;
    }
    SpawnRequest req; req.cls = cls; req.label = label;
    EnqueueSpawn(std::move(req));
    LOG("AiSpawnModel: queued streamed spawn of '%s'", label.c_str());
    return true;
}

int Features::AiSpawnQueueCount() { return g_spawnQueueCount.load(); }

// ---- Global model search (spawn ANY loaded character type) -------------------
// Returns the filtered list of model names. Calling this also keeps the worker
// sweep alive (g_allModelsWanted), so the list stays fresh while the panel is open.
std::vector<std::string> Features::AiSearchModels(const char* query, int maxResults)
{
    std::vector<std::string> out;
    if (!G::sdkReady.load()) return out;
    KickBossModelDiscovery();
    std::string q = query ? query : "";
    for (char& c : q) c = (char)std::tolower((unsigned char)c);

    std::vector<LiveModel> snap;
    { std::lock_guard<std::mutex> lk(g_allModelsMutex); snap = g_allModels; }
    for (const LiveModel& m : snap)
    {
        if ((int)out.size() >= maxResults) break;
        if (!q.empty())
        {
            std::string nl = m.name + " " + m.path;
            for (char& c : nl) c = (char)std::tolower((unsigned char)c);
            if (nl.find(q) == std::string::npos) continue;
        }
        out.push_back(m.name);
    }
    return out;
}

// Spawn a model by its (pretty) name from the global list -> streamed bodyguard.
bool Features::AiSpawnModelByName(const char* prettyName)
{
    if (!G::sdkReady.load() || !prettyName || !*prettyName) return false;
    UClass* cls = nullptr;
    std::string label, path;
    {
        std::lock_guard<std::mutex> lk(g_allModelsMutex);
        for (const LiveModel& m : g_allModels)
            if (m.name == prettyName) { cls = m.cls; label = m.name; path = m.path; break; }
    }
    // A loaded class spawns directly; an Asset-Registry entry (cls null) carries a class
    // path that LoadClassByPath resolves on the game thread at spawn time (loads the
    // boss/DLC enemy on demand). One of the two must be present.
    if (!Mem::IsReadable(cls, 0x30) && path.empty())
    {
        LOG("AiSpawnModelByName: '%s' not resolvable (list may be rebuilding)", prettyName);
        return false;
    }
    if (!InstallProcessEventHook())
    {
        LOG("AiSpawnModelByName: ProcessEvent hook unavailable (try again once in-game)");
        return false;
    }
    SpawnRequest req;
    if (Mem::IsReadable(cls, 0x30)) req.cls = cls; else req.path = path;
    req.label = label;
    EnqueueSpawn(std::move(req));
    LOG("AiSpawnModelByName: queued streamed spawn of '%s' (%s)", label.c_str(),
        req.cls ? "loaded class" : req.path.c_str());
    return true;
}

// Deep kill: GObjects sweep for every live enemy, even ones the level-walk cache
// misses (the "uncatchable, unkillable, keeps attacking" edge case).
int Features::AiKillAllDeep()
{
    if (!G::sdkReady.load()) return 0;
    InstallProcessEventHook();
    g_deepKillRequested = true; // serviced on the worker thread (RunDeepKillWorker)
    LOG("AiKillAllDeep: deep enemy sweep requested");
    return 0;
}

// Delete an actor from the world outright (K2_DestroyActor). Validated against the
// cache/squad so we never DestroyActor a stale pointer. Runs on the game thread.
void Features::AiDeleteActor(unsigned long long id)
{
    if (!G::sdkReady.load()) return;
    UObject* ai = reinterpret_cast<UObject*>((uintptr_t)id);

    // Validate: must be a currently-cached AI or a squad member (never a raw guess).
    bool known = IsSquadMember(ai);
    if (!known)
    {
        std::vector<AiCachedActor> snap = CopyAiSnapshot();
        for (const AiCachedActor& e : snap) if (e.actor == ai) { known = true; break; }
    }
    if (!known || !Mem::IsReadable(ai, 0x30))
    {
        LOG("AiDeleteActor: id %llu not a live cached actor (ignored)", id);
        return;
    }

    g_selectedAi.erase(std::remove(g_selectedAi.begin(), g_selectedAi.end(), ai), g_selectedAi.end());

    InstallProcessEventHook();
    QueueGameThread([ai]()
    {
        try
        {
            if (!Mem::IsReadable(ai, 0x30)) return;
            // Stop every Hook/squad state machine from touching it before it dies.
            ReleaseHookNativeMovement(ai, true);
            SquadRemove(ai);
            ForgetHookBodyguardRuntimeState(ai, true);

            UFunction* fn = CachedFn(AH::Fn_K2DestroyActor);
            if (!fn) { LOG("AiDeleteActor: K2_DestroyActor unresolved"); return; }
            uint8_t noParams = 0;
            ai->ProcessEvent(fn, &noParams);
            LOG("AiDeleteActor: destroyed actor %p", (void*)ai);
            RequestAiDiscovery(); // refresh the roster so the row drops out
        }
        catch (...) { LOG("AiDeleteActor: exception during destroy (ignored)"); }
    });
}

int Features::AiQueueKillNearby()
{
    if (!G::sdkReady.load()) return 0;
    try { return QueueAiOperation(AiQueuedKind::Kill, CollectNearbyAi(g_state.aiRadius, kMaxAiCommandTargets)); }
    catch (...) { LOG("AiQueueKillNearby: exception (ignored)"); return 0; }
}

int Features::AiQueuePassiveNearby(bool passive)
{
    if (!G::sdkReady.load()) return 0;
    try
    {
        return QueueAiOperation(passive ? AiQueuedKind::PassiveOn : AiQueuedKind::PassiveOff,
            CollectNearbyAi(g_state.aiRadius, kMaxAiCommandTargets));
    }
    catch (...) { LOG("AiQueuePassiveNearby: exception (ignored)"); return 0; }
}

int Features::AiQueueFollowPlayer()
{
    if (!G::sdkReady.load()) return 0;
    try { return QueueAiOperation(AiQueuedKind::Follow, CollectNearbyAi(g_state.aiRadius, kMaxAiCommandTargets)); }
    catch (...) { LOG("AiQueueFollowPlayer: exception (ignored)"); return 0; }
}

int Features::AiQueueFightEachOther()
{
    if (!G::sdkReady.load()) return 0;
    try { return QueueAiOperation(AiQueuedKind::FightEachOther, CollectNearbyNonSquadAi(g_state.aiRadius, kMaxAiCommandTargets, true)); }
    catch (...) { LOG("AiQueueFightEachOther: exception (ignored)"); return 0; }
}

int Features::AiQueueReleaseNearby()
{
    if (!G::sdkReady.load()) return 0;
    try { return QueueAiOperation(AiQueuedKind::Release, CollectNearbyAi(g_state.aiRadius, kMaxAiCommandTargets)); }
    catch (...) { LOG("AiQueueReleaseNearby: exception (ignored)"); return 0; }
}

int Features::AiQueueKillAll()
{
    if (!G::sdkReady.load()) return 0;
    try
    {
        InstallProcessEventHook();
        g_deepKillRequested = true; // ordinary KILL ALL must include scanner-missed streamed enemies
        RequestAiDiscovery();
        int queued = QueueAiOperation(AiQueuedKind::Kill, CollectAllCachedAi(kMaxCachedAiActors));
        LOG("AiQueueKillAll: queued cached=%d and requested authoritative deep sweep", queued);
        return queued;
    }
    catch (...) { LOG("AiQueueKillAll: exception (ignored)"); return 0; }
}

int Features::AiQueueLaunchAll()
{
    if (!G::sdkReady.load()) return 0;
    try { return QueueAiOperation(AiQueuedKind::Launch, CollectAllCachedAi(kMaxCachedAiActors)); }
    catch (...) { LOG("AiQueueLaunchAll: exception (ignored)"); return 0; }
}

void Features::MaxWeaponUpgrades()
{
    if (!G::sdkReady.load()) { LOG("MaxWeaponUpgrades failed: SDK not ready"); return; }
    try
    {
        UObject* pawn = GetLocalPawn();
        UObject* weapon = pawn ? GetCurrentWeaponObject(pawn) : nullptr;
        UFunction* fn = CachedFn(AH::Fn_BaseWeapon_FullUpgrade);
        if (!weapon || !fn)
        {
            LOG("MaxWeaponUpgrades failed: weapon=%p fn=%p", (void*)weapon, (void*)fn);
            return;
        }
        uint8_t noParams = 0;
        weapon->ProcessEvent(fn, &noParams);
        LOG("MaxWeaponUpgrades: FullUpgrade applied to current weapon %p", (void*)weapon);
    }
    catch (...) { LOG("MaxWeaponUpgrades: exception (ignored)"); }
}

void Features::RunConsoleCommand(const char* command)
{
    if (!G::sdkReady.load() || !command || !*command) return;
    try
    {
        std::string cmd(command);
        // Ensure the game thread can be borrowed, then run the command there
        // (cvars / viewmodes are game-thread state, never touch them from Present).
        InstallProcessEventHook();
        QueueGameThread([cmd]() { RunConsoleCommandImpl(cmd); });
        LOG("Console queued: %s", cmd.c_str());
    }
    catch (...) { LOG("RunConsoleCommand: exception (ignored)"); }
}

void Features::NoteGameThread()
{
    // The WndProc thread IS the game thread in UE4 (window messages are pumped from
    // the game-thread main loop). Pinning it here means the ProcessEvent pump only
    // ever drains spawns / AI / visual work on the real game thread -- never on an
    // async loading or audio worker that also dispatches top-level UFunctions while
    // a quest NPC streams in (that race ran SpawnAIFromClass off-thread -> the
    // "AssembleReferenceTokenStream ... AnimInstance ... non-game thread" fatal).
    unsigned long tid = GetCurrentThreadId();
    if (tid != 0 && tid != g_renderThreadId.load())
        g_gameThreadId.store(tid);
}

bool Features::AiSpawnBodyguard()
{
    if (!G::sdkReady.load())
    {
        LOG("AiSpawnBodyguard failed: SDK not ready");
        return false;
    }

    // SpawnAIFromClass runs the pawn's full actor construction (SpawnActor,
    // component registration, construction script). That is GAME-THREAD work --
    // doing it from our render-thread Present hook races the engine's own actor
    // iteration and crashes. So we install the ProcessEvent hook and STREAM the
    // spawn through the queue: the AI pump constructs it on the game thread, one
    // request per window, so nothing ever stalls a frame.
    if (!InstallProcessEventHook())
    {
        LOG("AiSpawnBodyguard failed: ProcessEvent hook unavailable (try again once in-game)");
        return false;
    }

    // Clone the NEAREST live, healthy enemy's exact runtime class (resolved on the
    // game thread at construction time). That's the working, configured class --
    // never a bare native template (the original crash). No live enemy => the drain
    // logs and skips.
    SpawnRequest req; req.cloneNearest = true; req.label = "nearest enemy";
    EnqueueSpawn(std::move(req));
    LOG("AiSpawnBodyguard: queued streamed clone of the nearest live enemy");
    return true;
}

// Capture the nearest live enemy's runtime class into the saved-character DB so
// it can be re-spawned later (even after a restart, as long as that character
// type is loaded). Reads game memory (class name) -- safe from the UI thread,
// same as ESP reads.
bool Features::AiSaveNearestCharacter(const char* displayName)
{
    if (!G::sdkReady.load())
    {
        LOG("AiSaveNearestCharacter: SDK not ready");
        return false;
    }
    UClass* cls = NearestLiveEnemyClass();
    if (!Mem::IsReadable(cls, 0x30))
    {
        LOG("AiSaveNearestCharacter: no live enemy nearby to copy -- stand next to one");
        return false;
    }
    std::string full = cls->GetFullName(); // "<TypePrefix> /Game/.../BP_X.BP_X_C"
    std::string path = full;
    size_t sp = full.find(' ');
    if (sp != std::string::npos)
        path = full.substr(sp + 1); // drop the type prefix -> the resolvable path
    if (path.empty())
    {
        LOG("AiSaveNearestCharacter: empty class path");
        return false;
    }
    std::string shortName = cls->GetName();
    std::string name = (displayName && *displayName) ? std::string(displayName) : shortName;

    std::lock_guard<std::mutex> lk(g_savedCharsMutex);
    LoadSavedCharacters();
    for (SavedCharacter& s : g_savedChars)
    {
        if (s.classPath == path) // already saved -> just refresh the label + class ptr
        {
            s.name = name;
            s.cachedClass = cls;
            WriteSavedCharacters();
            LOG("AiSaveNearestCharacter: updated '%s'", name.c_str());
            return true;
        }
    }
    g_savedChars.push_back({ name, path, cls }); // cache the live class ptr for instant same-session respawn
    WriteSavedCharacters();
    LOG("AiSaveNearestCharacter: saved '%s' (%s)", name.c_str(), path.c_str());
    return true;
}

// Save every SELECTED unit's class to the model DB (the growing model library you
// can spawn anywhere, load-on-demand). Dedupes against what's already saved.
void Features::AiSaveSelected()
{
    if (!G::sdkReady.load()) { LOG("AiSaveSelected: SDK not ready"); return; }
    std::vector<UObject*> sel = g_selectedAi; // render-thread selection snapshot
    if (sel.empty()) { LOG("AiSaveSelected: nothing selected"); return; }

    std::lock_guard<std::mutex> lk(g_savedCharsMutex);
    LoadSavedCharacters();
    int added = 0;
    for (UObject* ai : sel)
    {
        if (!AiUsable(ai)) continue;
        UClass* cls = ai->Class();
        if (!Mem::IsReadable(cls, 0x30)) continue;
        std::string full = cls->GetFullName();
        size_t sp = full.find(' ');
        std::string path = (sp != std::string::npos) ? full.substr(sp + 1) : full;
        if (path.empty()) continue;
        std::string name = cls->GetName();
        bool dup = false;
        for (SavedCharacter& s : g_savedChars)
            if (s.classPath == path) { s.cachedClass = cls; dup = true; break; }
        if (dup) continue;
        g_savedChars.push_back({ name, path, cls });
        ++added;
    }
    if (added > 0) WriteSavedCharacters();
    LOG("AiSaveSelected: added %d new model(s) to the DB", added);
}

// Spawn a previously-saved character by DB index. Re-resolves its class by name
// on the game thread, then spawns it as a permanent bodyguard.
bool Features::AiSpawnSavedCharacter(int index)
{
    if (!G::sdkReady.load())
    {
        LOG("AiSpawnSavedCharacter: SDK not ready");
        return false;
    }
    std::string path, name;
    UClass* cached = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_savedCharsMutex);
        LoadSavedCharacters();
        if (index < 0 || index >= (int)g_savedChars.size())
            return false;
        path   = g_savedChars[index].classPath;
        name   = g_savedChars[index].name;
        cached = g_savedChars[index].cachedClass;
    }
    if (!InstallProcessEventHook())
    {
        LOG("AiSpawnSavedCharacter: ProcessEvent hook unavailable (try again once in-game)");
        return false;
    }
    // Stream it through the spawn queue. Prefer the class ptr cached at save time
    // (instant, same session). Otherwise the path is loaded ON DEMAND on the game
    // thread (LoadClassByPath) -- so you NO LONGER need to be near the NPC to spawn it.
    SpawnRequest req; req.path = path; req.label = name;
    if (Mem::IsReadable(cached, 0x30)) req.cls = cached;
    EnqueueSpawn(std::move(req));
    LOG("AiSpawnSavedCharacter: queued streamed spawn of '%s'", name.c_str());
    return true;
}

int Features::AiSavedCharacterCount()
{
    std::lock_guard<std::mutex> lk(g_savedCharsMutex);
    LoadSavedCharacters();
    return (int)g_savedChars.size();
}

std::vector<std::string> Features::AiSavedCharacterNames()
{
    std::lock_guard<std::mutex> lk(g_savedCharsMutex);
    LoadSavedCharacters();
    std::vector<std::string> out;
    out.reserve(g_savedChars.size());
    for (const SavedCharacter& s : g_savedChars)
        out.push_back(s.name);
    return out;
}

bool Features::AiDeleteSavedCharacter(int index)
{
    std::lock_guard<std::mutex> lk(g_savedCharsMutex);
    LoadSavedCharacters();
    if (index < 0 || index >= (int)g_savedChars.size())
        return false;
    LOG("AiDeleteSavedCharacter: removed '%s'", g_savedChars[index].name.c_str());
    g_savedChars.erase(g_savedChars.begin() + index);
    WriteSavedCharacters();
    return true;
}

// Stand every spawned ally down and forget them (game thread: touches AI).
void Features::AiClearSpawnedAllies()
{
    InstallProcessEventHook();
    QueueGameThread([]()
    {
        try
        {
            std::vector<UObject*> squad;
            std::vector<UObject*> hookGuards;
            { std::lock_guard<std::mutex> lk(g_squadMutex); squad.swap(g_spawnedAllies); g_spawnedAllyCount = 0; }
            { std::lock_guard<std::mutex> lk(g_hookBodyguardMutex); hookGuards.swap(g_hookBodyguards); }
            int n = 0;
            for (UObject* a : squad)
            {
                bool hookOwned = std::find(hookGuards.begin(), hookGuards.end(), a) != hookGuards.end();
                if (hookOwned) ReleaseHookNativeMovement(a, true);
                bool released = AiUsable(a) && ApplyAiRelease(a); // back to normal killable enemies
                if (hookOwned) ForgetHookBodyguardRuntimeState(a, !released);
                if (released) ++n;
            }
            LOG("AiClearSpawnedAllies: stood down %d squad member(s)", n);
        }
        catch (...) { LOG("AiClearSpawnedAllies: exception (ignored)"); }
    });
}

int Features::AiSpawnedAllyCount()
{
    return g_spawnedAllyCount.load();
}

// =======================================================================
//  HORDE ROUNDS  --  public API (the Horde Rounds tab calls these)
// -----------------------------------------------------------------------
//  Start/Stop marshal onto the game thread (they teleport / spawn / touch the
//  no-save guard); the count getters are plain atomic reads for the menu. The
//  location DB calls run on the calling (render) thread under their own mutex,
//  exactly like the saved-character DB.
// =======================================================================
void Features::HordeStart()
{
    if (!G::sdkReady.load()) { LOG("HordeStart: SDK not ready"); return; }
    if (!InstallProcessEventHook())
    {
        LOG("HordeStart: ProcessEvent hook unavailable (try again once in-game)");
        return;
    }

    // Resolve the selected location on THIS thread (don't touch the DB from the
    // game thread). loc 0 = "Here" (no teleport); >0 = a saved arena.
    bool teleport = false;
    FVector dest{};
    int loc = g_state.hordeLocation;
    if (loc > 0)
    {
        std::lock_guard<std::mutex> lk(g_arenasMutex);
        LoadArenas();
        int idx = loc - 1;
        if (idx >= 0 && idx < (int)g_arenas.size())
        { dest = g_arenas[idx].loc; teleport = true; }
    }

    bool advanceWave = g_hordeActive.load(); // already running -> just push the next wave

    QueueGameThread([teleport, dest, advanceWave]()
    {
        try
        {
            ResolveSaveBlockFns();

            if (advanceWave)
            {
                HordeStartWave(g_hordeRound.load() + 1);
                return;
            }

            // Fresh run: capture the return point, optionally teleport into the arena,
            // arm the no-save guard, and begin wave 1.
            UObject* player = nullptr; FVector ploc{};
            ReadLocalPawnLocationFast(ploc, &player);
            g_hordeReturnLoc = ploc;
            FRotator rr{}; if (GetControlRotation(rr)) g_hordeReturnRot = rr;
            g_hordeHaveReturn = Mem::IsReadable(player, 0x30);
            g_hordeTeleported = false;
            g_hordeKills = 0;

            if (teleport && Mem::IsReadable(player, 0x30))
            {
                if (SetActorLocation(player, dest, true))
                {
                    g_hordeTeleported = true;
                    RefreshFlyStreaming(player, true);
                    LOG("Horde: teleported to arena %.0f %.0f %.0f", dest.X, dest.Y, dest.Z);
                }
            }

            g_blockSaves = true;          // arm the no-save guard (hkProcessEvent swallows saves)
            g_hordeActive = true;
            g_state.hordeActive = true;
            HordeStartWave(1);
            LOG("Horde: run STARTED (saves blocked)");
        }
        catch (...) { LOG("HordeStart: exception (ignored)"); }
    });
}

void Features::HordeStop()
{
    InstallProcessEventHook();
    QueueGameThread([]()
    {
        // Always safe to call -- restores position, kills robots, re-enables saving.
        try { HordeEndRun(true /*restorePos*/, true /*destroyEnemies*/, "user stop"); }
        catch (...) { LOG("HordeStop: exception (ignored)"); }
    });
}

bool Features::HordeIsActive()    { return g_hordeActive.load(); }
int  Features::HordeRound()       { return g_hordeRound.load(); }
int  Features::HordeAliveCount()  { return g_hordeAlive.load(); }
int  Features::HordeKillCount()   { return g_hordeKills.load(); }
int  Features::HordePendingCount(){ return g_hordePending.load(); }

// ---- AI ownership lock (native ProcessEvent-level "they're ours, permanently") ----
void Features::SetOwnershipLock(bool on)
{
    if (on)
    {
        // Resolve (and cache) the three un-ally UFunctions we compare against, and make
        // sure the ProcessEvent detour is live so the swallow can actually fire.
        if (!g_fnOwnSwitchTeamAttitude.load())
            g_fnOwnSwitchTeamAttitude.store(CachedClassFn(AH::Cls_AICharacter, "SwitchTeamToMatchCharacterAttitude"));
        if (!g_fnOwnSetTargetEnemy.load())
            g_fnOwnSetTargetEnemy.store(CachedClassFn(AH::Cls_AICharacter, "SetTargetEnemy"));
        if (!g_fnOwnSetBbTargetEnemy.load())
            g_fnOwnSetBbTargetEnemy.store(CachedClassFn(AH::Cls_AHAIController, "SetBlackboardTargetEnemy"));
        InstallProcessEventHook();
        LOG("Ownership lock: ON (resolved switch=%p targetEnemy=%p bbTargetEnemy=%p)",
            g_fnOwnSwitchTeamAttitude.load(), g_fnOwnSetTargetEnemy.load(), g_fnOwnSetBbTargetEnemy.load());
    }
    else
    {
        LOG("Ownership lock: OFF");
    }
    g_ownershipLock.store(on, std::memory_order_relaxed);
}
bool     Features::OwnershipLockActive()  { return g_ownershipLock.load(std::memory_order_relaxed); }
uint64_t Features::OwnershipSwallowCount(){ return g_ownershipSwallows.load(std::memory_order_relaxed); }
bool     Features::OwnershipResolved()
{
    return g_fnOwnSwitchTeamAttitude.load() && g_fnOwnSetTargetEnemy.load() && g_fnOwnSetBbTargetEnemy.load();
}

void Features::SetHookTwinForensics(bool on)
{
    Get().hookTwinForensics = on;
    LOG("Hook Twin forensics requested: %s", on ? "ON" : "OFF");
}

bool Features::HookTwinForensicsActive()
{
    return g_hookTwinForensicsActive.load(std::memory_order_relaxed);
}

void Features::SetHookBodyguardMode(bool on)
{
    if (on && g_hookBodyguardMode.load(std::memory_order_relaxed) && HookFriendshipResolved())
    {
        ResolveHookTwinDeathPipelineFns();
        return;
    }
    if (!on)
    {
        g_hookBodyguardMode.store(false, std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> lock(g_hookControllerOwnedMutex);
            g_hookControllerOwned.clear();
        }
        std::vector<UObject*> guards = HookBodyguardSnapshot();
        QueueGameThread([guards]() { for (UObject* guard : guards) ReleaseHookNativeMovement(guard, true); });
        SetOwnershipLock(false);
        LOG("[AI-HOOK] experimental Hook Debug bodyguard mode OFF");
        return;
    }

    bool peReady = InstallProcessEventHook();
    UFunction* fn = FindFunction(AH::Fn_AIUtils_AreFriendlyCharacters);
    bool exactMetadata = false;
    try { exactMetadata = Mem::IsReadable(fn, 0x30) && fn->GetFullName() == AH::Fn_AIUtils_AreFriendlyCharacters; }
    catch (...) { exactMetadata = false; }
    if (exactMetadata)
        g_fnHookAreFriendly.store(fn, std::memory_order_relaxed);
    g_fnHookMoveToActor.store(CachedFn(AH::Fn_AIController_MoveToActor), std::memory_order_relaxed);
    g_fnHookMoveToLocation.store(CachedFn(AH::Fn_AIController_MoveToLocation), std::memory_order_relaxed);
    g_fnHookStopMovement.store(CachedFn(AH::Fn_AIController_StopMovement), std::memory_order_relaxed);
    g_fnHookSetCharacterAggressive.store(CachedFn(AH::Fn_AIUtils_SetCharacterAggressive), std::memory_order_relaxed);
    g_fnHookSetCharacterPassive.store(CachedFn(AH::Fn_AIUtils_SetCharacterPassive), std::memory_order_relaxed);
    ResolveHookTwinDeathPipelineFns();
    bool fsHook = EnsureHookTwinFightStagingNativeHook();
    EnsureHookTwinActionContainerFactory();
    AiMovementHooks::ResolveHelpers(CachedFn(AH::Fn_Mercuna_MoveToLocation), nullptr);

    const bool ready = peReady && exactMetadata;
    g_hookBodyguardMode.store(ready, std::memory_order_relaxed);
    if (ready)
    {
        SetOwnershipLock(true);
        LOG("[AI-HOOK] friendship hook resolved via SDK metadata: %s -> %p; ReVa native exec thunk RVA 0x225BDA0 confirmed",
            AH::Fn_AIUtils_AreFriendlyCharacters, (void*)fn);
        LOG("[AI-HOOK] experimental bodyguard mode ACTIVE (follow/protect/attack driven by our hook pump; TargetAlly never receives player)");
        LOG("[AI-MOVE] controller ownership resolved moveActor=%p moveLocation=%p stop=%p",
            g_fnHookMoveToActor.load(), g_fnHookMoveToLocation.load(), g_fnHookStopMovement.load());
        LOG("[AI-DEATH] Hook Twin death/QTE guard resolved death=%p load=%p loadState=%p stagingUFn=%p stagingNative=%p sendDeath=%p sendLoad=%p died=%p qte=%p cachePose=%p destroyOwner=%p",
            g_fnHookK2OnDeath.load(), g_fnHookK2OnLoadDeathState.load(), g_fnHookLoadDeadState.load(),
            g_fnHookTryFightStaging.load(), g_fnHookTryFightStagingNative.load(), g_fnHookSendDeathEvent.load(), g_fnHookSendLoadDeathStateEvent.load(),
            g_fnHookSendCharacterDiedEvent.load(), g_fnHookStartVersusQTE.load(), g_fnHookCacheDeathPose.load(),
            g_fnHookDestroyOwnerCharacter.load());
        LOG("[AI-FSTAGE] Hook Twin fight-staging hooks selector=%d container=%d selections=%llu blocked=%llu containers=%llu",
            fsHook ? 1 : 0, g_hookTwinActionContainerHookLive.load(std::memory_order_relaxed) ? 1 : 0,
            (unsigned long long)g_hookTwinFightStagingSelections.load(std::memory_order_relaxed),
            (unsigned long long)g_hookTwinFightStagingBlocks.load(std::memory_order_relaxed),
            (unsigned long long)g_hookTwinFightStagingContainerLogs.load(std::memory_order_relaxed));
    }
    else
    {
        LOG("[AI-HOOK] optional friendship hook unavailable: ProcessEvent=%d UFunction=%p exactMetadata=%d; mode refused fail-closed",
            peReady ? 1 : 0, (void*)fn, exactMetadata ? 1 : 0);
    }
}

bool Features::HookBodyguardModeActive()
{
    return g_hookBodyguardMode.load(std::memory_order_relaxed);
}

bool Features::HookFriendshipResolved()
{
    return oProcessEvent && Mem::IsReadable(g_fnHookAreFriendly.load(std::memory_order_relaxed), 0x30);
}

uint64_t Features::HookFriendshipForceCount()
{
    return g_friendshipForces.load(std::memory_order_relaxed);
}

uint64_t Features::UnsafeTargetAllySkipCount()
{
    return g_unsafeTargetAllySkips.load(std::memory_order_relaxed);
}

Features::HookBodyguardValidation Features::ValidateHookBodyguardPair(bool writeLog)
{
    HookBodyguardValidation out{};
    UObject* guard = HookDebugSelectedGuard();
    UObject* player = GetLocalPawn();
    out.guard = reinterpret_cast<std::uintptr_t>(guard);
    out.player = reinterpret_cast<std::uintptr_t>(player);
    out.guardManaged = guard && IsHookBodyguard(guard);
    out.guardIsAHAICharacter = guard && NativeHooks::IsAHAICharacter(guard);
    out.playerIsAHAICharacter = player && NativeHooks::IsAHAICharacter(player);
    UClass* baseClass = FindObjectFast(AH::Cls_AHBaseCharacter);
    out.playerIsAHBaseCharacter = player && baseClass && player->IsA(baseClass);
    out.legacyTargetAllyWouldBeUnsafe = player && !out.playerIsAHAICharacter;
    out.currentCodeWouldAssignUnsafeTargetAlly = false;
    out.friendshipWouldForce = HookBodyguardModeActive() && HookFriendshipResolved() &&
                               out.guardManaged && out.guardIsAHAICharacter && player;
    out.crashGuardActive = NativeHooks::CrashGuardActive();
    out.mixedNavigation = guard && IsMixedNavCharacter(guard);
    // Dry-run stays read-only: use only a component already registered by the
    // game-thread native controller; never call GetComponentsByClass from the UI.
    UObject* nav = guard ? AiMovementHooks::RegisteredNavigation(guard) : nullptr;
    UObject* ctrl = guard ? GetAiController(guard) : nullptr;
    out.navigationComponent = reinterpret_cast<std::uintptr_t>(nav);
    out.controller = reinterpret_cast<std::uintptr_t>(ctrl);
    AiMovementHooks::Status move = AiMovementHooks::GetStatus();
    out.nativeMoveHelperResolved = move.moveHelperResolved;
    out.nativeMercunaDetoursLive = move.mercunaDetoursLive;
    out.nativeMovementOwned = guard && (AiMovementHooks::OwnsGuard(guard) || [&]() {
        std::lock_guard<std::mutex> lock(g_hookControllerOwnedMutex);
        return g_hookControllerOwned.find(guard) != g_hookControllerOwned.end();
    }());
    out.currentNavigationMode = out.mixedNavigation ? AiMovementHooks::CurrentMixedNavigation(guard) : 0;
    out.movementBackend = nav ? "Mercuna native/vtable-owned" : (ctrl ? "AIController native/hook-owned" : "none (fail closed)");
    try
    {
        if (guard)
        {
            out.guardName = guard->GetName();
            UObject* cls = guard->Class();
            out.guardClass = Mem::IsReadable(cls, 0x30) ? cls->GetName() : "<unreadable>";
        }
        if (player)
        {
            out.playerName = player->GetName();
            UObject* cls = player->Class();
            out.playerClass = Mem::IsReadable(cls, 0x30) ? cls->GetName() : "<unreadable>";
        }
    }
    catch (...) {}

    if (writeLog)
    {
        LOG("[AI-DRYRUN] guard=%p name=%s class=%s managed=%d isAHAI=%d | player=%p name=%s class=%s isAHBase=%d isAHAI=%d | legacyUnsafe=%d currentUnsafeAttempt=%d friendshipForce=%d crashGuard=%d | movement=%s nav=%p ctrl=%p owned=%d helper=%d detours=%d mixed=%d mode=%u",
            (void*)guard, out.guardName.c_str(), out.guardClass.c_str(), out.guardManaged ? 1 : 0,
            out.guardIsAHAICharacter ? 1 : 0, (void*)player, out.playerName.c_str(),
            out.playerClass.c_str(), out.playerIsAHBaseCharacter ? 1 : 0,
            out.playerIsAHAICharacter ? 1 : 0, out.legacyTargetAllyWouldBeUnsafe ? 1 : 0,
            out.currentCodeWouldAssignUnsafeTargetAlly ? 1 : 0, out.friendshipWouldForce ? 1 : 0,
            out.crashGuardActive ? 1 : 0, out.movementBackend.c_str(), (void*)nav, (void*)ctrl,
            out.nativeMovementOwned?1:0, out.nativeMoveHelperResolved?1:0,
            out.nativeMercunaDetoursLive?1:0, out.mixedNavigation?1:0,
            (unsigned)out.currentNavigationMode);
    }
    return out;
}

void Features::DumpHookAiStatus()
{
    NativeHooks::DumpStatus();
    AiMovementHooks::DumpStatus();
    ValidateHookBodyguardPair(true);
    LOG("[AI-HOOK] mode=%d friendshipResolved=%d friendshipForced=%llu unsafeTargetAllySkipped=%llu ownershipLock=%d ownershipBlocked=%llu directMoveInputs=%llu velocityFallbacks=%llu movementRecoveries=%llu firstHitKills=%llu staleTargetsCleared=%llu",
        HookBodyguardModeActive() ? 1 : 0, HookFriendshipResolved() ? 1 : 0,
        (unsigned long long)HookFriendshipForceCount(), (unsigned long long)UnsafeTargetAllySkipCount(),
        OwnershipLockActive() ? 1 : 0, (unsigned long long)OwnershipSwallowCount(),
        (unsigned long long)g_hookDirectMoveInputs.load(std::memory_order_relaxed),
        (unsigned long long)g_hookVelocityFallbacks.load(std::memory_order_relaxed),
        (unsigned long long)g_hookMovementRecoveries.load(std::memory_order_relaxed),
        (unsigned long long)g_hookFirstHitKills.load(std::memory_order_relaxed),
        (unsigned long long)g_hookStaleTargetsCleared.load(std::memory_order_relaxed));
    LOG("[AI-CHAIN] combat target/state events=%llu/%llu aggressiveFn=%p passiveFn=%p",
        (unsigned long long)g_hookCombatTargetEvents.load(std::memory_order_relaxed),
        (unsigned long long)g_hookCombatStateEvents.load(std::memory_order_relaxed),
        g_fnHookSetCharacterAggressive.load(std::memory_order_relaxed),
        g_fnHookSetCharacterPassive.load(std::memory_order_relaxed));
    LOG("[AI-MOVE] controller external Move/Stop replacements blocked=%llu native-follow-states=%d",
        (unsigned long long)g_hookControllerMovesBlocked.load(std::memory_order_relaxed),
        HookAiCount());
    LOG("[AI-DEATH] Hook Twin deathPipelineBlocked=%llu qtePoseBlocked=%llu fns[death=%p load=%p stagingUFn=%p stagingNative=%p sendDeath=%p died=%p qte=%p cache=%p destroy=%p]",
        (unsigned long long)g_hookTwinDeathPipelineBlocks.load(std::memory_order_relaxed),
        (unsigned long long)g_hookTwinQtePipelineBlocks.load(std::memory_order_relaxed),
        g_fnHookK2OnDeath.load(std::memory_order_relaxed),
        g_fnHookLoadDeadState.load(std::memory_order_relaxed),
        g_fnHookTryFightStaging.load(std::memory_order_relaxed),
        g_fnHookTryFightStagingNative.load(std::memory_order_relaxed),
        g_fnHookSendDeathEvent.load(std::memory_order_relaxed),
        g_fnHookSendCharacterDiedEvent.load(std::memory_order_relaxed),
        g_fnHookStartVersusQTE.load(std::memory_order_relaxed),
        g_fnHookCacheDeathPose.load(std::memory_order_relaxed),
        g_fnHookDestroyOwnerCharacter.load(std::memory_order_relaxed));
    LOG("[AI-FSTAGE] selectorLive=%d containerLive=%d selections=%llu blocked=%llu containers=%llu recentGuard=%p",
        g_hookTwinFightStagingSelectorHookLive.load(std::memory_order_relaxed) ? 1 : 0,
        g_hookTwinActionContainerHookLive.load(std::memory_order_relaxed) ? 1 : 0,
        (unsigned long long)g_hookTwinFightStagingSelections.load(std::memory_order_relaxed),
        (unsigned long long)g_hookTwinFightStagingBlocks.load(std::memory_order_relaxed),
        (unsigned long long)g_hookTwinFightStagingContainerLogs.load(std::memory_order_relaxed),
        g_hookTwinRecentFightStagingGuard.load(std::memory_order_relaxed));
}

void Features::RescanHookMovementResolvers()
{
    UFunction* force = nullptr;
    UObject* guard = HookDebugSelectedGuard();
    if (guard && IsMixedNavCharacter(guard)) force = CachedObjectClassFn(guard, "ForceNavigationType");
    bool ok = AiMovementHooks::ResolveHelpers(CachedFn(AH::Fn_Mercuna_MoveToLocation), force);
    LOG("[AI-MOVE] resolver rescan: moveHelper=%d selectedMixedForceFn=%p", ok?1:0, (void*)force);
}

int Features::HookAiRecruitNearby()
{
    SetHookBodyguardMode(true);
    if (!HookBodyguardModeActive() || !G::sdkReady.load())
        return 0;
    std::vector<UObject*> targets = CollectNearbyAi(g_state.aiRadius, kMaxAiCommandTargets);
    if (targets.empty())
    {
        RequestAiDiscovery();
        LOG("[AI-HOOK] recruit nearby: no candidates yet");
        return 0;
    }
    InstallProcessEventHook();
    QueueGameThread([targets]()
    {
        UObject* player = GetLocalPawn();
        FVector playerLoc{}; ReadLocalPawnLocationFast(playerLoc);
        int added = 0;
        for (UObject* ai : targets)
        {
            if (!AiUsable(ai) || ai == player) continue;
            SquadAdd(ai);
            HookBodyguardAdd(ai);
            ApplyAiBodyguard(ai, player, playerLoc);
            ++added;
        }
        LOG("[AI-HOOK] dedicated roster recruited %d bodyguard(s)", added);
    });
    return (int)targets.size();
}

bool Features::HookAiSpawnBodyguard()
{
    SetHookBodyguardMode(true);
    if (!HookBodyguardModeActive())
        return false;
    if (!G::sdkReady.load() || !InstallProcessEventHook())
        return false;
    SpawnRequest req;
    req.cloneNearest = true;
    req.hookOwned = true;
    req.label = "Hook Diagnostics nearest enemy";
    bool queued = EnqueueSpawn(std::move(req));
    LOG("[AI-HOOK] dedicated spawn %s", queued ? "queued" : "refused (queue full)");
    return queued;
}

bool Features::HookAiSpawnModel(int index)
{
    SetHookBodyguardMode(true);
    if (!HookBodyguardModeActive() || !G::sdkReady.load()) return false;
    RefreshLiveModels();
    if (index < 0 || index >= (int)g_liveModels.size())
    {
        LOG("[AI-HOOK] spawn selected refused: live model index %d unavailable", index);
        return false;
    }
    UClass* cls = g_liveModels[index].cls;
    std::string label = g_liveModels[index].name;
    if (!Mem::IsReadable(cls, 0x30) || !InstallProcessEventHook()) return false;
    SpawnRequest req;
    req.cls = cls;
    req.label = "Hook Diagnostics " + label;
    req.hookOwned = true;
    bool queued = EnqueueSpawn(std::move(req));
    LOG("[AI-HOOK] selected runtime class '%s' spawn %s", label.c_str(), queued ? "queued" : "refused");
    return queued;
}

bool Features::HookAiSpawnModelByName(const char* prettyName)
{
    SetHookBodyguardMode(true);
    if (!HookBodyguardModeActive() || !G::sdkReady.load() || !prettyName || !*prettyName)
        return false;
    UClass* cls = nullptr;
    std::string label, path;
    {
        std::lock_guard<std::mutex> lk(g_allModelsMutex);
        for (const LiveModel& model : g_allModels)
            if (model.name == prettyName)
            { cls = model.cls; label = model.name; path = model.path; break; }
    }
    if (!Mem::IsReadable(cls, 0x30) && path.empty())
    {
        LOG("[AI-HOOK] model '%s' refused: class/path unavailable", prettyName);
        return false;
    }
    if (!InstallProcessEventHook()) return false;
    SpawnRequest req;
    if (Mem::IsReadable(cls, 0x30)) req.cls = cls; else req.path = path;
    req.label = "Hook Diagnostics " + label;
    req.hookOwned = true;
    bool queued = EnqueueSpawn(std::move(req));
    LOG("[AI-HOOK] searchable model '%s' spawn %s (%s)", label.c_str(),
        queued ? "queued" : "refused", cls ? "loaded runtime class" : "load-on-demand class path");
    return queued;
}

void Features::HookAiFollow()
{
    SetHookBodyguardMode(true);
    if (!HookBodyguardModeActive()) return;
    std::vector<UObject*> guards = HookBodyguardSnapshot();
    if (guards.empty()) { LOG("[AI-HOOK] follow: dedicated roster empty"); return; }
    InstallProcessEventHook();
    QueueGameThread([guards]()
    {
        UObject* player = GetLocalPawn();
        int n = 0;
        for (UObject* guard : guards)
        {
            if (!AiUsable(guard)) continue;
            ClearHookBodyguardCombat(guard, player, "manual native follow");
            g_engagedUntilMs.erase(guard);
            ReleaseHookNativeMovement(guard, true); // next 20 Hz pass acquires a clean native request
            ++n;
        }
        LOG("[AI-HOOK] native follow reset for %d dedicated bodyguard(s)", n);
    });
}

void Features::HookAiAttack()
{
    SetHookBodyguardMode(true);
    if (!HookBodyguardModeActive()) return;
    std::vector<UObject*> units = HookBodyguardSnapshot();
    if (units.empty()) { LOG("[AI-HOOK] attack: dedicated roster empty"); return; }
    InstallProcessEventHook();
    QueueGameThread([units]()
    {
        UObject* player = GetLocalPawn();
        FVector playerLoc{};
        if (!ReadLocalPawnLocationFast(playerLoc) || !Mem::IsReadable(player, 0x30))
            return;
        std::vector<UObject*> all = CollectAllCachedAi(kMaxCachedAiActors);
        std::vector<InjNode> enemies;
        std::vector<UObject*> candidates;
        for (UObject* actor : all)
        {
            bool managed = false;
            for (UObject* guard : units) if (guard == actor) { managed = true; break; }
            if (!managed) candidates.push_back(actor);
        }
        BuildInjNodes(candidates, enemies);
        int n = 0;
        for (UObject* guard : units)
        {
            FVector loc{};
            if (!AiUsable(guard) || !ReadActorLocationFast(guard, loc)) continue;
            UObject* target = nullptr;
            float best = 3.4e38f;
            for (const InjNode& candidate : enemies)
            {
                if (!candidate.ok || candidate.actor == guard || candidate.actor == player ||
                    IsHookBodyguard(candidate.actor) || !IsLiveCombatTarget(candidate.actor) ||
                    !AiIsCombatCapable(candidate.actor)) continue;
                float d = DistanceMetres(loc, candidate.loc);
                if (d <= kHookAttackerAwarenessM && d < best)
                { best = d; target = candidate.actor; }
            }
            if (target && InjectHookBodyguard(guard, player, playerLoc, target, true)) ++n;
        }
        LOG("[AI-HOOK] attack dispatched for %d dedicated bodyguard(s)", n);
    });
}

void Features::HookAiRelease()
{
    std::vector<UObject*> guards = HookBodyguardSnapshot();
    {
        std::lock_guard<std::mutex> lk(g_hookBodyguardMutex);
        g_hookBodyguards.clear();
    }
    if (guards.empty()) return;
    InstallProcessEventHook();
    QueueGameThread([guards]()
    {
        int n = 0;
        for (UObject* guard : guards)
        {
            ReleaseHookNativeMovement(guard, true);
            bool released = AiUsable(guard) && ApplyAiRelease(guard);
            SquadRemove(guard);
            ForgetHookBodyguardRuntimeState(guard, !released);
            if (released) ++n;
        }
        LOG("[AI-HOOK] released %d dedicated bodyguard(s)", n);
    });
}

void Features::HookAiDeleteRoster()
{
    std::vector<UObject*> guards = HookBodyguardSnapshot();
    {
        std::lock_guard<std::mutex> lk(g_hookBodyguardMutex);
        g_hookBodyguards.clear();
    }
    for (UObject* guard : guards)
        g_selectedAi.erase(std::remove(g_selectedAi.begin(), g_selectedAi.end(), guard), g_selectedAi.end());
    if (guards.empty()) { LOG("[AI-HOOK] delete roster: dedicated roster empty"); return; }
    InstallProcessEventHook();
    QueueGameThread([guards]()
    {
        UFunction* fn = CachedFn(AH::Fn_K2DestroyActor);
        int n = 0;
        for (UObject* guard : guards)
        {
            ReleaseHookNativeMovement(guard, true);
            SquadRemove(guard);
            ForgetHookBodyguardRuntimeState(guard, true);
            if (!Mem::IsReadable(guard, 0x30) || !fn) continue;
            uint8_t noParams = 0;
            try { guard->ProcessEvent(fn, &noParams); ++n; }
            catch (...) { LOG("[AI-HOOK] delete roster: destroy exception guard=%p", (void*)guard); }
        }
        RequestAiDiscovery();
        LOG("[AI-HOOK] deleted %d dedicated bodyguard(s); runtime state cleared", n);
    });
}

int Features::HookAiCount()
{
    int count = 0;
    std::lock_guard<std::mutex> lk(g_hookBodyguardMutex);
    for (UObject* guard : g_hookBodyguards)
        if (Mem::IsReadable(guard, 0x30)) ++count;
    return count;
}

const char* Features::HordeStatusText()
{
    static char buf[176];
    if (!g_hordeActive.load())
    {
        snprintf(buf, sizeof(buf), "Idle -- pick a location and start a run.");
        return buf;
    }
    snprintf(buf, sizeof(buf), "Wave %d   alive %d   queued %d   kills %d   [SAVES BLOCKED]",
             g_hordeRound.load(), g_hordeAlive.load(), g_hordePending.load(), g_hordeKills.load());
    return buf;
}

int Features::HordeLocationCount()
{
    std::lock_guard<std::mutex> lk(g_arenasMutex);
    LoadArenas();
    return 1 + (int)g_arenas.size(); // index 0 is the implicit "Here"
}

const char* Features::HordeLocationName(int i)
{
    static thread_local std::string s;
    if (i == 0) { s = "Here (fight in place)"; return s.c_str(); }
    std::lock_guard<std::mutex> lk(g_arenasMutex);
    LoadArenas();
    int idx = i - 1;
    if (idx < 0 || idx >= (int)g_arenas.size()) { s = "?"; return s.c_str(); }
    s = g_arenas[idx].name.empty() ? "(unnamed arena)" : g_arenas[idx].name;
    return s.c_str();
}

void Features::HordeSaveLocationHere(const char* name)
{
    if (!G::sdkReady.load()) { LOG("HordeSaveLocationHere: SDK not ready"); return; }
    UObject* pawn = GetLocalPawn();
    FVector loc{};
    if (!Mem::IsReadable(pawn, 0x30) || !ReadActorLocation(pawn, loc))
    {
        LOG("HordeSaveLocationHere: no pawn / location");
        return;
    }
    ArenaLocation a;
    a.name = (name && *name) ? name : "Arena";
    a.loc = loc;
    FRotator rot{}; if (GetControlRotation(rot)) a.rot = rot;
    std::lock_guard<std::mutex> lk(g_arenasMutex);
    LoadArenas();
    g_arenas.push_back(a);
    SaveArenas();
    LOG("HordeSaveLocationHere: saved '%s' @ %.0f %.0f %.0f", a.name.c_str(), loc.X, loc.Y, loc.Z);
}

void Features::HordeDeleteLocation(int i)
{
    if (i < 1) return; // index 0 ("Here") can't be deleted
    std::lock_guard<std::mutex> lk(g_arenasMutex);
    LoadArenas();
    int idx = i - 1;
    if (idx < 0 || idx >= (int)g_arenas.size()) return;
    LOG("HordeDeleteLocation: removed '%s'", g_arenas[idx].name.c_str());
    g_arenas.erase(g_arenas.begin() + idx);
    SaveArenas();
}

// =======================================================================
//  SQUAD / SELECTION / DISPATCH  --  the AI-control core
// =======================================================================
int Features::AiSquadCount() { return g_spawnedAllyCount.load(); }
int Features::AiSelectedCount() { return (int)g_selectedAi.size(); }

// Build the nearby-AI list for the menu (render thread). Each row carries a stable
// id = (uintptr_t)actor used for select/dispatch, plus its squad/selected state.
std::vector<Features::AiListEntry> Features::AiNearbyList(int maxCount)
{
    std::vector<AiListEntry> out;
    if (!G::sdkReady.load()) return out;
    // Keep the cache fresh while the roster is open, but THROTTLE -- this is called
    // every UI frame; requesting discovery every frame drove the worker to rescan
    // ~10k actors at 15Hz (log spam + wasted CPU).
    static ULONGLONG lastReqMs = 0;
    ULONGLONG nowReq = GetTickCount64();
    if (nowReq - lastReqMs > 500) { lastReqMs = nowReq; RequestAiDiscovery(); }
    std::vector<AiCachedActor> snap = CopyAiSnapshot(); // distance-sorted
    out.reserve(snap.size());
    for (const AiCachedActor& e : snap)
    {
        if ((int)out.size() >= maxCount) break;
        if (!Mem::IsReadable(e.actor, 0x30)) continue;
        AiListEntry row{};
        row.id         = (unsigned long long)(uintptr_t)e.actor;
        row.distanceM  = e.distanceM;
        row.healthFrac = e.healthFrac;
        row.inSquad    = IsSquadMember(e.actor);
        row.selected   = IsSelectedAi(e.actor);
        try { row.name = e.actor->GetName(); } catch (...) { row.name = "?"; }
        out.push_back(std::move(row));
    }
    return out;
}

namespace
{
    // Resolve a UI id back to a live, usable AI by matching it against the cache.
    UObject* ResolveAiId(unsigned long long id)
    {
        UObject* p = reinterpret_cast<UObject*>((uintptr_t)id);
        std::vector<AiCachedActor> snap = CopyAiSnapshot();
        for (const AiCachedActor& e : snap)
            if (e.actor == p) return AiUsable(p) ? p : nullptr;
        return nullptr;
    }
}

void Features::AiToggleSelect(unsigned long long id)
{
    UObject* ai = ResolveAiId(id);
    if (!ai) return;
    for (size_t i = 0; i < g_selectedAi.size(); ++i)
        if (g_selectedAi[i] == ai) { g_selectedAi.erase(g_selectedAi.begin() + i); return; }
    g_selectedAi.push_back(ai);
}

void Features::AiSelectAllNearby()
{
    g_selectedAi.clear();
    std::vector<AiCachedActor> snap = CopyAiSnapshot();
    const float r = g_state.aiRadius;
    FVector playerLoc{};
    bool haveLoc = ReadLocalPawnLocationFast(playerLoc);
    for (const AiCachedActor& e : snap)
    {
        if (!AiUsable(e.actor)) continue;
        if (haveLoc && DistanceMetres(e.location, playerLoc) > r) continue;
        g_selectedAi.push_back(e.actor);
    }
    LOG("AiSelectAllNearby: %d selected", (int)g_selectedAi.size());
}

void Features::AiClearSelection() { g_selectedAi.clear(); }

// Convert a set of enemies into squad members on the game thread (friendly + add).
static void RecruitListGameThread(std::vector<UObject*> targets)
{
    InstallProcessEventHook();
    QueueGameThread([targets]()
    {
        try
        {
            UObject* player = GetLocalPawn();
            FVector playerLoc{}; ReadLocalPawnLocationFast(playerLoc);
            int n = 0;
            for (UObject* ai : targets)
            {
                if (!AiUsable(ai) || ai == player) continue;
                ApplyAiBodyguard(ai, player, playerLoc); // friendly + follow baseline
                SquadAdd(ai);                            // join the squad (driven every pump)
                ++n;
            }
            LOG("Recruit: %d unit(s) joined the squad", n);
        }
        catch (...) { LOG("Recruit: exception (ignored)"); }
    });
}

void Features::AiRecruitSelected()
{
    std::vector<UObject*> targets = g_selectedAi; // copy render-thread selection
    if (targets.empty()) { LOG("AiRecruitSelected: nothing selected"); return; }
    RecruitListGameThread(std::move(targets));
}

void Features::AiRecruitNearby()
{
    if (!G::sdkReady.load()) return;
    std::vector<UObject*> targets = CollectNearbyAi(g_state.aiRadius, kMaxAiCommandTargets);
    if (targets.empty()) { RequestAiDiscovery(); LOG("AiRecruitNearby: none nearby yet"); return; }
    RecruitListGameThread(std::move(targets));
}

void Features::AiReleaseSelected()
{
    std::vector<UObject*> targets = g_selectedAi;
    g_selectedAi.clear();
    if (targets.empty()) return;
    InstallProcessEventHook();
    QueueGameThread([targets]()
    {
        try
        {
            int n = 0;
            for (UObject* ai : targets)
            {
                bool hookOwned = IsHookBodyguard(ai);
                if (hookOwned) ReleaseHookNativeMovement(ai, true);
                bool released = AiUsable(ai) && ApplyAiRelease(ai);
                SquadRemove(ai);
                if (hookOwned) ForgetHookBodyguardRuntimeState(ai, !released);
                if (released) ++n;
            }
            LOG("AiReleaseSelected: released %d", n);
        }
        catch (...) {}
    });
}

void Features::AiReleaseSquad()
{
    AiClearSpawnedAllies(); // releases + clears the whole squad
}

// Dispatch: selected (or whole squad if nothing selected) act now.
static std::vector<UObject*> DispatchTargets()
{
    if (!g_selectedAi.empty()) return g_selectedAi;
    std::lock_guard<std::mutex> lk(g_squadMutex);
    return g_spawnedAllies;
}

void Features::AiDispatchAttack()
{
    std::vector<UObject*> units = DispatchTargets();
    if (units.empty()) { LOG("AiDispatchAttack: no selection/squad"); return; }
    InstallProcessEventHook();
    QueueGameThread([units]()
    {
        try
        {
            UObject* player = GetLocalPawn();
            FVector playerLoc{}; ReadLocalPawnLocationFast(playerLoc);
            // Pick the nearest enemy to the player that is NOT one of our units.
            std::vector<UObject*> all = CollectAllCachedAi(kMaxCachedAiActors);
            std::vector<InjNode> enemies;
            {
                std::vector<UObject*> src;
                for (UObject* a : all)
                {
                    bool isUnit = false;
                    for (UObject* u : units) if (u == a) { isUnit = true; break; }
                    if (!isUnit) src.push_back(a);
                }
                BuildInjNodes(src, enemies);
            }
            int n = 0;
            for (UObject* u : units)
            {
                if (!AiUsable(u)) continue;
                FVector loc{}; if (!ReadActorLocationFast(u, loc)) continue;
                UObject* target = NearestNode(enemies, loc, player, -1, u);
                if (target && InjectAttack(u, target, player, 0 /*friendly to you*/)) ++n;
            }
            LOG("AiDispatchAttack: %d unit(s) sent at the nearest enemy", n);
        }
        catch (...) {}
    });
}

void Features::AiDispatchKill()
{
    std::vector<UObject*> units = DispatchTargets();
    if (units.empty()) return;
    InstallProcessEventHook();
    QueueGameThread([units]()
    {
        try { int n = 0; for (UObject* u : units) if (AiUsable(u) && ApplyAiKill(u)) ++n; LOG("AiDispatchKill: %d", n); }
        catch (...) {}
    });
}

// =======================================================================
//  ZONE RESPAWN  --  snapshot the current enemies, respawn that set later
// =======================================================================
namespace
{
    std::mutex                g_zoneMutex;
    std::vector<std::string>  g_zoneSnapshot;   // class paths of the snapshotted enemies
    constexpr int             kMaxZoneSnapshot = 24;
}

void Features::AiSnapshotZone()
{
    if (!G::sdkReady.load()) { LOG("AiSnapshotZone: SDK not ready"); return; }
    std::vector<AiCachedActor> snap = CopyAiSnapshot();
    std::vector<std::string> paths;
    for (const AiCachedActor& e : snap)
    {
        if ((int)paths.size() >= kMaxZoneSnapshot) break;
        if (!AiUsable(e.actor)) continue;
        UObject* c = e.actor->Class();
        if (!Mem::IsReadable(c, 0x30)) continue;
        std::string full = c->GetFullName();      // "<prefix> /Game/.../BP_X.BP_X_C"
        size_t sp = full.find(' ');
        std::string path = (sp != std::string::npos) ? full.substr(sp + 1) : full;
        if (!path.empty()) paths.push_back(path);
    }
    { std::lock_guard<std::mutex> lk(g_zoneMutex); g_zoneSnapshot.swap(paths); }
    LOG("AiSnapshotZone: recorded %d enemy type(s) in this zone", (int)g_zoneSnapshot.size());
}

void Features::AiRespawnZone()
{
    if (!G::sdkReady.load()) { LOG("AiRespawnZone: SDK not ready"); return; }
    std::vector<std::string> paths;
    { std::lock_guard<std::mutex> lk(g_zoneMutex); paths = g_zoneSnapshot; }
    if (paths.empty()) { LOG("AiRespawnZone: nothing snapshotted -- press Snapshot zone first"); return; }
    if (!InstallProcessEventHook()) { LOG("AiRespawnZone: hook unavailable"); return; }
    // Stream each one back in (load-on-demand handles unloaded types). They spawn as
    // your allies; release them if you want them hostile again.
    int q = 0;
    for (const std::string& p : paths)
    {
        SpawnRequest req; req.path = p; req.label = "zone respawn";
        EnqueueSpawn(std::move(req));
        ++q;
    }
    LOG("AiRespawnZone: queued %d streamed respawn(s)", q);
}

int Features::AiZoneSnapshotCount()
{
    std::lock_guard<std::mutex> lk(g_zoneMutex);
    return (int)g_zoneSnapshot.size();
}

// Diagnostic: log nearby volume/trigger actors so we can identify the out-of-bounds
// teleporter (e.g. the lighthouse one) and disable it precisely next. Background
// thread (reads names over the level actor list -- no ProcessEvent).
void Features::DumpNearbyVolumes()
{
    if (!G::sdkReady.load()) { LOG("DumpNearbyVolumes: SDK not ready"); return; }
    QueueGameThread([]()
    {
        try
        {
            FVector playerLoc{}; bool haveLoc = ReadLocalPawnLocationFast(playerLoc);
            std::vector<UObject*> actors; actors.reserve(2048);
            CollectLevelActors(actors);
            static const char* kNeedles[] = { "Volume","Trigger","Bound","Restrict","Teleport",
                                              "Respawn","Barrier","Kill","Death","Forbidden","Zone","OutOf" };
            int hits = 0;
            for (UObject* a : actors)
            {
                if (!Mem::IsReadable(a, 0x30)) continue;
                UObject* c = a->Class();
                if (!Mem::IsReadable(c, 0x30)) continue;
                std::string cn;
                try { cn = c->GetName(); } catch (...) { continue; }
                bool match = false;
                for (const char* n : kNeedles) if (cn.find(n) != std::string::npos) { match = true; break; }
                if (!match) continue;
                float dist = -1.0f; FVector loc{};
                if (haveLoc && ReadActorLocationFast(a, loc)) dist = DistanceMetres(loc, playerLoc);
                if (dist >= 0.0f && dist > 120.0f) continue; // only what's near you
                LOG("VOLUME near you: class=%s dist=%.0fm actor=%p", cn.c_str(), dist, (void*)a);
                if (++hits >= 60) break;
            }
            LOG("DumpNearbyVolumes: %d candidate volume(s) within 120m -- check AtomicHeartMenu.log", hits);
        }
        catch (...) { LOG("DumpNearbyVolumes: exception (ignored)"); }
    });
    InstallProcessEventHook();
}

void Features::DebugDumpGameSnapshot()
{
    try { RequestManualDiagnosticSnapshot(); }
    catch (...) { LOG("Diagnostics: manual request exception (ignored)"); }
}

void Features::DebugDumpTargetedActor(const char* nameSubstr)
{
    try { ScheduleTargetedDump(nameSubstr ? std::string(nameSubstr) : std::string()); }
    catch (...) { LOG("Diagnostics: targeted dump exception (ignored)"); }
}

void Features::DebugSetTraceTarget(const char* nameSubstr)
{
    try { SetDiagTraceTargetByName(nameSubstr ? std::string(nameSubstr) : std::string()); }
    catch (...) { LOG("Diagnostics: set trace target exception (ignored)"); }
}

const char* Features::DebugTraceTargetName()
{
    static std::string name;
    std::lock_guard<std::mutex> lk(g_diagMutex);
    name = g_traceTargetName;
    return name.c_str();
}

const char* Features::DebugLastDumpDir()
{
    static std::string last;
    std::lock_guard<std::mutex> lk(g_diagMutex);
    last = g_diagLastDir;
    return last.c_str();
}

const std::vector<Features::EspEntry>& Features::BuildEspFrame(float screenW, float screenH)
{
    static std::vector<EspEntry> out;
    out.clear();

    if (!g_state.espEnabled || screenW <= 1.0f || screenH <= 1.0f)
        return out;

    FVector camLoc{};
    FRotator camRot{};
    float fov = 0.0f;
    if (!ReadCameraPOV(camLoc, camRot, fov))
        return out;

    std::vector<AiCachedActor> snapshot = CopyAiSnapshot();
    RefreshAiRenderLocations(snapshot);
    snapshot = CopyAiSnapshot();
    const float maxDistance = g_state.espMaxDistance < 5.0f ? 5.0f : g_state.espMaxDistance;
    out.reserve(snapshot.size());

    for (const AiCachedActor& e : snapshot)
    {
        if (!Mem::IsReadable(e.actor, 0x30))
            continue;

        float distance = DistanceMetres(e.location, camLoc);
        if (distance > maxDistance)
            continue;

        FVector head = e.location;
        FVector feet = e.location;
        head.Z += 95.0f;
        feet.Z -= 95.0f;

        float hx = 0.0f, hy = 0.0f, fx = 0.0f, fy = 0.0f;
        if (!WorldToScreen(head, camLoc, camRot, fov, screenW, screenH, hx, hy) ||
            !WorldToScreen(feet, camLoc, camRot, fov, screenW, screenH, fx, fy))
            continue;

        float h = fabsf(fy - hy);
        if (h < 4.0f)
            continue;
        float w = h * 0.45f;

        EspEntry entry{};
        entry.x = fx - w * 0.5f;
        entry.y = hy;
        entry.w = w;
        entry.h = h;
        entry.feetX = fx;
        entry.feetY = fy;
        entry.headX = fx;
        entry.headY = hy;
        entry.distance = distance;
        entry.healthFrac = e.healthFrac;
        entry.inSquad  = IsSquadMember(e.actor); // under our control -> glow + arrow
        entry.selected = IsSelectedAi(e.actor);  // UI-selected -> stronger highlight
        out.push_back(entry);
    }

    return out;
}

void Features::Prewarm()
{
    if (!G::sdkReady.load()) return;
    try
    {
        ULONGLONG startMs = GetTickCount64();
        StartObjectNameIndex();
        CachedFn(AH::Fn_GetActorLocation);
        CachedFn(AH::Fn_SetActorLocation);
        CachedFn(AH::Fn_SetActorEnableCollision);
        CachedFn(AH::Fn_SetActorScale3D);
        CachedFn(AH::Fn_LaunchCharacter);
        CachedFn(AH::Fn_BaseWeapon_FullUpgrade);
        CachedFn(AH::Fn_GetControlRotation);
        CachedFn(AH::Fn_InvalidateStreaming);
        CachedFn(AH::Fn_GetAHWorldStreamingSubsystem);
        CachedFn(AH::Fn_EnableLevelStreaming);
        CachedFn(AH::Fn_GetCurrentWeapon);
        CachedFn(AH::Fn_GetWeaponInventoryAmmoCount);
        CachedFn(AH::Fn_InstantTakeWeapon);
        CachedFn(AH::Fn_TakeWeapon);
        CachedFn(AH::Fn_EquipWeaponByDataAsset);
        CachedFn(AH::Fn_GetCurrentWeaponInventoryAmmoCount);
        CachedFn(AH::Fn_GetInventoryPlayer);
        CachedFn(AH::Fn_AHInventory_AddItemsToInventory);
        CachedFn(AH::Fn_AHInventory_GetItemsCount);
        CachedFn(AH::Fn_AHInventory_SetIgnoreOverWeight);
        CachedFn(AH::Fn_BaseWeapon_GetAmmoInPossession);
        CachedFn(AH::Fn_BaseWeapon_GetAmmoCount);
        CachedFn(AH::Fn_BaseWeapon_GetAmmoSize);
        CachedFn(AH::Fn_EBBarrel_GetAmmoCount);
        CachedFn(AH::Fn_EBBarrel_SetAmmo);
        CachedFn(AH::Fn_SetIgnoreLookInput);
        CachedFn(AH::Fn_SetIgnoreMoveInput);
        CachedFn(AH::Fn_Debug_InstantLockUnlock);
        CachedFn(AH::Fn_Debug_SetInstantPuzzleResolve);
        CachedFn(AH::Fn_Debug_WinQTE);
        CachedFn(AH::Fn_Debug_PromoteAllActiveQuests);
        CachedFn(AH::Fn_Debug_CompleteAllActiveQuests);
        FindObjectFast(AH::Cls_AIController);
        FindObjectFast(AH::Cls_Pawn);
        FindObjectFast(AH::Cls_Character);
        CachedFn(AH::Fn_AIController_MoveToActor);
        CachedFn(AH::Fn_AIController_MoveToLocation);
        CachedFn(AH::Fn_AIController_StopMovement);
        CachedFn(AH::Fn_AIBlueprintHelper_SimpleMoveToActor);
        CachedFn(AH::Fn_Pawn_AddMovementInput);
        // Pre-arm the fight-staging native hook after SDK/module globals are ready.
        // Previously this was only attempted when Hook Bodyguard mode was toggled, so
        // a clean startup log could misleadingly miss the expected LIVE line.
        ResolveHookTwinDeathPipelineFns();
        EnsureHookTwinFightStagingNativeHook();
        // Mercuna (the game's real ground-AI mover). Resolve on the worker thread so the
        // game-thread squad walk never triggers a slow GObjects scan.
        FindObjectFast(AH::Cls_MercunaNavComponent);
        CachedFn(AH::Fn_Mercuna_MoveToActor);
        CachedFn(AH::Fn_Mercuna_MoveToLocation);
        CachedFn(AH::Fn_Mercuna_Stop);
        // Render-hijack functions (console / lights / chams material params).
        CachedObject(AH::Obj_KismetSystemLibrary);
        CachedFn(AH::Fn_ExecuteConsoleCommand);
        CachedFn(AH::Fn_Light_SetLightColor);
        CachedFn(AH::Fn_MeshSetVectorParamOnMaterials);
        CachedFn(AH::Fn_MeshSetScalarParamOnMaterials);
        CachedFn(AH::Fn_ActorGetComponentsByClass);
        CachedFn(AH::Fn_PrimGetNumMaterials);
        CachedFn(AH::Fn_PrimGetMaterial);
        CachedFn(AH::Fn_PrimSetMaterial);
        CachedFn(AH::Fn_PrimCreateMID);
        CachedFn(AH::Fn_PrimCreateMIDFromMaterial);
        CachedFn(AH::Fn_MIDSetVectorParam);
        CachedFn(AH::Fn_MIDSetScalarParam);
        CachedFn(AH::Fn_PrimSetRenderCustomDepth);
        CachedFn(AH::Fn_PrimSetCustomDepthStencil);
        CachedObject(AH::Obj_WeaponRgbScannerSelectMaterial);
        CachedObject(AH::Obj_WeaponRgbGameEmissiveMaterial);
        CachedObject(AH::Obj_WeaponRgbTurretEmissiveMaterial);
        CachedObject(AH::Obj_WeaponRgbBasicShapeMaterial);
        CachedObject(AH::Obj_WeaponRgbDebugMaterial);
        CachedObject(AH::Obj_WeaponRgbDefaultMaterial);
        CachedObject(AH::Obj_WeaponRgbWorldGridMaterial);
        { std::vector<UE::FName> c, s, o; ResolveWeaponRgbParamNames(c, s); ResolveWeaponRgbOpacityParamNames(o); ResolveWeaponRgbForcedParamNames(c, s); }
        FindObjectFast(AH::Cls_MeshComponent);
        FindObjectFast(AH::Cls_Light);
        StartObjectNameIndex();

        LOG("Prewarm complete in %llums", GetTickCount64() - startMs);
    }
    catch (...) { LOG("Prewarm: exception (ignored)"); }
}

// Top the player's health attribute to its max once (button-driven). Raw guarded
// write -- no ProcessEvent -- so it's safe to call straight from the menu thread.
void Features::FullHeal()
{
    if (!G::sdkReady.load()) { LOG("FullHeal: SDK not ready"); return; }
    try
    {
        UObject* pawn = GetLocalPawn();
        uint8_t* base = reinterpret_cast<uint8_t*>(pawn);
        if (!Mem::IsReadable(base + AH::Char_AttributeSet, sizeof(void*)))
        {
            LOG("FullHeal: no pawn / attribute set");
            return;
        }
        uint8_t* set = *reinterpret_cast<uint8_t**>(base + AH::Char_AttributeSet);
        if (!Mem::IsReadable(set + AH::Set_MaxHealth + AH::Attr_CurrentValue, sizeof(float)))
        {
            LOG("FullHeal: attribute set unreadable");
            return;
        }
        float mx = *reinterpret_cast<float*>(set + AH::Set_MaxHealth + AH::Attr_CurrentValue);
        if (mx > 0.0f && std::isfinite(mx))
        {
            *reinterpret_cast<float*>(set + AH::Set_Health + AH::Attr_BaseValue)    = mx;
            *reinterpret_cast<float*>(set + AH::Set_Health + AH::Attr_CurrentValue) = mx;
            LOG("FullHeal: health -> %.0f", mx);
        }
    }
    catch (...) { LOG("FullHeal: exception (ignored)"); }
}

void Features::SavePosition()
{
    try
    {
        UObject* pawn = GetLocalPawn();
        FVector loc{};
        if (!pawn || !ReadActorLocation(pawn, loc))
        {
            LOG("SavePosition failed: pawn=%p", (void*)pawn);
            return;
        }
        g_state.savedLocation = loc;
        g_state.hasSaved = true;
        LOG("Saved position %.1f %.1f %.1f", loc.X, loc.Y, loc.Z);
    }
    catch (...) { LOG("SavePosition: exception (ignored)"); }
}

void Features::TeleportToSaved()
{
    try
    {
        if (!g_state.hasSaved)
        {
            LOG("TeleportToSaved failed: no saved position.");
            return;
        }
        UObject* pawn = GetLocalPawn();
        if (!SetActorLocation(pawn, g_state.savedLocation, true))
        {
            LOG("TeleportToSaved failed: pawn=%p", (void*)pawn);
            return;
        }
        LOG("Teleported to saved position %.1f %.1f %.1f",
            g_state.savedLocation.X, g_state.savedLocation.Y, g_state.savedLocation.Z);
    }
    catch (...) { LOG("TeleportToSaved: exception (ignored)"); }
}

void Features::ReturnToFlyStart()
{
    try
    {
        if (!g_state.hasFlyStart)
        {
            LOG("ReturnToFlyStart failed: no fly start captured.");
            return;
        }

        UObject* pawn = GetLocalPawn();
        if (!SetActorLocation(pawn, g_state.flyStartLocation, true))
        {
            LOG("ReturnToFlyStart failed: pawn=%p", (void*)pawn);
            return;
        }

        RefreshFlyStreaming(pawn, true);
        LOG("Returned to fly start %.1f %.1f %.1f",
            g_state.flyStartLocation.X, g_state.flyStartLocation.Y, g_state.flyStartLocation.Z);
    }
    catch (...) { LOG("ReturnToFlyStart: exception (ignored)"); }
}

void Features::RefillAmmoNow()
{
    if (!G::sdkReady.load())
    {
        LOG("RefillAmmoNow failed: SDK not ready");
        return;
    }

    try
    {
        UObject* pawn = GetLocalPawn();
        if (!pawn)
        {
            LOG("RefillAmmoNow failed: no local pawn");
            return;
        }

        ApplyInfiniteAmmo(pawn, GetCurrentWeaponObject(pawn), true, false, " manual");
    }
    catch (...) { LOG("RefillAmmoNow: exception (ignored)"); }
}

void Features::SolveCurrentPuzzle()
{
    if (!G::sdkReady.load())
    {
        LOG("SolveCurrentPuzzle failed: SDK not ready");
        return;
    }

    try
    {
        bool pulseInstant = false;
        if (!g_state.instantPuzzleResolve)
            pulseInstant = ApplyInstantPuzzleResolve(true, true);

        bool lock = CallDebugNoParams(AH::Fn_Debug_InstantLockUnlock, "InstantLockUnlock");
        bool qte = CallDebugNoParams(AH::Fn_Debug_WinQTE, "WinQTE");
        // Hand the interactive puzzles (minigame button grids/dials AND the
        // BP_LockComponent door locks) to the worker thread -- discovery sweeps
        // GObjects and must NOT run on the render/Present path. The worker picks
        // this up within ~50ms and the render Tick fires a small bounded batch.
        g_puzzleSolveOnce = true;
        if (pulseInstant)
            ApplyInstantPuzzleResolve(false, true);

        LOG("SolveCurrentPuzzle: instantPulse=%s lock=%s qte=%s (minigame solve queued)",
            pulseInstant ? "yes" : (g_state.instantPuzzleResolve ? "already-on" : "no"),
            lock ? "yes" : "no",
            qte ? "yes" : "no");
    }
    catch (...) { LOG("SolveCurrentPuzzle: exception (ignored)"); }
}

void Features::WorkerTick()
{
    // Worker-thread heartbeat (called from the dllmain idle loop). All heavy
    // GObjects scanning happens here so the render thread never stalls.
    // ProcessEvent itself is deferred to the render Tick.
    if (!G::sdkReady.load())
        return;

    static ULONGLONG lastAiExceptionLogMs = 0;
    static ULONGLONG lastPuzzleExceptionLogMs = 0;

    try
    {
        RefreshAiActors();
        RefreshAllModelsWorker(); // global model search list (only sweeps while panel open)
        RunDeepKillWorker();      // one-shot deep enemy sweep when requested
    }
    catch (...)
    {
        ULONGLONG nowMs = GetTickCount64();
        if (nowMs - lastAiExceptionLogMs > 3000)
        {
            LOG("WorkerTick: AI refresh exception (throttled)");
            lastAiExceptionLogMs = nowMs;
        }
    }

    try
    {
        static ULONGLONG lastScanMs = 0;
        static bool      prevContinuous = false;

        bool oneShot    = g_puzzleSolveOnce.exchange(false);
        bool continuous = g_state.instantPuzzleResolve;   // tie auto-solve to the toggle
        bool enableEdge = continuous && !prevContinuous;  // just switched on -> scan now
        prevContinuous  = continuous;

        if (!oneShot && !continuous)
            return;

        ULONGLONG nowMs = GetTickCount64();
        // The button press and the enable edge scan immediately; steady-state
        // polling is throttled so the background sweep does not peg a core.
        if (!oneShot && !enableEdge && nowMs - lastScanMs < 1500)
            return;
        lastScanMs = nowMs;

        // verbose (full counters + candidate dump on a miss) only for the
        // explicit button press / toggle enable, not the steady-state poll.
        DiscoverPuzzles(oneShot || enableEdge);
    }
    catch (...)
    {
        ULONGLONG nowMs = GetTickCount64();
        if (nowMs - lastPuzzleExceptionLogMs > 3000)
        {
            LOG("WorkerTick: puzzle scan exception (throttled)");
            lastPuzzleExceptionLogMs = nowMs;
        }
    }
}

void Features::UnlockCurrentLock()
{
    if (!G::sdkReady.load())
    {
        LOG("UnlockCurrentLock failed: SDK not ready");
        return;
    }

    try { CallDebugNoParams(AH::Fn_Debug_InstantLockUnlock, "InstantLockUnlock"); }
    catch (...) { LOG("UnlockCurrentLock: exception (ignored)"); }
}

void Features::WinCurrentQTE()
{
    if (!G::sdkReady.load())
    {
        LOG("WinCurrentQTE failed: SDK not ready");
        return;
    }

    try { CallDebugNoParams(AH::Fn_Debug_WinQTE, "WinQTE"); }
    catch (...) { LOG("WinCurrentQTE: exception (ignored)"); }
}

void Features::SkipObjective()
{
    if (!G::sdkReady.load())
    {
        LOG("SkipObjective failed: SDK not ready");
        return;
    }

    // PromoteAllActiveQuests advances every active quest one step -- the game's own
    // debug "skip the current objective" (jumps past gates like "you need a ticket
    // for the train"). Press again to skip the next objective.
    try { CallDebugNoParams(AH::Fn_Debug_PromoteAllActiveQuests, "PromoteAllActiveQuests"); }
    catch (...) { LOG("SkipObjective: exception (ignored)"); }
}

void Features::CompleteActiveQuests()
{
    if (!G::sdkReady.load())
    {
        LOG("CompleteActiveQuests failed: SDK not ready");
        return;
    }

    try { CallDebugNoParams(AH::Fn_Debug_CompleteAllActiveQuests, "CompleteAllActiveQuests"); }
    catch (...) { LOG("CompleteActiveQuests: exception (ignored)"); }
}

bool Features::GiveWeapon(int index, bool equip)
{
    if (!G::sdkReady.load())
    {
        LOG("GiveWeapon failed: SDK not ready");
        return false;
    }
    try { return GiveWeaponInternal(index, equip); }
    catch (...) { LOG("GiveWeapon: exception (ignored)"); return false; }
}

int Features::GiveAllWeapons(bool equipLast)
{
    if (!G::sdkReady.load())
    {
        LOG("GiveAllWeapons failed: SDK not ready");
        return 0;
    }

    try
    {
        if (g_giveAll.active)
        {
            LOG("GiveAllWeapons already running: index=%d ok=%d/%d", g_giveAll.index, g_giveAll.ok, WeaponCount());
            return g_giveAll.ok;
        }

        g_giveAll = {};
        g_giveAll.active = true;
        g_giveAll.equipLast = equipLast;
        LOG("GiveAllWeapons queued: count=%d equipLast=%s", WeaponCount(), equipLast ? "yes" : "no");
    }
    catch (...) { LOG("GiveAllWeapons: exception (ignored)"); }
    return 0;
}

// ---- Mercuna (the game's real ground-AI pather) -------------------------------
// Get (+cache) the AI pawn's UMercunaNavigationComponent. Game-thread only (no mutex).
static UObject* GetMercunaNavComp(UObject* ai)
{
    static std::unordered_map<UObject*, UObject*> cache;
    auto it = cache.find(ai);
    if (it != cache.end() && Mem::IsReadable(it->second, 0x30))
        return it->second;
    static UClass* navCls = nullptr;
    if (!Mem::IsReadable(navCls, 0x30)) navCls = FindObjectFast(AH::Cls_MercunaNavComponent);
    UFunction* fn = CachedFn(AH::Fn_ActorGetComponentsByClass);
    if (!navCls || !fn || !Mem::IsReadable(ai, 0x30)) return nullptr;
    UObject* found = nullptr;
    try
    {
        P_ActorGetComponentsByClass p{};
        p.ComponentClass = navCls;
        ai->ProcessEvent(fn, &p);
        int n = p.ReturnValue.Count;
        if (n > 0 && Mem::IsReadable(p.ReturnValue.Data, sizeof(void*)))
            found = p.ReturnValue.Data[0];
    }
    catch (...) {}
    if (Mem::IsReadable(found, 0x30)) { cache[ai] = found; return found; }
    return nullptr;
}

// Path the AI to the player ACTOR via Mercuna = the game's OWN ground mover (real walk
// + locomotion animation + obstacle avoidance). It auto-tracks the actor, so re-issue
// only on a timer. Returns false if the pawn has no Mercuna nav component.
static bool MercunaMoveToPlayer(UObject* ai, UObject* player, float endDistU, float speed)
{
    UObject* nav = GetMercunaNavComp(ai);
    UFunction* fn = CachedFn(AH::Fn_Mercuna_MoveToActor);
    if (!Mem::IsReadable(nav, 0x30) || !fn || !Mem::IsReadable(player, 0x30))
    {
        static ULONGLONG lg = 0; ULONGLONG n = GetTickCount64();
        if (n - lg > 2000) { lg = n; LOG("Mercuna MoveToActor SKIP: nav=%p fn=%p target=%p (game can't accept the move)", (void*)nav, (void*)fn, (void*)player); }
        return false;
    }
    struct { void* Actor; float EndDistance; float Speed; bool UsePartialPath; uint8_t pad[7]; } p{};
    p.Actor = player;
    p.EndDistance = endDistU;
    p.Speed = speed;
    p.UsePartialPath = true;
    try { nav->ProcessEvent(fn, &p); }
    catch (...) { LOG("Mercuna MoveToActor EXCEPTION: nav=%p target=%p endDist=%.0f", (void*)nav, (void*)player, endDistU); return false; }
    return true;
}

// Cancel a pawn's current Mercuna move. Mercuna MoveToActor is a CONTINUOUS "follow this
// actor" order, so re-issuing it every pump stacks path requests (lag grows linearly until
// she stalls). We issue MoveToActor once and call this to cleanly END the move when she
// should hold or stand down. Game thread; crash-safe (guarded, /EHa-wrapped).
static bool MercunaStop(UObject* ai)
{
    UObject* nav = GetMercunaNavComp(ai);
    UFunction* fn = CachedFn(AH::Fn_Mercuna_Stop);
    if (!Mem::IsReadable(nav, 0x30) || !fn)
    {
        static ULONGLONG lg = 0; ULONGLONG n = GetTickCount64();
        if (n - lg > 2000) { lg = n; LOG("Mercuna Stop SKIP: nav=%p fn=%p", (void*)nav, (void*)fn); }
        return false;
    }
    uint8_t noParams = 0;
    try { nav->ProcessEvent(fn, &noParams); } catch (...) { LOG("Mercuna Stop EXCEPTION: nav=%p", (void*)nav); return false; }
    return true;
}

// Hook Diagnostics has its own movement controller. Hook-owned Twins now mirror the
// proven normal Twin mover: reflected Mercuna MoveToActor retargeting plus standalone
// stop flushes, with the native hook layer registered for diagnostics only. Generic
// robots still use the protected AIController request path below.
struct HookNativeFollowState
{
    UObject* nav = nullptr;
    UObject* controller = nullptr;
    UObject* movementTarget = nullptr;
    FVector issuedGoal{};
    FVector sampleLocation{};
    ULONGLONG lastIssueMs = 0;
    ULONGLONG restartAtMs = 0;
    ULONGLONG sampleMs = 0;
    ULONGLONG lastProgressMs = 0;
    ULONGLONG lastTransitionMs = 0;
    ULONGLONG lastFocusClearMs = 0;
    ULONGLONG lastNativeMoveMs = 0;
    ULONGLONG lastRecoveryMs = 0;
    ULONGLONG lastMercunaMoveMs = 0;
    ULONGLONG lastMercunaFlushMs = 0;
    bool mercuna = false;
    bool mixed = false;
    bool moving = false;
    bool commandActive = false;
    bool restartPending = false;
    bool wasEngaged = false;
    bool mixedRecovery3D = false;
    bool velocityFallback = false;
    uint8_t navigationMode = 0;
    int followState = 0;
    float closeBestM = -1.0f;
    ULONGLONG closeBestMs = 0;
    ULONGLONG modeEnteredMs = 0;
    ULONGLONG mixedFallbackUntilMs = 0;
    bool traverse3D = false;
    uint32_t issues = 0;
    uint32_t restarts = 0;
    uint32_t directInputs = 0;
    uint32_t velocityWrites = 0;
    uint32_t recoveries = 0;
    uint32_t mercunaIssues = 0;
    uint32_t mercunaStops = 0;
    uint32_t groundPins = 0;
    uint32_t flightPins = 0;
};
static std::unordered_map<UObject*, HookNativeFollowState> g_hookNativeFollow;

static void SetHookControllerOwned(UObject* guard, bool owned)
{
    std::lock_guard<std::mutex> lock(g_hookControllerOwnedMutex);
    if (owned) g_hookControllerOwned.insert(guard);
    else g_hookControllerOwned.erase(guard);
}

static void YieldHookMovement(UObject* guard, HookNativeFollowState& s, bool stop)
{
    if (s.mercuna && s.nav)
    {
        if (stop && s.commandActive) MercunaStop(guard);
        AiMovementHooks::SetMercunaOwned(guard, s.nav, false);
    }
    else if (s.controller)
    {
        if (stop && s.commandActive)
        {
            if (s.mixed) StopHookControllerMovement(s.controller);
            else AiMovementHooks::ControllerStop(guard, s.controller);
        }
        AiMovementHooks::SetControllerOwned(guard, s.controller, false);
        SetHookControllerOwned(guard, false);
    }
    if (s.mixed) AiMovementHooks::SetMixedAutomatic(guard, true);
    if (s.velocityFallback)
    {
        WriteHookFollowVelocity(guard, FVector{}, 0.0f);
        s.velocityFallback = false;
    }
    s.commandActive = false;
    s.restartPending = false;
    s.moving = false;
    s.followState = 0;
    s.closeBestM = -1.0f;
    s.traverse3D = false;
}

static void ReleaseHookNativeMovement(UObject* guard, bool stop)
{
    auto it = g_hookNativeFollow.find(guard);
    if (it != g_hookNativeFollow.end())
    {
        YieldHookMovement(guard, it->second, stop);
        g_hookNativeFollow.erase(it);
    }
    else
    {
        SetHookControllerOwned(guard, false);
        if (IsMixedNavCharacter(guard)) AiMovementHooks::SetMixedAutomatic(guard, true);
    }
    AiMovementHooks::UnregisterGuard(guard);
}

static void ForgetHookBodyguardRuntimeState(UObject* guard, bool eraseReleaseBookkeeping)
{
    if (!guard) return;

    g_hookNativeFollow.erase(guard);
    g_inject.erase(guard);
    g_engagedUntilMs.erase(guard);
    g_lastThreatMs.erase(guard);
    g_twin.erase(guard);
    g_hookCombatRouteAbortPending.erase(guard);

    for (auto& pair : g_inject)
    {
        InjectState& s = pair.second;
        if (s.target == guard) s.target = nullptr;
        if (s.hitTarget == guard) s.hitTarget = nullptr;
        if (s.lastClearedTarget == guard) s.lastClearedTarget = nullptr;
    }
    for (auto& pair : g_hookNativeFollow)
    {
        HookNativeFollowState& s = pair.second;
        if (s.movementTarget == guard)
        {
            s.movementTarget = nullptr;
            s.commandActive = false;
            s.restartPending = true;
        }
    }

    if (eraseReleaseBookkeeping)
    {
        g_stashedSchedule.erase(guard);
        g_origTeam.erase(guard);
    }
    {
        std::lock_guard<std::mutex> lock(g_hookControllerOwnedMutex);
        g_hookControllerOwned.erase(guard);
    }
    AiMovementHooks::UnregisterGuard(guard);
}

static bool DriveHookCombatApproach(UObject* guard, HookNativeFollowState& s,
                                    UObject* target, ULONGLONG now)
{
    if (!IsLiveCombatTarget(target)) return false;
    FVector guardLoc{}, targetLoc{};
    if (!ReadActorLocationFast(guard, guardLoc) || !ReadActorLocationFast(target, targetLoc))
        return false;
    FVector delta{targetLoc.X-guardLoc.X,targetLoc.Y-guardLoc.Y,targetLoc.Z-guardLoc.Z};
    float distance = sqrtf(delta.X*delta.X+delta.Y*delta.Y+delta.Z*delta.Z);

    // At attack range, stop and release movement ownership. Native combat then
    // owns turning, root motion, ability movement and the attack montage.
    if (distance <= kHookFirstHitContactM*kUnitsPerMetre)
    {
        if (AiMovementHooks::OwnsGuard(guard) || (s.mixed && s.commandActive))
        {
            YieldHookMovement(guard,s,true);
            LOG("[AI-COMBAT] melee range reached; movement released to native abilities guard=%p target=%p dist=%.1fm",
                (void*)guard,(void*)target,distance/kUnitsPerMetre);
        }
        s.movementTarget=target;
        return true;
    }

    bool targetChanged=s.movementTarget!=target;
    if(targetChanged)
    {
        if(s.mercuna && s.nav && s.commandActive) MercunaStop(guard);
        else if(s.controller && s.commandActive)
        {
            if(s.mixed) StopHookControllerMovement(s.controller);
            else AiMovementHooks::ControllerStop(guard,s.controller);
        }
        s.commandActive=false; s.restartPending=true; s.restartAtMs=now+150;
        s.lastIssueMs=0; s.movementTarget=target;
        LOG("[AI-COMBAT] native approach acquired guard=%p target=%p dist=%.1fm mixed=%d",
            (void*)guard,(void*)target,distance/kUnitsPerMetre,s.mixed?1:0);
    }

    // Approach the enemy. Hook Twins route through the same generic AIController/Recast
    // stage that normal ground bodyguards use; generic robots keep the owned route.
    UObject* ctrl=GetAiController(guard);
    if(!ctrl) return false;

    if(s.mixed)
    {
        if(!ControllerPathFollowingReady(ctrl,guard)) return false;
        if(s.mercuna&&s.nav)
        {
            if(s.commandActive) MercunaStop(guard);
            AiMovementHooks::SetMercunaOwned(guard,s.nav,false);
        }
        s.mercuna=false;s.nav=nullptr;s.controller=ctrl;
        if(!AiMovementHooks::RegisterController(guard,ctrl)) return false;
        AiMovementHooks::SetControllerOwned(guard,ctrl,false);
        SetHookControllerOwned(guard,false);

        uint8_t mode=0;
        float speed=-1.0f;
        uint8_t* gb=reinterpret_cast<uint8_t*>(guard);
        if(Mem::IsReadable(gb+AH::Char_CharacterMovement,sizeof(void*)))
        {
            uint8_t* mv=*reinterpret_cast<uint8_t**>(gb+AH::Char_CharacterMovement);
            if(Mem::IsReadable(mv+AH::Move_MovementMode,1))mode=*reinterpret_cast<uint8_t*>(mv+AH::Move_MovementMode);
            if(Mem::IsReadable(mv+AH::Move_Velocity,sizeof(FVector)))
            { FVector v=*reinterpret_cast<FVector*>(mv+AH::Move_Velocity); speed=sqrtf(v.X*v.X+v.Y*v.Y+v.Z*v.Z); }
            if(Mem::IsReadable(mv+AH::Move_MaxWalkSpeed,sizeof(float)))
                *reinterpret_cast<float*>(mv+AH::Move_MaxWalkSpeed)=kHookTwinCombatWalkSpeed;
        }

        s.navigationMode=AiMovementHooks::CurrentMixedNavigation(guard);
        bool groundOk=false;
        if(targetChanged || s.navigationMode!=1 || mode==AH::MOVE_Flying)
        {
            ForceGroundNavIfMixed(guard);
            s.navigationMode=AiMovementHooks::CurrentMixedNavigation(guard);
            s.lastTransitionMs=now;
            ++s.groundPins;
            groundOk=true;
        }
        UnpinTwinSchedule(guard);
        bool forceOk=SetControllerForceFollow(ctrl,false);
        // Do not use AHAIController.SetBlackboardIsAggressive(false) here: that wrapper
        // broadcasts character events into GA_FightStaging_Twins. Write the BB key only.
        bool aggrOk=SetControllerBoolKeyAt(ctrl, AH::AICtrl_Key_IsAggressive, false);
        bool use3dOk=SetControllerBoolKeyAt(ctrl,AH::AICtrl_Key_Uses3DNavigation,false);
        bool can2dOk=SetControllerBoolKeyAt(ctrl,AH::AICtrl_Key_CanReach2D,true);
        bool can3dOk=SetControllerBoolKeyAt(ctrl,AH::AICtrl_Key_CanReach3D,false);
        ClearControllerFocus(ctrl);
        EnsureFollowerCanMove(guard);

        HookPathChainState chain{};
        bool chainOk=EnsureHookTwinGroundPathChain(guard,ctrl,targetChanged,chain);
        uint8_t status=DirectControllerMoveStatus(ctrl);
        bool routeIssued=false,moveAccepted=false;
        auto issueMove=[&]()
        {
            HookPathChainState issuedChain{};
            bool issuedChainOk=EnsureHookTwinGroundPathChain(guard,ctrl,true,issuedChain);
            if(issuedChain.pathFollower) chain=issuedChain;
            chainOk=issuedChainOk;
            moveAccepted=issuedChainOk && MoveControllerToActor(ctrl,guard,target,350.0f,true);
            routeIssued=true;
            s.commandActive=moveAccepted;
            s.restartPending=false;
            s.lastIssueMs=now;
            s.lastProgressMs=now;
            ++s.issues;
        };
        if(s.restartPending)
        {
            if(now>=s.restartAtMs) issueMove();
        }
        else if((!s.commandActive||status!=3)&&now-s.lastIssueMs>=500)
            issueMove();
        status=DirectControllerMoveStatus(ctrl);
        if(!chain.pathFollower)
            chainOk=EnsureHookTwinGroundPathChain(guard,ctrl,false,chain);
        float chainSpeed=speed;
        if(Mem::IsReadable(&chain.velocity,sizeof(FVector)))
            chainSpeed=sqrtf(chain.velocity.X*chain.velocity.X+chain.velocity.Y*chain.velocity.Y+chain.velocity.Z*chain.velocity.Z);

        static ULONGLONG lastCombatChainLog=0;
        if(now-lastCombatChainLog>=2000)
        {
            lastCombatChainLog=now;
            AiMovementHooks::Status hookDiag=AiMovementHooks::GetStatus();
            LOG("[AI-CHAIN] TwinCombatRecast guard=%p target=%p dist=%.1fm status=%u route=%d ret=%d ground=%d mode=%u navMode=%u vel=%.1f keys[force=%d aggr=%d use3d=%d can2d=%d can3d=%d] chain[pfc=%p move=%p cmc=%p nav=%p bind=%d repaired=%d active=%d mode=%u vel=%.1f] totals[issue=%u restart=%u ground=%u] hooks[direct=%llu/%llu faults=%llu path=%llu/%llu canStart=%llu/%llu]",
                (void*)guard,(void*)target,distance/kUnitsPerMetre,(unsigned)status,routeIssued?1:0,moveAccepted?1:0,
                groundOk?1:0,(unsigned)mode,(unsigned)s.navigationMode,speed,forceOk?1:0,aggrOk?1:0,use3dOk?1:0,
                can2dOk?1:0,can3dOk?1:0,(void*)chain.pathFollower,(void*)chain.boundMovement,(void*)chain.characterMovement,
                (void*)chain.navData,chainOk?1:0,chain.repairedBinding?1:0,chain.reactivated?1:0,(unsigned)chain.movementMode,
                chainSpeed,s.issues,s.restarts,s.groundPins,(unsigned long long)hookDiag.twinDirectGenericAccepted,
                (unsigned long long)hookDiag.twinGenericBypasses,(unsigned long long)hookDiag.controllerMoveFaults,
                (unsigned long long)hookDiag.pathRequests,(unsigned long long)hookDiag.pathAborts,
                (unsigned long long)hookDiag.movementCanStartCalls,(unsigned long long)hookDiag.movementCanStartForced);
        }
        return s.commandActive || status==3;
    }
    // Robots: owned native MoveToActor (protected from game override).
    if(!ControllerPathFollowingReady(ctrl,guard)) return false;
    if(!AiMovementHooks::RegisterController(guard,ctrl))
        return false;
    if(s.mercuna&&s.nav)
    { if(s.commandActive)AiMovementHooks::Stop(s.nav); AiMovementHooks::SetMercunaOwned(guard,s.nav,false);s.commandActive=false;s.restartPending=false; }
    s.mercuna=false;s.nav=nullptr;s.controller=ctrl;
    AiMovementHooks::SetControllerOwned(guard,ctrl,true);SetHookControllerOwned(guard,true);
    auto issueMove=[&]()
    {
        s.commandActive=AiMovementHooks::ControllerMoveToActor(guard,ctrl,target,350.0f);
        s.lastIssueMs=now; ++s.issues;
    };
    uint8_t status=DirectControllerMoveStatus(ctrl);
    if(s.restartPending){ if(now>=s.restartAtMs){ issueMove(); s.restartPending=false; } }
    else if((!s.commandActive||status!=3)&&now-s.lastIssueMs>=400) issueMove();
    return s.commandActive;
}

static void DriveHookBodyguardsNativeGameThread(UObject* player, const FVector& playerLoc, ULONGLONG now)
{
    std::vector<UObject*> guards = HookBodyguardSnapshot();
    if (guards.empty() || !g_hookBodyguardMode.load(std::memory_order_relaxed)) return;

    UFunction* moveLocationFn = CachedFn(AH::Fn_Mercuna_MoveToLocation);
    int active = 0, moving = 0, engagedCount = 0, mercuna = 0, controller = 0, stalledCount = 0, mixed3D = 0;
    float nearestM = -1.0f;

    for (UObject* guard : guards)
    {
        if (!AiUsable(guard) || guard == player)
        {
            ForgetHookBodyguardRuntimeState(guard, true);
            SquadRemove(guard);
            continue;
        }
        HookNativeFollowState& s = g_hookNativeFollow[guard];
        s.mixed = IsMixedNavCharacter(guard);

        if (g_hookCombatRouteAbortPending.erase(guard) > 0)
        {
            // Combat teardown is earlier than this native movement state type. Consume
            // its request here and kill the old enemy path before follow can fight it.
            YieldHookMovement(guard, s, true);
            s.wasEngaged = false;
            s.commandActive = false;
            s.restartPending = false;
            s.lastIssueMs = 0;
            s.sampleMs = 0;
            s.lastProgressMs = 0;
            s.movementTarget = nullptr;
            LOG("[AI-MOVE] aborted stale combat route before follow reacquire guard=%p", (void*)guard);
        }

        // Never retain the player or a corpse as a combat target while follow owns movement.
        uint8_t* base = reinterpret_cast<uint8_t*>(guard);
        if (Mem::IsReadable(base + AH::AICh_CachedTargetEnemy, sizeof(void*)) &&
            *reinterpret_cast<UObject**>(base + AH::AICh_CachedTargetEnemy) == player)
            *reinterpret_cast<UObject**>(base + AH::AICh_CachedTargetEnemy) = nullptr;
        InjectState& injection = g_inject[guard];
        UObject* activeTarget = injection.target ? injection.target : ReadAiTargetField(guard);
        if (activeTarget && activeTarget != player && !IsLiveCombatTarget(activeTarget))
            ClearHookBodyguardCombat(guard, player, "native follow dead-target check");

        auto engagedIt = g_engagedUntilMs.find(guard);
        bool engaged = engagedIt != g_engagedUntilMs.end() && engagedIt->second > now;
        if (engaged)
        {
            ++engagedCount;
            if (!s.wasEngaged)
            {
                YieldHookMovement(guard,s,true);
                s.lastIssueMs=0;s.movementTarget=nullptr;
            }
            s.wasEngaged = true;
            DriveHookCombatApproach(guard,s,injection.target?injection.target:ReadAiTargetField(guard),now);
            continue;
        }
        if (s.wasEngaged)
        {
            YieldHookMovement(guard,s,true); // never inherit an enemy-approach request into follow
            s.wasEngaged = false;
            s.lastIssueMs = 0;
            s.sampleMs = 0;
            s.navigationMode = AiMovementHooks::CurrentMixedNavigation(guard);
            s.modeEnteredMs = now;
            s.lastTransitionMs = now;
            s.movementTarget = nullptr;
            LOG("[AI-MOVE] combat released movement; native follow reacquiring guard=%p", (void*)guard);
        }

        FVector guardLoc{};
        if (!ReadActorLocationFast(guard, guardLoc)) continue;
        FVector delta{playerLoc.X-guardLoc.X, playerLoc.Y-guardLoc.Y, playerLoc.Z-guardLoc.Z};
        float planar = sqrtf(delta.X*delta.X + delta.Y*delta.Y);
        float distance = s.mixed ? sqrtf(planar*planar + delta.Z*delta.Z) : planar;
        float distanceM = distance / kUnitsPerMetre;
        if (nearestM < 0.0f || distanceM < nearestM) nearestM = distanceM;
        bool wasMoving = s.moving;
        s.moving = wasMoving ? distance > kHookFollowStopM*kUnitsPerMetre
                             : distance > kHookFollowStartM*kUnitsPerMetre;

        bool stalled = false;
        if (s.moving)
        {
            if (!s.sampleMs)
            {
                s.sampleLocation = guardLoc; s.sampleMs = now; s.lastProgressMs = now;
            }
            else if (now - s.sampleMs >= 500)
            {
                float dx=guardLoc.X-s.sampleLocation.X, dy=guardLoc.Y-s.sampleLocation.Y,
                      dz=s.mixed ? guardLoc.Z-s.sampleLocation.Z : 0.0f;
                float progressed = sqrtf(dx*dx + dy*dy + dz*dz);
                if (progressed >= 25.0f) s.lastProgressMs = now;
                stalled = s.commandActive && now - s.lastProgressMs >= 1800;
                s.sampleLocation = guardLoc; s.sampleMs = now;
            }
        }
        else { s.sampleMs = 0; s.lastProgressMs = 0; }
        if (stalled) ++stalledCount;

        // Hook Diagnostics resolves both native helper layers for instrumentation, but movement is
        // split by pawn type: Twins use the proven Mercuna actor-follow loop, generic robots use the
        // protected AIController/Recast route below.
        (void)AiMovementHooks::ResolveHelpers(moveLocationFn,
            s.mixed ? CachedObjectClassFn(guard, "ForceNavigationType") : nullptr);
        if (!s.mixed && s.mercuna && s.nav) // release stale Mercuna ownership before generic controller mode
        {
            if (s.commandActive) AiMovementHooks::Stop(s.nav);
            AiMovementHooks::SetMercunaOwned(guard, s.nav, false);
            s.mercuna = false; s.nav = nullptr; s.commandActive = false; s.restartPending = false;
        }

        // ---- TWIN (mixed-nav): controller/Recast ground follow through the generic +0x790 stage ----
        // ReVa confirms Twin ground locomotion is AIController + Recast + CharacterMovement. Mercuna
        // is kept out of normal follow so she walks with animation instead of gliding/flying.
        if (s.mixed)
        {
            UObject* ctrl = GetAiController(guard);
            if (!ControllerPathFollowingReady(ctrl, guard))
            { YieldHookMovement(guard, s, false); continue; }
            if (!AiMovementHooks::RegisterController(guard, ctrl))
            { YieldHookMovement(guard, s, false); continue; }
            if (s.mercuna && s.nav)
            {
                AiMovementHooks::SetMercunaOwned(guard, s.nav, false);
                s.mercuna = false;
                s.nav = nullptr;
            }
            AiMovementHooks::SetControllerOwned(guard, ctrl, false);
            SetHookControllerOwned(guard, false);
            s.controller = ctrl;
            s.mercuna = false;
            s.nav = nullptr;
            ++controller; ++active;

            uint8_t mode = 0;
            float moveSpeed = -1.0f;
            uint8_t* gb = reinterpret_cast<uint8_t*>(guard);
            uint8_t* mv = Mem::IsReadable(gb + AH::Char_CharacterMovement, sizeof(void*))
                ? *reinterpret_cast<uint8_t**>(gb + AH::Char_CharacterMovement) : nullptr;
            if (Mem::IsReadable(mv + AH::Move_MovementMode, 1))
                mode = *reinterpret_cast<uint8_t*>(mv + AH::Move_MovementMode);
            if (Mem::IsReadable(mv + AH::Move_Velocity, sizeof(FVector)))
            { FVector v = *reinterpret_cast<FVector*>(mv + AH::Move_Velocity); moveSpeed = sqrtf(v.X*v.X + v.Y*v.Y + v.Z*v.Z); }
            if (Mem::IsReadable(mv + AH::Move_MaxWalkSpeed, sizeof(float)))
                *reinterpret_cast<float*>(mv + AH::Move_MaxWalkSpeed) = kHookTwinPrettyWalkSpeed;
            if (mode == AH::MOVE_Flying) ++mixed3D;

            UnpinTwinSchedule(guard);
            s.navigationMode = AiMovementHooks::CurrentMixedNavigation(guard);
            bool groundOk = false;
            if (s.moving && (!wasMoving || s.followState != 1 || s.navigationMode != 1 || mode == AH::MOVE_Flying))
            {
                ForceGroundNavIfMixed(guard);
                s.navigationMode = AiMovementHooks::CurrentMixedNavigation(guard);
                s.lastTransitionMs = now;
                ++s.groundPins;
                groundOk = true;
            }

            const bool forceOk = SetControllerForceFollow(ctrl, s.moving);
            bool uses3DOk = SetControllerBoolKeyAt(ctrl, AH::AICtrl_Key_Uses3DNavigation, false);
            bool can2DOk = SetControllerBoolKeyAt(ctrl, AH::AICtrl_Key_CanReach2D, true);
            bool can3DOk = SetControllerBoolKeyAt(ctrl, AH::AICtrl_Key_CanReach3D, false);

            if (!s.moving)
            {
                bool stopped = false;
                if (s.commandActive)
                    stopped = StopHookControllerMovement(ctrl);
                bool aggrOk = SetControllerBoolKeyAt(ctrl, AH::AICtrl_Key_IsAggressive, false);
                ClearAiAggressiveLatch(guard);
                if (s.followState != 2)
                {
                    StopCharacterAggressive(guard);
                    FocusControllerOnActor(ctrl, player);
                    s.followState = 2;
                }
                if (s.navigationMode == 2 || mode == AH::MOVE_Flying)
                {
                    ForceGroundNavIfMixed(guard);
                    s.navigationMode = AiMovementHooks::CurrentMixedNavigation(guard);
                    ++s.groundPins;
                }
                s.commandActive = false;
                s.restartPending = false;
                s.closeBestM = -1.0f;
                s.sampleMs = 0;
                static ULONGLONG lastHoldLog = 0;
                if (now - lastHoldLog >= 3000)
                {
                    lastHoldLog = now;
                    uint8_t status = DirectControllerMoveStatus(ctrl);
                    LOG("[AI-MOVE] HookTwinRecast hold guard=%p dist=%.1fm status=%u stop=%d force=%d aggr=%d mode=%u navMode=%u vel=%.1f issues=%u restarts=%u ground=%u",
                        (void*)guard, distanceM, (unsigned)status, stopped?1:0, forceOk?1:0, aggrOk?1:0,
                        (unsigned)mode, (unsigned)s.navigationMode, moveSpeed, s.issues, s.restarts, s.groundPins);
                }
                continue;
            }

            ++moving;
            if (s.closeBestM < 0.0f || distanceM < s.closeBestM - 0.5f)
            {
                s.closeBestM = distanceM;
                s.closeBestMs = now;
                s.lastProgressMs = now;
            }

            float inv = distance > 1.0f ? 1.0f / distance : 0.0f;
            FVector goal{ playerLoc.X - delta.X*inv*kHookFollowStopM*kUnitsPerMetre,
                          playerLoc.Y - delta.Y*inv*kHookFollowStopM*kUnitsPerMetre,
                          playerLoc.Z - delta.Z*inv*kHookFollowStopM*kUnitsPerMetre };
            bool locationOk = SetControllerFollowLocation(ctrl, goal) |
                              SetControllerVectorKeyAt(ctrl, AH::AICtrl_Key_FollowLocation, goal);
            SetControllerVectorKeyAt(ctrl, AH::AICtrl_Key_CurrentWaypoint, goal);
            bool reachOk = SetControllerBoolKeyAt(ctrl, AH::AICtrl_Key_CanReachFollowLoc, true);
            // Follow is NOT combat. Keep the Twin out of the aggressive/fight-staging
            // branch while issuing normal follow/path requests. Use raw BB write only:
            // the reflected wrapper itself wakes GA_FightStaging_Twins every frame.
            bool aggrOk = SetControllerBoolKeyAt(ctrl, AH::AICtrl_Key_IsAggressive, false);
            bool speedOk = true;
            if (s.followState != 1)
            {
                WriteAiTargetField(guard, nullptr);
                // Do not call reflected SetTargetEnemy(nullptr) / SetBlackboardTargetEnemy(nullptr)
                // here for Twins: it ends GA_FightStaging_Twins and interrupts montages.
                SetControllerObjectKeyAt(ctrl, AH::AICtrl_Key_TargetEnemy, nullptr);
                ClearControllerFocus(ctrl);
                EnsureFollowerCanMove(guard);
                speedOk = SetControllerFollowSpeed(ctrl);
                s.followState = 1;
                s.restartPending = true;
                s.restartAtMs = now;
                s.lastIssueMs = 0;
            }

            HookPathChainState chain{};
            bool chainOk = EnsureHookTwinGroundPathChain(guard, ctrl, !s.commandActive || stalled, chain);
            uint8_t status = DirectControllerMoveStatus(ctrl);
            bool routeIssued = false, moveAccepted = false;
            if (stalled && now - s.lastIssueMs >= 1200)
            {
                StopHookControllerMovement(ctrl);
                s.commandActive = false;
                s.restartPending = true;
                s.restartAtMs = now + 150;
                ++s.restarts;
                ++s.recoveries;
                g_hookMovementRecoveries.fetch_add(1, std::memory_order_relaxed);
                LOG("[AI-MOVE] HookTwinRecast progress timeout -> stop/reissue guard=%p dist=%.1fm status=%u vel=%.1f restart=%u",
                    (void*)guard, distanceM, (unsigned)status, moveSpeed, s.restarts);
            }
            auto issueMove = [&]()
            {
                HookPathChainState issuedChain{};
                bool issuedChainOk = EnsureHookTwinGroundPathChain(guard, ctrl, true, issuedChain);
                if (issuedChain.pathFollower) chain = issuedChain;
                chainOk = issuedChainOk;
                moveAccepted = issuedChainOk && MoveControllerToActor(ctrl, guard, player, kHookFollowStopM*kUnitsPerMetre, true);
                routeIssued = true;
                s.commandActive = moveAccepted;
                s.restartPending = false;
                s.lastIssueMs = now;
                s.lastProgressMs = now;
                ++s.issues;
            };
            if (s.restartPending)
            {
                if (now >= s.restartAtMs) issueMove();
            }
            else if ((!s.commandActive || status != 3) && now - s.lastIssueMs >= 500)
                issueMove();
            status = DirectControllerMoveStatus(ctrl);
            if (status == 3) s.commandActive = true;
            if (!chain.pathFollower)
                chainOk = EnsureHookTwinGroundPathChain(guard, ctrl, false, chain);
            if (!wasMoving || now - s.lastFocusClearMs >= 1500)
            { ClearControllerFocus(ctrl); s.lastFocusClearMs = now; }

            static ULONGLONG lastTwinHookLog = 0;
            if (now - lastTwinHookLog >= 3000)
            {
                lastTwinHookLog = now;
                bool rbForce=false, rbReach=false, rbAggressive=false, rbUses3D=false, rbCan2D=false, rbCan3D=false;
                bool okRbForce=ReadControllerBoolKeyAt(ctrl, AH::AICtrl_Key_ForceFollowLoc, rbForce);
                bool okRbReach=ReadControllerBoolKeyAt(ctrl, AH::AICtrl_Key_CanReachFollowLoc, rbReach);
                bool okRbAggressive=ReadControllerBoolKeyAt(ctrl, AH::AICtrl_Key_IsAggressive, rbAggressive);
                bool okRbUses3D=ReadControllerBoolKeyAt(ctrl, AH::AICtrl_Key_Uses3DNavigation, rbUses3D);
                bool okRbCan2D=ReadControllerBoolKeyAt(ctrl, AH::AICtrl_Key_CanReach2D, rbCan2D);
                bool okRbCan3D=ReadControllerBoolKeyAt(ctrl, AH::AICtrl_Key_CanReach3D, rbCan3D);
                AiMovementHooks::Status hookDiag = AiMovementHooks::GetStatus();
                UObject* cachedTarget = ReadAiTargetField(guard);
                float chainSpeed = sqrtf(chain.velocity.X*chain.velocity.X + chain.velocity.Y*chain.velocity.Y + chain.velocity.Z*chain.velocity.Z);
                LOG("[AI-MOVE] HookTwinRecast(GT): guard=%p dist=%.1fm moving=1 status=%u route=%d ret=%d stalled=%d ground=%d mode=%u navMode=%u vel=%.1f keys[loc=%d force=%d reach=%d aggr=%d speed=%d use3d=%d can2d=%d can3d=%d] chain[pfc=%p move=%p cmc=%p nav=%p bind=%d repaired=%d active=%d mode=%u vel=%.1f] totals[issue=%u restart=%u recover=%u ground=%u] hooks[direct=%llu/%llu faults=%llu ctrl=%llu/%llu/%llu path=%llu/%llu canStart=%llu/%llu]",
                    (void*)guard, distanceM, (unsigned)status, routeIssued?1:0, moveAccepted?1:0, stalled?1:0,
                    groundOk?1:0, (unsigned)mode, (unsigned)s.navigationMode, moveSpeed, locationOk?1:0, forceOk?1:0,
                    reachOk?1:0, aggrOk?1:0, speedOk?1:0, uses3DOk?1:0, can2DOk?1:0, can3DOk?1:0,
                    (void*)chain.pathFollower, (void*)chain.boundMovement, (void*)chain.characterMovement, (void*)chain.navData,
                    chainOk?1:0, chain.repairedBinding?1:0, chain.reactivated?1:0, (unsigned)chain.movementMode, chainSpeed,
                    s.issues, s.restarts, s.recoveries, s.groundPins, (unsigned long long)hookDiag.twinDirectGenericAccepted,
                    (unsigned long long)hookDiag.twinGenericBypasses, (unsigned long long)hookDiag.controllerMoveFaults,
                    (unsigned long long)hookDiag.controllerMoveCalls, (unsigned long long)hookDiag.controllerPathCommits,
                    (unsigned long long)hookDiag.controllerPathBuilds, (unsigned long long)hookDiag.pathRequests,
                    (unsigned long long)hookDiag.pathAborts, (unsigned long long)hookDiag.movementCanStartCalls,
                    (unsigned long long)hookDiag.movementCanStartForced);
                LOG("[AI-MOVE] HookTwinRecastBB: guard=%p target=%p read[force=%d/%d reach=%d/%d aggr=%d/%d uses3d=%d/%d can2d=%d/%d can3d=%d/%d]",
                    (void*)guard,(void*)cachedTarget,okRbForce?1:0,rbForce?1:0,okRbReach?1:0,rbReach?1:0,
                    okRbAggressive?1:0,rbAggressive?1:0,okRbUses3D?1:0,rbUses3D?1:0,
                    okRbCan2D?1:0,rbCan2D?1:0,okRbCan3D?1:0,rbCan3D?1:0);
            }
            continue;
        }

        // ---- ROBOTS (generic AIController, clean +0x790 one-shot move): owned native MoveToActor ----
        UObject* ctrl = GetAiController(guard);
        if (!ControllerPathFollowingReady(ctrl, guard))
        {
            YieldHookMovement(guard, s, false);
            continue;
        }
        if (!AiMovementHooks::RegisterController(guard, ctrl))
        {
            YieldHookMovement(guard, s, false);
            continue; // fail closed: no unprotected controller request
        }
        ++controller; ++active;
        if (s.mercuna && s.nav)
        {
            if (s.commandActive) AiMovementHooks::Stop(s.nav);
            AiMovementHooks::SetMercunaOwned(guard, s.nav, false);
            s.commandActive=false; s.restartPending=false;
        }
        s.mercuna=false; s.nav=nullptr; s.controller=ctrl;
        AiMovementHooks::SetControllerOwned(guard,ctrl,true);
        SetHookControllerOwned(guard, true);
        if (!s.moving)
        {
            if (s.commandActive) AiMovementHooks::ControllerStop(guard, ctrl);
            s.commandActive=false; s.restartPending=false;
            if (wasMoving) FocusControllerOnActor(ctrl, player);
            continue;
        }
        ++moving;
        uint8_t status = DirectControllerMoveStatus(ctrl);
        if (s.restartPending)
        {
            if (now >= s.restartAtMs)
            {
                s.commandActive = AiMovementHooks::ControllerMoveToActor(guard, ctrl, player,
                                                                          kHookFollowStopM*kUnitsPerMetre);
                s.restartPending=false; s.lastIssueMs=now; ++s.issues; s.lastProgressMs=now;
            }
        }
        else if ((!s.commandActive || status != 3) && now-s.lastIssueMs>=500)
        {
            s.commandActive = AiMovementHooks::ControllerMoveToActor(guard, ctrl, player,
                                                                      kHookFollowStopM*kUnitsPerMetre);
            s.lastIssueMs=now; ++s.issues; s.lastProgressMs=now;
        }
        else if (stalled && now-s.lastIssueMs >= 1500)
        {
            AiMovementHooks::ControllerStop(guard, ctrl); s.commandActive=false; s.restartPending=true;
            s.restartAtMs=now+150; ++s.restarts;
            LOG("[AI-MOVE] controller progress timeout -> controlled stop/replan guard=%p dist=%.1fm status=%u", (void*)guard, distanceM, (unsigned)status);
        }
        if (!wasMoving || now-s.lastFocusClearMs>=1500)
        { ClearControllerFocus(ctrl); s.lastFocusClearMs=now; }
    }

    static ULONGLONG lastLog = 0;
    if (now-lastLog >= 3000)
    {
        lastLog=now;
        LOG("HookFollowNative(GT): active=%d moving=%d engaged=%d mercuna=%d controller=%d mixed3D=%d stalled=%d roster=%zu nearest=%.1fm",
            active,moving,engagedCount,mercuna,controller,mixed3D,stalledCount,guards.size(),nearestM);
    }
}

// Per-member nav re-issue timer (game-thread only): Mercuna MoveToActor auto-tracks the
// target, so we only re-issue it on a timer rather than every frame (no path-request spam).
// (The idle-schedule stash g_stashedSchedule lives up with the top-of-file squad globals
// so the release path, which is defined earlier, can restore it.)
static std::unordered_map<UObject*, unsigned long long> g_navReissueMs;

// =======================================================================
//  SQUAD FOLLOW  --  NATIVE ANIMATION + DIRECT CMC INPUT (game thread, per frame)
// -----------------------------------------------------------------------
//  No drag, no teleport, no velocity write. Every one of those produced the
//  "dogshit gliding" the user rejected. Instead we make the member's OWN behaviour
//  tree walk it to you, exactly like the game does when it follows an ally:
//
//   1) UNPIN the schedule. A companion like Larisa is pinned by an idle schedule
//      asset at AICh_Schedule (e.g. DA_AIScheduleLarisaNPC_Idle_NoOnteraction); while
//      that's set, her BT runs the schedule branch (which walks her to schedule points,
//      NOT to you -- the "she walks AWAY" we saw) and ignores the follow keys. We stash
//      it once (for restore) and null it every tick, so her BT falls through to the
//      follow branch.
//   2) DRIVE FollowLocation + CurrentWaypoint (never TargetAlly(player)). Hook Debug
//      guards also receive Pawn.AddMovementInput every frame, which uses Character
//      Movement and therefore preserves their normal walk/run animation even when
//      they have no Mercuna component.
//   3) Mercuna MoveToActor as a bonus for pawns that DO have a nav component (real
//      nav pawns/robots); a no-op for Larisa (she has none). Re-issued on a timer.
//
//  Result: she uses her own walk/run animation + obstacle avoidance to come to you.
//  Game-thread only (these are ProcessEvent dispatches). Crash-safe: AiUsable-gated,
//  IsReadable-guarded, every setter blackboard-gated, /EHa-wrapped caller.
// =======================================================================
//  TWIN FOLLOW  --  per-frame, game thread (the smooth, controlled locomotion)
// -----------------------------------------------------------------------
//  The "move" half of the Twin super-bodyguard, run every frame right after the normal
//  squad follow (which SKIPS Twins). It recreates the smoothness of her native follow:
//   * While she's FIGHTING (engagedUntilMs stamped by the brain) or AIRBORNE (movement_mode
//     5, mid ability/flight) we DON'T touch her -- her own combat AI moves + attacks +
//     lands cleanly. Yanking her here was half the jank, and re-pinning nav mid-flight was
//     the "flying bugs out".
//   * Otherwise we drive a STABLE ring-follow: pin to ground nav ONCE (so FollowLocation
//     works and she doesn't hover), and move her with hysteresis + a throttled goal so the
//     destination doesn't jitter every frame (that per-frame goal swing + ForceFollow toggle
//     was the start/stop/twitch). She faces her travel direction while moving (focus cleared)
//     and turns to watch you when she stops -- exactly like her native locomotion.
//  Game-thread only. Crash-safe: every read IsReadable-guarded, every setter blackboard-gated.
static void DriveTwinsFollowGameThread()
{
    using namespace UE;
    UObject* player = nullptr;
    FVector  playerLoc{};
    if (!ReadLocalPawnLocationFast(playerLoc, &player) || !Mem::IsReadable(player, 0x30))
        return;

    std::vector<UObject*> squad;
    { std::lock_guard<std::mutex> lk(g_squadMutex); squad = g_spawnedAllies; }

    float stopM = Features::Get().aiFollowStopM;
    if (stopM < 0.5f) stopM = 0.5f;
    if (stopM > 15.0f) stopM = 15.0f;
    const float stopU  = stopM * kUnitsPerMetre;
    const float startU = stopU + kTwinFollowBandM * kUnitsPerMetre; // hysteresis outer ring
    ULONGLONG now = GetTickCount64();

    float dbgDist = -1.0f, dbgVel = -1.0f; int dbgMoving = -1, dbgMode = -1, dbgMerc = -1, dbgMercCount = -1, dbgT3d = -1; const char* dbgSkip = "none";
    bool dbgAny = false;

    for (UObject* twin : squad)
    {
        if (!Mem::IsReadable(twin, 0x30) || twin == player) continue;
        if (g_hookBodyguardMode.load(std::memory_order_relaxed) && IsHookBodyguard(twin)) continue;
        if (!IsMixedNavCharacter(twin) || !FollowPawnUsable(twin)) continue;
        TwinState& s = g_twin[twin];
        dbgAny = true;

        // Never attack you, at the fast per-frame rate (raw, crash-safe).
        {
            uint8_t* gb = reinterpret_cast<uint8_t*>(twin);
            if (Mem::IsReadable(gb + AH::AICh_CachedTargetEnemy, sizeof(void*)) &&
                *reinterpret_cast<UObject**>(gb + AH::AICh_CachedTargetEnemy) == player)
                *reinterpret_cast<UObject**>(gb + AH::AICh_CachedTargetEnemy) = nullptr;
        }

        // Fighting -> leave her to her own combat AI (movement + montages + flight).
        if (s.engagedUntilMs > now) { s.followState = 0; dbgSkip = "engaged"; continue; }

        // Airborne / mid-flight -> do NOT drive a ground follow into her; wait until she lands.
        // (Re-pinning nav or overwriting move keys mid-air is what bugged her flight.)
        uint8_t* base = reinterpret_cast<uint8_t*>(twin);
        uint8_t* mv = Mem::IsReadable(base + AH::Char_CharacterMovement, sizeof(void*))
            ? *reinterpret_cast<uint8_t**>(base + AH::Char_CharacterMovement) : nullptr;
        if (Mem::IsReadable(mv + AH::Move_MovementMode, 1))
            dbgMode = *reinterpret_cast<uint8_t*>(mv + AH::Move_MovementMode);
        // If she's flying on her OWN (not our forced traversal), leave her be. While WE force
        // flight to cross stairs (traverse3D), keep driving her with Mercuna.
        if (dbgMode == AH::MOVE_Flying && !s.traverse3D)
        { s.followState = 0; dbgSkip = "flying"; continue; }
        if (Mem::IsReadable(mv + 0xE4, sizeof(FVector)))
        { FVector v = *reinterpret_cast<FVector*>(mv + 0xE4); dbgVel = sqrtf(v.X*v.X + v.Y*v.Y + v.Z*v.Z); }

        // (tick + brisk walk speed + GROUND-nav pin are maintained by the 5 Hz brain.)
        // UNPIN her idle schedule so the BT actually runs the follow branch -- without this she
        // runs the schedule branch and ignores the follow keys (moving=1 but vel=0).
        UnpinTwinSchedule(twin);

        FVector twinLoc{};
        if (!ReadActorLocationFast(twin, twinLoc)) continue;
        FVector to{ playerLoc.X - twinLoc.X, playerLoc.Y - twinLoc.Y, playerLoc.Z - twinLoc.Z };
        float dist = sqrtf(to.X * to.X + to.Y * to.Y + to.Z * to.Z);

        UObject* ctrl = GetAiController(twin);
        if (!ctrl) continue;
        // Hysteresis: start moving only when BEYOND the outer band, stop when inside the ring.
        // This stable band (not a per-frame "dist > ring" flip) kills the start/stop twitch.
        if (s.moving) { if (dist <= stopU)  s.moving = false; }
        else          { if (dist >  startU) s.moving = true;  }

        dbgDist = dist / kUnitsPerMetre; dbgMoving = s.moving ? 1 : 0; dbgSkip = "drive"; dbgMercCount = s.mercCount; dbgT3d = s.traverse3D ? 1 : 0;

        // ForceFollowLocation = constant within each state (hysteresis), so no toggle jank.
        SetControllerForceFollow(ctrl, s.moving);

        if (s.moving)
        {
            // STUCK / STAIRS: track whether she's CLOSING distance. If she stalls (vel~0) while
            // still far, the ground path can't reach you (down stairs / across a gap) -> FORCE
            // FLIGHT so Mercuna paths her through the air. Once she's caught up, ground her ONCE
            // (single re-pin on landing -- the REPEATED re-pin is what froze her animations).
            if (s.closeBestM < 0.0f || dbgDist < s.closeBestM - 0.5f)
            { s.closeBestM = dbgDist; s.closeBestMs = now; }
            bool stuck = (now - s.closeBestMs > 3000) && dbgDist > 10.0f && dbgVel >= 0.0f && dbgVel < 50.0f;
            if (stuck && !s.traverse3D)
            { s.traverse3D = true; ForceFlightNavIfMixed(twin); s.lastGoalMs = 0;
              LOG("TwinFollow: STUCK %.1fm vel=%.0f -> FORCE FLIGHT", dbgDist, dbgVel); }
            else if (s.traverse3D && dbgDist <= (stopM + kTwinFollowBandM))
            { s.traverse3D = false; ForceGroundNavIfMixed(twin); s.lastGoalMs = 0;   // ground ONCE on recovery
              LOG("TwinFollow: recovered %.1fm -> GROUND once", dbgDist); }

            // *** THE ACTUAL MOVER: Mercuna MoveToActor, re-targeted frequently. ***
            // The Twin (unlike regular robots) HAS a MercunaNavigationComponent; Mercuna is the
            // game's real pather. Frequent MoveToActor re-targeting is what moves her (a single
            // order doesn't sustain her). MoveToActor STACKS path requests over time (lag grows
            // linearly -> she stalls), so every few seconds FLUSH with a STANDALONE Stop on its
            // own pump -- NEVER in the same pump as a Move (Stop+Move together cancels the move
            // and she freezes, confirmed by the vel=0 logs). Between flushes, re-target on a timer.
            if (now - s.lastFlushMs > 4000)
            {
                MercunaStop(twin);          // flush accumulated path requests (own pump, no Move)
                s.lastFlushMs = now;
            }
            else if (now - s.lastGoalMs > 300)
            {
                dbgMerc = MercunaMoveToPlayer(twin, player, stopU, 600.0f) ? 1 : 0;
                s.lastGoalMs = now; ++s.mercCount;
            }
            // Belt-and-braces BT follow keys (harmless if her BT ignores them).
            float inv = dist > 1.0f ? 1.0f / dist : 0.0f;
            FVector goal{ playerLoc.X - to.X * inv * stopU,
                          playerLoc.Y - to.Y * inv * stopU,
                          playerLoc.Z - to.Z * inv * stopU };
            SetControllerFollowLocation(ctrl, goal);
            SetControllerVectorKeyAt(ctrl, AH::AICtrl_Key_FollowLocation, goal);
            SetControllerVectorKeyAt(ctrl, AH::AICtrl_Key_CurrentWaypoint, goal);
            SetControllerBoolKeyAt(ctrl, AH::AICtrl_Key_CanReachFollowLoc, true);
            if (s.followState != 1)
            {
                ClearControllerFocus(ctrl);   // face travel direction while moving (natural)
                SetControllerFollowSpeed(ctrl);
                s.followState = 1;
            }
        }
        else
        {
            // Within the ring -> STOP her Mercuna move (she should actually stop here) and watch
            // you (guard stance). Set once on the transition. If she arrived while force-flying,
            // ground her ONCE so she lands.
            if (s.traverse3D)
            { s.traverse3D = false; ForceGroundNavIfMixed(twin); LOG("TwinFollow: reached you while flying -> GROUND once"); }
            if (s.followState != 2)
            {
                MercunaStop(twin);
                FocusControllerOnActor(ctrl, player);
                s.followState = 2;
            }
            s.closeBestM = -1.0f;
        }
    }

    if (dbgAny)
    {
        static ULONGLONG lastFollowLog = 0;
        if (now - lastFollowLog > 3000)
        {
            lastFollowLog = now;
            LOG("TwinFollow: skip=%s dist=%.1fm moving=%d mode=%d vel=%.0f merc=%d issues=%d t3d=%d", dbgSkip, dbgDist, dbgMoving, dbgMode, dbgVel, dbgMerc, dbgMercCount, dbgT3d);
        }
    }
}

void DriveSquadVelocityGameThread()
{
    using namespace UE;
    if (g_spawnedAllyCount.load() <= 0)
        return;

    UObject* player = nullptr;
    FVector playerLoc{};
    if (!ReadLocalPawnLocationFast(playerLoc, &player) || !Mem::IsReadable(player, 0x30))
        return;

    float stopM = Features::Get().aiFollowStopM;
    if (stopM < 0.5f) stopM = 0.5f;
    if (stopM > 15.0f) stopM = 15.0f;
    const float stopU = stopM * kUnitsPerMetre;

    std::vector<UObject*> squad;
    { std::lock_guard<std::mutex> lk(g_squadMutex); squad = g_spawnedAllies; }

    int followed = 0, mercuna = 0, unpinned = 0, directMoves = 0, velocityMoves = 0;
    int hookMoving = 0, recoveries = 0;
    float nearest = -1.0f, nearVel = -1.0f;
    std::string nearSched = "?";
    ULONGLONG nowm = GetTickCount64();

    // Dedicated Hook Diagnostics roster is driven first by its native ownership
    // controller and is skipped completely by the legacy squad/Twin paths below.
    DriveHookBodyguardsNativeGameThread(player, playerLoc, nowm);

    for (UObject* ai : squad)
    {
        if (!Mem::IsReadable(ai, 0x30) || ai == player) continue;
        if (!AiUsable(ai)) continue; // AHAICharacter
        const bool hookOwned = g_hookBodyguardMode.load(std::memory_order_relaxed) &&
                               IsHookBodyguard(ai);
        if (hookOwned) continue; // never share SDK/blackboard/input/velocity movement with Hook Bodyguards
        if (IsMixedNavCharacter(ai)) continue; // non-hook Twins use their legacy dedicated path

        // *** HARD never-attack-you guard, at the FAST 20Hz rate (the 5Hz pump alone
        // wasn't fast enough -- a guard you shot got a few hits in first). If its cached
        // aggro target is YOU, blank it immediately (raw write, crash-safe, no dispatch).
        // Runs for EVERY member regardless of engagement, so it always wins. ***
        {
            uint8_t* gb = reinterpret_cast<uint8_t*>(ai);
            if (Mem::IsReadable(gb + AH::AICh_CachedTargetEnemy, sizeof(void*)) &&
                *reinterpret_cast<UObject**>(gb + AH::AICh_CachedTargetEnemy) == player)
                *reinterpret_cast<UObject**>(gb + AH::AICh_CachedTargetEnemy) = nullptr;
        }

        // Hook Debug does not wait for the 5 Hz selector to notice a corpse. Tear down
        // a dead/unreadable target immediately so the next lines resume follow in this
        // same frame instead of attacking the old location behind the guard.
        if (hookOwned)
        {
            InjectState& hs = g_inject[ai];
            UObject* cached = ReadAiTargetField(ai);
            UObject* active = hs.target ? hs.target : cached;
            if (active && active != player && !IsLiveCombatTarget(active))
                ClearHookBodyguardCombat(ai, player, "fast dead-target check");
        }

        // A guard that's actively fighting a threat (stamped by the bodyguard injection)
        // is LEFT ALONE here -- otherwise we'd yank its FollowLocation back to you every
        // 50ms and it would never close on the enemy (the "recruited robot stalls then
        // follows once combat ends" behaviour). When the fight ends, the stamp lapses and
        // it resumes following on the next tick.
        {
            auto eit = g_engagedUntilMs.find(ai);
            if (eit != g_engagedUntilMs.end() && eit->second > nowm) continue;
        }

        FVector aiLoc{};
        if (!ReadActorLocationFast(ai, aiLoc)) continue;
        FVector to{ playerLoc.X - aiLoc.X, playerLoc.Y - aiLoc.Y, playerLoc.Z - aiLoc.Z };
        float dist = sqrtf(to.X * to.X + to.Y * to.Y + to.Z * to.Z);
        float planarDist = sqrtf(to.X * to.X + to.Y * to.Y);
        InjectState& followState = g_inject[ai];
        const float memberStopU = (hookOwned ? kHookFollowStopM : stopM) * kUnitsPerMetre;
        const float memberStartU = (hookOwned ? kHookFollowStartM : stopM) * kUnitsPerMetre;
        bool wasMoving = followState.followMoving;
        bool needsToMove = hookOwned
            ? (wasMoving ? planarDist > memberStopU : planarDist > memberStartU)
            : planarDist > memberStopU;
        followState.followMoving = needsToMove;
        if (hookOwned && needsToMove) ++hookMoving;

        // Measure real actor displacement. AddMovementInput returning successfully only
        // means the function ran; it does not prove CharacterMovement consumed it.
        bool hardRecover = false;
        if (hookOwned && needsToMove)
        {
            if (!followState.followSampleMs)
            {
                followState.followSampleLoc = aiLoc;
                followState.followSampleMs = nowm;
                followState.followLastProgressMs = nowm;
            }
            else if (nowm - followState.followSampleMs >= 500)
            {
                float dx = aiLoc.X - followState.followSampleLoc.X;
                float dy = aiLoc.Y - followState.followSampleLoc.Y;
                float movedU = sqrtf(dx * dx + dy * dy);
                if (movedU >= 20.0f)
                    followState.followLastProgressMs = nowm;
                else if (nowm - followState.followLastProgressMs >= 1500 &&
                         nowm - followState.lastMoveRecoveryMs >= 2000)
                    hardRecover = true;
                followState.followSampleLoc = aiLoc;
                followState.followSampleMs = nowm;
            }
        }
        else if (hookOwned)
        {
            followState.followSampleMs = 0;
            followState.followLastProgressMs = 0;
        }

        uint8_t* base = reinterpret_cast<uint8_t*>(ai);

        // Diagnostics for the nearest member: real CMC velocity + active schedule name.
        // vel>0 + nearest shrinking = her own AI is walking her in (working). sched name
        // still "...Idle..." + vel=0 = the schedule re-pinned her (need a different unpin).
        if (nearest < 0.0f || dist < nearest)
        {
            nearest = dist;
            uint8_t* cmc = Mem::IsReadable(base + AH::Char_CharacterMovement, sizeof(void*))
                ? *reinterpret_cast<uint8_t**>(base + AH::Char_CharacterMovement) : nullptr;
            if (Mem::IsReadable(cmc, 0xF0))
            {
                FVector v = *reinterpret_cast<FVector*>(cmc + 0xE4); // UMovementComponent::Velocity
                nearVel = sqrtf(v.X * v.X + v.Y * v.Y + v.Z * v.Z);
            }
            if (Mem::IsReadable(base + AH::AICh_Schedule, sizeof(void*)))
            {
                UObject* sc = *reinterpret_cast<UObject**>(base + AH::AICh_Schedule);
                nearSched = sc ? SafeObjectName(sc) : std::string("<null>");
            }
            else nearSched = "<unreadable>";
        }

        // (LAYER 0) never let a follower sit parked passive/idle (crash-safe raw write).
        if (Mem::IsReadable(base + AH::AICh_bIsPassive, 1))
            *reinterpret_cast<bool*>(base + AH::AICh_bIsPassive) = false;

        // (LAYER 1) UNPIN: clear the idle schedule so the BT falls through to its follow
        // branch. Stash the original ONCE for restore, then null it every tick (the
        // schedule system re-applies the asset, so a one-shot null gets overwritten).
        if (Mem::IsReadable(base + AH::AICh_Schedule, sizeof(void*)))
        {
            UObject** schedSlot = reinterpret_cast<UObject**>(base + AH::AICh_Schedule);
            if (*schedSlot)
            {
                auto its = g_stashedSchedule.find(ai);
                if (its == g_stashedSchedule.end())
                    g_stashedSchedule[ai] = *schedSlot; // remember the real schedule once
                *schedSlot = nullptr;
                ++unpinned;
            }
        }

        // (LAYER 2) DRIVE the follow keys her BT reads -> her own locomotion walks her in.
        // Aim her at a point on the stop ring (stopU from you, nearest her), NOT at your
        // exact spot -- otherwise she always tries to close the final metre and keeps
        // pressing into you. Once she's inside the ring, point the destination at where
        // she stands so the BT considers her "arrived" and she just holds.
        UObject* ctrl = GetAiController(ai);
        if (ctrl)
        {
            FVector goal;
            if (needsToMove)
            {
                float inv = planarDist > 1.0f ? 1.0f / planarDist : 0.0f;
                FVector dir{ to.X * inv, to.Y * inv, 0.0f };
                goal = { playerLoc.X - dir.X * memberStopU, playerLoc.Y - dir.Y * memberStopU, playerLoc.Z };
            }
            else
            {
                goal = aiLoc; // already within the ring -> destination == her position -> stop
            }
            bool refreshGoal = !hookOwned || wasMoving != needsToMove ||
                               nowm - followState.lastFollowGoalMs >= 250;
            if (refreshGoal)
            {
                followState.lastFollowGoalMs = nowm;
                SetControllerFollowLocation(ctrl, goal);
                SetControllerVectorKeyAt(ctrl, AH::AICtrl_Key_FollowLocation, goal);
                SetControllerVectorKeyAt(ctrl, AH::AICtrl_Key_CurrentWaypoint, goal);
                SetControllerForceFollow(ctrl, needsToMove);
                SetControllerBoolKeyAt(ctrl, AH::AICtrl_Key_CanReachFollowLoc, true);
                SetControllerFollowSpeed(ctrl);
                if (hookOwned && needsToMove)
                    ClearControllerFocus(ctrl); // face velocity, not backwards toward the player
                else
                    FocusControllerOnActor(ctrl, player);
            }

            // Start one direct, non-pathfinding controller request. Do not restart an
            // active request; if measured displacement stalls, StopMovement + recovery
            // below deliberately replaces it.
            if (hookOwned && needsToMove && nowm - followState.lastMoveMs >= 750)
            {
                uint8_t moveStatus = ControllerMoveStatus(ctrl);
                bool requestNeeded = !wasMoving || moveStatus != 3;
                if (requestNeeded)
                {
                    followState.lastMoveMs = nowm;
                    MoveControllerToActor(ctrl, ai, player, memberStopU);
                }
            }
            ++followed;
        }

        // Hook Debug fallback/primary mover: this enters CharacterMovement through the
        // engine's own AddMovementInput path, so animation remains native. It is driven
        // every game frame and cannot go idle merely because the BT ignored follow keys.
        if (hookOwned && needsToMove && planarDist > 1.0f)
        {
            FVector dir{ to.X / planarDist, to.Y / planarDist, 0.0f };
            if (AddPawnMovementInput(ai, dir, 1.0f))
            {
                ++directMoves;
                g_hookDirectMoveInputs.fetch_add(1, std::memory_order_relaxed);
            }
            if (hardRecover)
            {
                followState.lastMoveRecoveryMs = nowm;
                followState.followLastProgressMs = nowm;
                UObject* ctrl = GetAiController(ai);
                RecoverHookFollowerMovement(ai, ctrl);
                if (ctrl) MoveControllerToActor(ctrl, ai, player, memberStopU);
                if (!followState.velocityFallback)
                {
                    followState.velocityFallback = true;
                    g_hookVelocityFallbacks.fetch_add(1, std::memory_order_relaxed);
                }
                ++followState.movementRecoveries;
                ++recoveries;
                uint64_t total = g_hookMovementRecoveries.fetch_add(1, std::memory_order_relaxed) + 1;
                LOG("[AI-FOLLOW] zero displacement recovery: guard=%p dist=%.1fm recoveries=%u total=%llu -> walking + velocity fallback",
                    (void*)ai, planarDist / kUnitsPerMetre, followState.movementRecoveries,
                    (unsigned long long)total);
            }
            if (followState.velocityFallback && WriteHookFollowVelocity(ai, dir, 450.0f))
            {
                ++velocityMoves;
            }
        }
        else if (hookOwned && followState.velocityFallback)
        {
            WriteHookFollowVelocity(ai, FVector{}, 0.0f); // stop cleanly at the 2 m ring
        }

        // (LAYER 3) Mercuna nav bonus for pawns that have a nav component.
        if (planarDist > memberStopU)
        {
            ULONGLONG& last = g_navReissueMs[ai];
            if (nowm - last > 900) { last = nowm; if (MercunaMoveToPlayer(ai, player, memberStopU, 600.0f)) ++mercuna; }
        }
    }

    // Prune dead members from the per-member maps so they can't grow unbounded.
    if (g_navReissueMs.size() > 64)
        for (auto it = g_navReissueMs.begin(); it != g_navReissueMs.end(); )
        { if (Mem::IsReadable(it->first, 0x30)) ++it; else it = g_navReissueMs.erase(it); }
    if (g_stashedSchedule.size() > 64)
        for (auto it = g_stashedSchedule.begin(); it != g_stashedSchedule.end(); )
        { if (Mem::IsReadable(it->first, 0x30)) ++it; else it = g_stashedSchedule.erase(it); }
    if (g_engagedUntilMs.size() > 64)
        for (auto it = g_engagedUntilMs.begin(); it != g_engagedUntilMs.end(); )
        { if (Mem::IsReadable(it->first, 0x30)) ++it; else it = g_engagedUntilMs.erase(it); }
    if (g_lastThreatMs.size() > 64)
        for (auto it = g_lastThreatMs.begin(); it != g_lastThreatMs.end(); )
        { if (Mem::IsReadable(it->first, 0x30)) ++it; else it = g_lastThreatMs.erase(it); }

    static ULONGLONG lastLog = 0;
    if (nowm - lastLog > 3000)
    {
        lastLog = nowm;
        LOG("SquadFollow(GT): followed=%d hookMoving=%d input=%d velocity=%d recoveries=%d mercuna=%d unpinned=%d /%zu, nearest=%.1fm vel=%.0f sched=%s",
            followed, hookMoving, directMoves, velocityMoves, recoveries, mercuna, unpinned, squad.size(),
            nearest >= 0.0f ? nearest / kUnitsPerMetre : -1.0f, nearVel, nearSched.c_str());
    }

    // The Twin follows on her OWN dedicated, smooth path (the normal loop above skips her).
    DriveTwinsFollowGameThread();
}

static void TickImpl()
{
    using namespace UE;
    auto& st = Features::Get();

    static ULONGLONG lastTickMs = 0;
    ULONGLONG nowMs = GetTickCount64();
    float dt = lastTickMs ? (float)(nowMs - lastTickMs) / 1000.0f : (1.0f / 60.0f);
    lastTickMs = nowMs;
    dt = ClampDeltaSeconds(dt);

    UpdateGameInputBlock();
    UpdateInstantPuzzleResolveToggle();
    UpdateDebugDiagnostics();
    UpdateHookTwinForensics();
    // Fire any puzzle completions the worker thread discovered (render thread).
    DrainPuzzleCompletions();
    // World-level toggles that don't need the pawn.
    ApplyTimeDilation();
    // AI commands auto-fire onto the cache (no ProcessEvent here -- safe on the
    // render thread); the actual AI ProcessEvent work is marshalled to the game
    // thread by the scheduler below to avoid racing the engine's AI tick.
    DrainDeferredAiCommand();
    ScheduleAiGameThreadWork();
    // (Squad walk is driven on the GAME thread from hkProcessEvent -- the render thread
    //  runs a frame behind, so RequestedVelocity written here would never be consumed.)
    // Render-hijack visuals (chams / world tint) are marshalled to the game
    // thread too -- independent of the pawn, so schedule before the pawn gate.
    ScheduleVisualGameThreadWork();

    if (!HasPawnFeatureWork(st))
        return;

    static UObject* lastPawn = nullptr;
    static bool loggedNoPawn = false;

    UObject* pawn = GetLocalPawn();
    if (!pawn)
    {
        if (!loggedNoPawn)
        {
            LOG("Feature tick: no local pawn yet (PlayerController=%p)", (void*)GetPlayerController());
            loggedNoPawn = true;
            lastPawn = nullptr;
        }
        return;
    }
    if (pawn != lastPawn)
    {
        std::string pawnName;
        try { pawnName = pawn->GetName(); }
        catch (...) {}
        LOG("Feature tick: local pawn=%p name=%s", (void*)pawn, pawnName.c_str());
        loggedNoPawn = false;
        lastPawn = pawn;
    }
    uint8_t* p = reinterpret_cast<uint8_t*>(pawn);

    ProcessGiveAllQueue();

    // --- coordinate readout -------------------------------------------------
    FVector currentLoc{};
    bool needLoc = st.showCoords || st.flyHack || st.noclip;
    bool haveLoc = needLoc && ReadActorLocation(pawn, currentLoc);
    if (haveLoc)
    {
        g_lastLoc = currentLoc;
    }

    // --- attribute-set features (god / one-hit / stamina / energy / air) ----
    uint8_t* set = nullptr;
    if (Mem::IsReadable(p + AH::Char_AttributeSet, 8))
        set = *reinterpret_cast<uint8_t**>(p + AH::Char_AttributeSet);

    static bool wasGodMode = false;
    static bool wasOneHitKill = false;
    static bool wasInfiniteStamina = false;
    static bool wasInfiniteEnergy = false;
    static bool wasInfiniteAir = false;
    if (wasGodMode && !st.godMode)
    {
        RestoreAttrBackup(g_incomingDamageBackup, "God mode incoming damage");
        RestoreAttrBackup(g_healthBackup, "God mode health");
    }
    if (wasOneHitKill && !st.oneHitKill)
        RestoreAttrBackup(g_instigatedDamageBackup, "One-hit outgoing damage");
    if (wasInfiniteStamina && !st.infiniteStamina)
        RestoreAttrBackup(g_staminaBackup, "Infinite stamina");
    if (wasInfiniteEnergy && !st.infiniteEnergy)
        RestoreAttrBackup(g_energyBackup, "Infinite energy");
    if (wasInfiniteAir && !st.infiniteAir)
        RestoreAttrBackup(g_airBackup, "Infinite air");

    if (Mem::IsReadable(set, AH::Set_InstigatedDmgMult + 0x10))
    {
        if (!st.godMode)
            NormalizeDisabledDamageAttr(set, AH::Set_IncomingDamageMult, "God mode incoming damage", false);
        if (!st.oneHitKill)
            NormalizeDisabledDamageAttr(set, AH::Set_InstigatedDmgMult, "One-hit outgoing damage", true);

        if (st.godMode)
        {
            CaptureDamageAttrBackup(g_incomingDamageBackup, set, AH::Set_IncomingDamageMult, "God mode incoming damage", false);
            CaptureAttrBackup(g_healthBackup, set, AH::Set_Health, "God mode health");
            SetAttr(set, AH::Set_IncomingDamageMult, kGodDamageMultiplier); // take no damage
            SetAttr(set, AH::Set_Health, GetAttrCur(set, AH::Set_MaxHealth)); // stay topped
            if (Mem::IsReadable(p + AH::Char_bIsDead, 1))
                *reinterpret_cast<bool*>(p + AH::Char_bIsDead) = false;
        }
        if (st.oneHitKill)
        {
            CaptureDamageAttrBackup(g_instigatedDamageBackup, set, AH::Set_InstigatedDmgMult, "One-hit outgoing damage", true);
            SetAttr(set, AH::Set_InstigatedDmgMult, kOneHitDamageMultiplier); // massive outgoing dmg
        }

        if (st.infiniteStamina)
        {
            CaptureAttrBackup(g_staminaBackup, set, AH::Set_Stamina, "Infinite stamina");
            SetAttr(set, AH::Set_Stamina, GetAttrCur(set, AH::Set_MaxStamina));
        }
        if (st.infiniteEnergy)
        {
            CaptureAttrBackup(g_energyBackup, set, AH::Set_Energy, "Infinite energy");
            SetAttr(set, AH::Set_Energy,  GetAttrCur(set, AH::Set_MaxEnergy));
        }
        if (st.infiniteAir)
        {
            CaptureAttrBackup(g_airBackup, set, AH::Set_Air, "Infinite air");
            SetAttr(set, AH::Set_Air,     GetAttrCur(set, AH::Set_MaxAir));
        }
    }
    wasGodMode = st.godMode;
    wasOneHitKill = st.oneHitKill;
    wasInfiniteStamina = st.infiniteStamina;
    wasInfiniteEnergy = st.infiniteEnergy;
    wasInfiniteAir = st.infiniteAir;

    // --- movement: fly / noclip + speed ------------------------------------
    // Noclip == free-fly + actor collision off, so both share the fly movement
    // and the flying movement-mode / max-fly-speed writes below.
    bool freeFly = st.flyHack || st.noclip;
    static bool wasFreeFly = false;
    static bool wasSpeedHack = false;
    if (freeFly && !wasFreeFly)
    {
        if (haveLoc)
        {
            st.flyStartLocation = currentLoc;
            st.hasFlyStart = true;
            LOG("Fly start captured %.1f %.1f %.1f", currentLoc.X, currentLoc.Y, currentLoc.Z);
        }
        RefreshFlyStreaming(pawn, true);
    }
    else if (!freeFly && wasFreeFly)
    {
        RefreshFlyStreaming(pawn, true);
        LOG("Fly/noclip disabled; streaming assist refreshed at current pawn location.");
    }

    // Noclip toggles the pawn's collision (edge-triggered, and re-applied if the
    // pawn is swapped out by a death/reload while noclip stays on).
    static UObject* noclipColPawn = nullptr;
    if (st.noclip)
    {
        if (pawn != noclipColPawn)
        {
            SetActorCollisionEnabled(pawn, false);
            noclipColPawn = pawn;
        }
    }
    else if (noclipColPawn)
    {
        SetActorCollisionEnabled(pawn, true);
        noclipColPawn = nullptr;
    }

    if (wasSpeedHack && !st.speedHack)
        RestoreMovementWalk();
    if (wasFreeFly && !freeFly)
        RestoreMovementFly();

    if (Mem::IsReadable(p + AH::Char_CharacterMovement, 8))
    {
        uint8_t* mv = *reinterpret_cast<uint8_t**>(p + AH::Char_CharacterMovement);
        if (Mem::IsReadable(mv, AH::Move_MaxFlySpeed + 4))
        {
            CaptureMovementBackup(mv, st.speedHack, freeFly, freeFly);

            if (st.speedHack)
                *reinterpret_cast<float*>(mv + AH::Move_MaxWalkSpeed) = 600.0f * st.speedMult;

            if (freeFly)
            {
                *reinterpret_cast<uint8_t*>(mv + AH::Move_MovementMode) = AH::MOVE_Flying;
                *reinterpret_cast<float*>(mv + AH::Move_MaxFlySpeed) = 600.0f * st.speedMult;
                if (haveLoc)
                    ApplyMinecraftFly(pawn, currentLoc, dt);
            }

            // super jump / low gravity: capture the original on first enable so
            // disabling restores the real value (offsets all < Move_MaxFlySpeed,
            // so the readable check above already covers them).
            {
                static bool jumpCap = false; static float jumpOrig = 0.0f;
                float* jz = reinterpret_cast<float*>(mv + AH::Move_JumpZVelocity);
                if (st.superJump) { if (!jumpCap) { jumpOrig = *jz; jumpCap = true; } *jz = jumpOrig * st.jumpMult; }
                else if (jumpCap) { *jz = jumpOrig; jumpCap = false; }

                static bool gravCap = false; static float gravOrig = 0.0f;
                float* gv = reinterpret_cast<float*>(mv + AH::Move_GravityScale);
                if (st.lowGravity) { if (!gravCap) { gravOrig = *gv; gravCap = true; } *gv = gravOrig * st.gravityScale; }
                else if (gravCap) { *gv = gravOrig; gravCap = false; }
            }
        }
    }
    wasFreeFly = freeFly;
    wasSpeedHack = st.speedHack;

    // --- camera FOV override ------------------------------------------------
    // Write the active camera component's FieldOfView each frame (FP + TP), so
    // the next game tick computes the view with our stretched FOV. Capture the
    // original on the enable edge and restore it on disable. Pure guarded writes.
    {
        auto camFovPtr = [&](int camOffset) -> float*
        {
            if (!Mem::IsReadable(p + camOffset, sizeof(void*)))
                return nullptr;
            uint8_t* cam = *reinterpret_cast<uint8_t**>(p + camOffset);
            if (!Mem::IsReadable(cam + AH::Camera_FieldOfView, sizeof(float)))
                return nullptr;
            return reinterpret_cast<float*>(cam + AH::Camera_FieldOfView);
        };

        if (st.customFov)
        {
            float fov = st.fovValue;
            if (fov < 5.0f) fov = 5.0f;
            if (fov > 170.0f) fov = 170.0f;
            if (float* fp = camFovPtr(AH::Char_FPCamera)) { if (!g_fovBackup.fpValid) { g_fovBackup.fp = *fp; g_fovBackup.fpValid = true; } *fp = fov; }
            if (float* tp = camFovPtr(AH::Char_TPCamera)) { if (!g_fovBackup.tpValid) { g_fovBackup.tp = *tp; g_fovBackup.tpValid = true; } *tp = fov; }
            g_fovBackup.active = true;
        }
        else if (g_fovBackup.active)
        {
            if (g_fovBackup.fpValid) { if (float* fp = camFovPtr(AH::Char_FPCamera)) *fp = g_fovBackup.fp; }
            if (g_fovBackup.tpValid) { if (float* tp = camFovPtr(AH::Char_TPCamera)) *tp = g_fovBackup.tp; }
            g_fovBackup = {};
        }
    }

    // --- bullet time (matrix mode): slow the whole world, keep the player fast --
    // Global time dilation slows everything; the player's own CustomTimeDilation
    // is set to 1/scale so the player effectively runs at real-time in the slowed
    // world -- i.e. you move many times faster than everything else.
    {
        static bool wasBullet = false;
        static ULONGLONG lastGlobalMs = 0;
        if (st.bulletTime)
        {
            float s = st.bulletTimeScale;
            if (s < 0.05f) s = 0.05f;
            if (s > 1.0f)  s = 1.0f;
            ULONGLONG nowB = GetTickCount64();
            if (!wasBullet || nowB - lastGlobalMs > 300) { SetTimeDilation(s); lastGlobalMs = nowB; }
            SetActorTimeDilation(pawn, 1.0f / s); // counter the global slow for the player only
        }
        else if (wasBullet)
        {
            SetTimeDilation(1.0f);
            SetActorTimeDilation(pawn, 1.0f);
            LOG("Bullet time off");
        }
        wasBullet = st.bulletTime;
    }

    // --- player scale (giant / tiny) ---------------------------------------
    {
        static bool wasScale = false;
        static float lastScale = -1.0f;
        if (st.customScale)
        {
            float s = st.playerScale;
            if (s < 0.1f) s = 0.1f;
            if (s > 10.0f) s = 10.0f;
            if (s != lastScale) { SetActorScale3D(pawn, { s, s, s }); lastScale = s; LOG("Player scale -> %.2f", s); }
        }
        else if (wasScale)
        {
            SetActorScale3D(pawn, { 1.0f, 1.0f, 1.0f });
            lastScale = -1.0f;
            LOG("Player scale reset to 1.0");
        }
        wasScale = st.customScale;
    }

    // --- infinite ammo: refill reserve inventory and top current weapon -----
    static bool wasInfiniteAmmo = false;
    static UObject* lastAmmoPawn = nullptr;
    static UObject* lastAmmoWeapon = nullptr;
    static ULONGLONG lastAmmoTickMs = 0;
    static ULONGLONG lastAmmoWeaponPollMs = 0;
    if (st.infiniteAmmo)
    {
        if (pawn != lastAmmoPawn)
        {
            lastAmmoPawn = pawn;
            lastAmmoWeapon = nullptr;
            lastAmmoTickMs = 0;
        }

        ULONGLONG ammoNowMs = GetTickCount64();
        UObject* weapon = nullptr;
        bool forceAmmo = !wasInfiniteAmmo;
        if (forceAmmo || ammoNowMs - lastAmmoWeaponPollMs > 1000)
        {
            weapon = GetCurrentWeaponObject(pawn);
            lastAmmoWeaponPollMs = ammoNowMs;
            if (weapon && weapon != lastAmmoWeapon)
            {
                forceAmmo = true;
                lastAmmoWeapon = weapon;
            }
        }

        UObject* effectiveWeapon = weapon;
        if (!effectiveWeapon && Mem::IsReadable(lastAmmoWeapon, 0x30))
            effectiveWeapon = lastAmmoWeapon;

        if (forceAmmo || ammoNowMs - lastAmmoTickMs > 500)
        {
            ApplyInfiniteAmmo(pawn, effectiveWeapon, forceAmmo, true, forceAmmo ? " enable/weapon-change" : " tick");
            lastAmmoTickMs = ammoNowMs;
        }
    }
    else if (wasInfiniteAmmo)
    {
        RestoreWeaponAmmoBackup();
        RestoreInventoryBackup();
        LOG("InfiniteAmmo disabled");
        lastAmmoPawn = nullptr;
        lastAmmoWeapon = nullptr;
        lastAmmoTickMs = 0;
        lastAmmoWeaponPollMs = 0;
    }
    wasInfiniteAmmo = st.infiniteAmmo;
}

void Features::Tick()
{
    if (!G::sdkReady.load()) return;
    // Present runs on the render thread; record it so the ProcessEvent hook never
    // mistakes it for the game thread when draining deferred (spawn) work.
    g_renderThreadId = GetCurrentThreadId();
    // /EHa: catch(...) traps C++ throws AND access violations, so a wrong
    // offset degrades to a no-op instead of crashing the game.
    try { TickImpl(); }
    catch (...) {}
}




