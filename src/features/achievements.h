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
#include <climits>
#include <cstdint>

// The Achievements tab: lists every achievement with its Steam state and fires
// the game's OWN achievement event (GameplayEventUtils.SendAchievementUnlockEvent),
// so UAHAchievementSubsystem does its normal bookkeeping and tells Steam itself.
// Nothing here writes to Steam directly, and nothing here is a workaround for a
// block: the game has no cheats-used gate. It exists because cheats make you SKIP
// the triggers -- fly past a boss and UnlockBossAchievement simply never fires.
//
// Every call is marshalled to the game thread. An unlock broadcasts a gameplay
// event that runs game logic (UI popup, the Steam write), which is exactly the
// kind of work the render thread must not start.
namespace Achievements
{
    // ---- ProcessEvent param structs -----------------------------------------
    // Sizes and member offsets read off the running game with Debug -> Dump
    // achievement API, and re-checked at injection against UFunction::PropertiesSize
    // (Features::RegisterParamsChecks; achievements.cpp registers them). A call
    // whose check did not pass is refused rather than made.
    struct P_SendAchievementUnlock { uint8_t Achievement; };
    static_assert(sizeof(P_SendAchievementUnlock) == 0x01, "SendAchievementUnlockEvent params");

    struct P_GetAchievementSubsystem { void* ReturnValue; };
    static_assert(sizeof(P_GetAchievementSubsystem) == 0x08, "GetAchievementSubsystem params");

    struct P_CacheAchievements
    {
        void* WorldContextObject; // @0x00
        void* PlayerController;   // @0x08
        void* ReturnValue;        // @0x10  (the async proxy; we do not subscribe to it)
    };
    static_assert(sizeof(P_CacheAchievements) == 0x18, "CacheAchievements params");

    struct P_GetCachedAchievementProgress
    {
        void*       WorldContextObject; // @0x00
        void*       PlayerController;   // @0x08
        UE::FName   AchievementID;      // @0x10
        bool        bFoundID;           // @0x18  (out)
        uint8_t     _pad0[3];
        float       Progress;           // @0x1C  (out, 0..100)
    };
    static_assert(sizeof(P_GetCachedAchievementProgress) == 0x20, "GetCachedAchievementProgress params");

    struct P_ConvStringToName
    {
        UE::FString InString;    // @0x00
        UE::FName   ReturnValue; // @0x10
    };
    static_assert(sizeof(P_ConvStringToName) == 0x18, "Conv_StringToName params");

    struct P_GetCachedAchievementDescription
    {
        void*       WorldContextObject;      // @0x00
        void*       PlayerController;        // @0x08
        UE::FName   AchievementID;           // @0x10
        bool        bFoundID;                // @0x18  (out)
        uint8_t     _pad0[7];
        uint8_t     Title[24];               // @0x20  (out) FText
        uint8_t     LockedDescription[24];   // @0x38  (out) FText
        uint8_t     UnlockedDescription[24]; // @0x50  (out) FText
        bool        bHidden;                 // @0x68  (out)
        uint8_t     _pad1[7];
    };
    static_assert(sizeof(P_GetCachedAchievementDescription) == 0x70, "GetCachedAchievementDescription params");

    struct P_ConvTextToString
    {
        uint8_t     InText[24];  // @0x00  FText
        UE::FString ReturnValue; // @0x18
    };
    static_assert(sizeof(P_ConvTextToString) == 0x28, "Conv_TextToString params");

    // ---- Constants ----------------------------------------------------------
    // UE's cached achievement progress is a 0..100 percentage.
    constexpr float kProgressComplete = 100.0f;
    // EAchievements travels as the one byte of P_SendAchievementUnlock.
    constexpr int kMaxAchievementValue = 255;
    static_assert(kMaxAchievementValue == (1 << (8 * sizeof(P_SendAchievementUnlock::Achievement))) - 1,
                  "an EAchievements value must fit the unlock params byte");
    constexpr unsigned int kNeverRefreshed = UINT_MAX;   // SecondsSinceRefresh() before the first read

    // ---- The row model ------------------------------------------------------
    // Everything the tab shows or decides about one achievement, filled in one
    // place (RowAt). The label, the Unlock button and its tooltip all read this
    // struct and nothing else, so they cannot disagree.
    struct Row
    {
        // Distinct on purpose: "locked" only appears when something actually
        // answered for this id, so a failed read says so instead of masquerading
        // as not-earned. `Sent` is what THIS menu fired this session while Steam
        // has not yet reported it unlocked.
        enum class State { Unknown, Locked, Unlocked, NoAnswer, Sent, Unconfirmed };
        // Who produced `state`. EngineCache is the engine's own cached layer, used
        // only while Steam cannot be reached at all; a reachable Steam that has no
        // answer for an id gives NoAnswer, never a cache guess.
        enum class Source { Steam, EngineCache };

        int          enumValue = 0;           // EAchievements numeric value
        const char*  name  = "";              // short name, "EAchievements::" stripped
        char         title[96] = {};          // Steam's display name; empty until fetched
        bool         titlePending = true;     // no title yet: not fetched, or fetched and empty
        State        state  = State::Unknown;
        Source       source = Source::Steam;
        char         stateLabel[32] = {};     // "unlocked", "locked (cache)", "sent, not confirmed", ...
        const char*  stateTooltip = "";       // what the label means
        bool         pending = false;         // a click is queued for the game thread
        bool         sent = false;            // this menu fired it; Steam has not confirmed yet
        bool         unlockFailed = false;    // the last click did not send; cleared by a click or a read
        char         unlockText[32] = {};     // when Steam says it was unlocked, or why that is unknown
        bool         canUnlock = false;
        const char*  disabledReason = "";     // why canUnlock is false; "" when enabled

        // Unlocked on Steam's word. A cache-derived "unlocked" is not confirmed.
        bool ConfirmedUnlocked() const { return state == State::Unlocked && source == Source::Steam; }
    };

    // The header line: how many are unlocked of how many are listed. `fromEngineCache`
    // is true while Steam is unreachable and the count is the engine cache's.
    struct Tally
    {
        int  total = 0;
        int  unlocked = 0;
        bool fromEngineCache = false;
    };
    Tally GetTally();

    // Debug toggle: make the Steam resolver report a failure (and log that piece)
    // without touching the real handles.
    enum class SteamSimulation { Off, DllMissing, EntryPointMissing };
    void SetSteamSimulation(SteamSimulation mode);
    SteamSimulation GetSteamSimulation();

    // Short name of the first achievement function whose params check failed
    // ("SendAchievementUnlockEvent"), or nullptr. While set, every Unlock is
    // refused and the tab shows a banner. Cached only; safe every frame.
    const char* SignatureDrift();

    // Debug toggles for the unlock path; none of them calls the game.
    void SetSimulateUnlockFailure(bool on);      // the queued call reports failure without ProcessEvent
    bool GetSimulateUnlockFailure();
    void SetSimulateStalledGameThread(bool on);  // clicks are never queued, so they expire
    bool GetSimulateStalledGameThread();
    void SimulateSend(int enumValue);            // mark a row Sent without ProcessEvent
    void SetSteamPinnedLocked(int enumValue);    // Steam's answer for this row reads locked; -1 = off
    int  GetSteamPinnedLocked();

    // Debug toggle: report one achievement function as drifted (-1 = off) so the
    // refusal path can be exercised without a patched game.
    int         DriftChoiceCount();
    const char* DriftChoiceName(int index);
    void        SetDriftSimulation(int index);
    int         GetDriftSimulation();

    // The list is read from the live EAchievements UEnum, never hardcoded, so a
    // patch that adds or reorders entries needs no code change. Resolving it needs
    // a GObjects scan, so it happens on a worker thread: Count() is 0 until it
    // lands, and the menu says so rather than showing an empty list.
    int  Count();
    void ResolveList();                         // idempotent; safe to call every frame
    bool ListResolveFailed();                   // no list, and the last resolve attempt failed
    void RetryResolve();                        // drops the retry back-off and resolves now
    // Fills `out` for list slot `index`. `out.name` points into the list and stays
    // valid for as long as the list does. False only when index is out of range.
    bool RowAt(int index, Row& out);

    void Unlock(int enumValue);                 // one, on the game thread; at most one in flight per row
    const char* LastActionStatus();             // one line for the UI

    void RefreshState();                        // prime + read, forced, on the game thread
    bool ReadInFlight();                        // a state read is queued or running
    // Called every frame the tab is open. Throttled and single-flight, so it is a
    // no-op on most frames; re-reads state on a slow cadence so unlock states
    // follow the game without anyone pressing anything. Does NOT re-prime the
    // engine's achievement cache -- that is an async online query and firing one
    // every few seconds would be rude to the subsystem for no benefit.
    void AutoRefresh();
    unsigned int AutoRefreshSeconds();          // the cadence AutoRefresh keeps
    unsigned int SecondsSinceRefresh();         // since the last read completed; kNeverRefreshed if none has
    const char* SteamReadSummary();

    // Discovery: log the game's whole achievement API off the running game --
    // every UFunction / class / struct / enum whose name contains "Achiev", with
    // full paths, params-block sizes and per-parameter type+offset, plus the
    // EAchievements name->value table and the full API of the two subsystems that
    // own the path. This is what a ProcessEvent achievement call has to be built
    // from; nothing here writes, calls or unlocks anything. Worker thread; results
    // go to the log.
    void DebugDumpApi();
}
