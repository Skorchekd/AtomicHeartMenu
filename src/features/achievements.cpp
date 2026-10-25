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
#include "achievements.h"
#include "features.h"
#include "../sdk/offsets.h"
#include "../sdk/reflect.h"
#include "../core/globals.h"
#include "../core/log.h"
#include "../core/memory.h"
#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace UE;
using namespace Achievements;
namespace AH = Offsets::AH;

// Shared with features.cpp (see features.h) rather than copied.
using Features::CachedFn;
using Features::CachedObject;
using Features::InstallProcessEventHook;
using Features::IsStructLike;
using Features::LowerCopy;
using Features::QueueGameThread;
using Features::SafeClassName;
using Features::SafeFNameToString;
using Features::SafeObjectFullName;
using Features::SafeObjectName;

namespace
{
    // =====================================================================
    //  Achievements.
    // ---------------------------------------------------------------------
    //  Unlocks go through the game's own event (see Offsets::AH), so the
    //  achievement subsystem keeps its own bookkeeping in step with what it
    //  tells Steam. Nothing here is a bypass -- the game has no cheats-used
    //  gate; this exists because cheating past content means the game's
    //  trigger never fires in the first place.
    // =====================================================================
    struct AchievementCatalogEntry
    {
        int         enumValue = 0;
        std::string name;
    };

    std::mutex                          g_achMutex;
    std::vector<AchievementCatalogEntry> g_achCatalog;  // stable storage for the names RowAt hands out
    std::unordered_set<int>             g_achSent;      // fired by US this session
    std::string                         g_achStatus = "No achievement action yet.";
    volatile LONG                       g_achResolveRunning = 0;

    // A failed resolve is a GObjects scan for nothing, and the tab asks for the
    // list every frame it is open, so a failure backs off instead of rescanning
    // each frame. Written by the resolve thread, read by the render thread.
    std::atomic<ULONGLONG>              g_achResolveFailedMs{ 0 };
    std::atomic<bool>                   g_achResolveFailed{ false };   // until a resolve succeeds
    std::atomic<bool>                   g_achMapTypeLogged{ false };   // once per resolve
    constexpr ULONGLONG kAchResolveRetryMs = 5000;
    constexpr int       kAchMaxIdChars     = 4096;   // longest platform id read out of the map

    // Same idea for the game-thread pump: before a world exists installing it
    // fails and logs, and the tab's callers run every frame.
    std::atomic<ULONGLONG>              g_achPumpFailedMs{ 0 };

    // What the engine and Steam say about one achievement. `engineProgress` is
    // UE's 0..100 percentage, so a plain (non-counting) achievement reads 0 or
    // 100 and "unlocked" is just progress >= kProgressComplete. `engineFound` is
    // the engine's bFoundID: false means the engine's cache has no such id -- which on
    // a first read usually means the async cache has not filled yet, NOT that the
    // achievement is locked. The two are kept apart so the UI can say "unknown"
    // instead of lying with "0%".
    struct AchievementReadState
    {
        bool        idKnown  = false;
        FName       id{};
        std::string idText;   // the platform id as the game stores it
        std::string title;    // Steam's own display name, "" until fetched
        // GetCachedAchievementDescription leaks an FText reference per call, so
        // it is asked once per id however the first try came out.
        bool        titleAttempted = false;
        bool        queried        = false;
        bool        engineFound    = false;
        float       engineProgress = 0.0f;   // the ENGINE's answer: 0 or 100, nothing between
        // Steam's own answer, read straight from steam_api64.dll. `steamReachable`
        // is whether steam_api64 could be used at all for the read that produced
        // this state; `steamResolved` is whether it answered for THIS id. When it
        // is reachable it is the authority and the engine cache is not consulted.
        bool        steamReachable  = false;
        bool        steamResolved   = false;
        bool        steamAchieved   = false;
        uint32_t    steamUnlockTime = 0; // unix seconds; 0 when never unlocked
    };
    std::vector<AchievementReadState> g_achStates;   // parallel to g_achCatalog
    std::string g_achSteamReadSummary = "Not read yet.";
    std::atomic<bool> g_achSteamUnavailable{ false };    // the last read ran without steam_api64
    std::atomic<int>  g_achSteamSim{ (int)SteamSimulation::Off };
    bool        g_achCachePrimed      = false;

    // The one read scheduler. A read is due once the previous one COMPLETED
    // kAchAutoRefreshMs ago (or none has), or when something asks for one soon
    // (an unlock); the in-flight flag keeps a read that has not drained from
    // being piled on. One read is ~1ms of game thread once the titles are cached
    // (the first pass is dearer because it fetches 83 of them), so a slow cadence
    // costs nothing worth measuring. The completion stamp is also what the tab
    // shows as "Updated N s ago".
    volatile LONG          g_achRefreshInFlight = 0;
    volatile LONG          g_achReadWanted      = 0;
    std::atomic<ULONGLONG> g_achLastReadDoneMs{ 0 };
    constexpr ULONGLONG kAchAutoRefreshMs    = 3000;
    constexpr ULONGLONG kAchMaxAutoRefreshMs = 10000;   // AC8 asks for a read at least this often
    static_assert(kAchAutoRefreshMs <= kAchMaxAutoRefreshMs, "auto-refresh cadence above the 10 s cap");

    constexpr const char* kAchNoPumpStatus =
        "No game-thread pump yet (ProcessEvent hook not installed) -- try again in the game world.";

    void SetAchievementStatus(std::string text)
    {
        std::lock_guard<std::mutex> lk(g_achMutex);
        g_achStatus = std::move(text);
    }

    // Installs the game-thread pump, backing off for kAchResolveRetryMs after a
    // failure. A button press passes `bypassBackoff` and tries again at once.
    bool EnsurePump(bool bypassBackoff)
    {
        const ULONGLONG failedAt = g_achPumpFailedMs.load();
        if (!bypassBackoff && failedAt && GetTickCount64() - failedAt < kAchResolveRetryMs)
            return false;
        if (InstallProcessEventHook())
        {
            g_achPumpFailedMs.store(0);
            return true;
        }
        g_achPumpFailedMs.store(GetTickCount64());
        SetAchievementStatus(kAchNoPumpStatus);
        return false;
    }

    // ProcessEvent refuses rather than faults on a dead object, and a refusal
    // leaves the out-params unwritten, so the caller must not read them. Logged
    // once per call site: these run per id, per refresh.
    bool Dispatch(UObject* object, UFunction* fn, void* params, const char* what)
    {
        if (object->ProcessEvent(fn, params))
            return true;
        static std::mutex                      s_mutex;
        static std::unordered_set<const char*> s_logged;
        bool first = false;
        {
            std::lock_guard<std::mutex> lk(s_mutex);
            first = s_logged.insert(what).second;
        }
        if (first)
            LOG("Achievements: %s refused (target not live); its results are ignored", what);
        return false;
    }

    void MarkResolveFailed()
    {
        g_achResolveFailed.store(true);
        g_achResolveFailedMs.store(GetTickCount64());
    }

    // UStruct::PropertiesSize of a class, struct or function; -1 if unreadable.
    int32_t ReadPropertiesSize(UObject* structOrFunction)
    {
        uint8_t* field = reinterpret_cast<uint8_t*>(structOrFunction) + Offsets::O_UStruct_PropertiesSize;
        return Mem::IsReadable(field, sizeof(int32_t)) ? *reinterpret_cast<int32_t*>(field) : -1;
    }

    bool EngineSaysUnlocked(const AchievementReadState& st)
    {
        return st.engineFound && st.engineProgress >= kProgressComplete;
    }

    // A reachable Steam is the authority, so an id it did not answer for is not
    // unlocked. The engine cache stands in only while Steam cannot be reached.
    bool IsUnlocked(const AchievementReadState& st)
    {
        return st.steamReachable ? (st.steamResolved && st.steamAchieved) : EngineSaysUnlocked(st);
    }

    // Each failing piece is named once per session, however many reads hit it.
    bool FirstTime(const std::string& key)
    {
        static std::mutex                    s_mutex;
        static std::unordered_set<std::string> s_seen;
        std::lock_guard<std::mutex> lk(s_mutex);
        return s_seen.insert(key).second;
    }

    bool IsSentinelName(const std::string& name)
    {
        static const std::string kMaxSuffix = "_MAX";
        return name == "INVALID" || name == "EAchievements_MAX" ||
               (name.size() >= kMaxSuffix.size() &&
                name.compare(name.size() - kMaxSuffix.size(), kMaxSuffix.size(), kMaxSuffix) == 0);
    }

    // Reads the live EAchievements enum into g_achCatalog. False leaves the list
    // empty.
    bool ResolveCatalog()
    {
        // CachedObject -> FindObjectFast only starts the background name index
        // when it MISSES, so without this the first Unlock press would resolve
        // nothing and no-op. Warming it here means it is ready long before the
        // user can click anything. VerifyOffsetsThread does the same.
        UE::StartObjectNameIndex();

        // A GObjects scan, hence the worker thread. The names come from the
        // live enum so they cannot drift out of step with the values we send.
        UObject* enumObject = FindObject(AH::Obj_EAchievementsEnum);
        int namesOffset = -1;
        std::vector<Reflect::EnumValue> values =
            Reflect::DescribeEnumValues(enumObject, &namesOffset);

        if (values.empty())
        {
            LOG_SDK("Achievements: EAchievements not readable (%s). The menu list stays "
                    "empty rather than guessing values; run Debug -> Dump achievement API "
                    "to see why.", enumObject ? "enum found, values unreadable" : "enum not found");
            SetAchievementStatus("Achievement list unavailable -- see the log.");
            return false;
        }

        const std::string prefix = AH::EAchievementsPrefix;
        std::vector<AchievementCatalogEntry> catalog;
        catalog.reserve(values.size());
        for (const Reflect::EnumValue& v : values)
        {
            std::string name = v.name;
            if (name.compare(0, prefix.size(), prefix) == 0)
                name.erase(0, prefix.size());
            // INVALID and the trailing _MAX sentinel are not achievements.
            if (IsSentinelName(name))
                continue;
            if (v.value < 0 || v.value > kMaxAchievementValue)   // the params block holds one byte
                continue;
            AchievementCatalogEntry e;
            e.enumValue = (int)v.value;
            e.name      = std::move(name);
            catalog.push_back(std::move(e));
        }

        int listed = 0;
        {
            std::lock_guard<std::mutex> lk(g_achMutex);
            g_achCatalog = std::move(catalog);
            g_achStatus  = "Achievement list ready.";
            listed       = (int)g_achCatalog.size();
        }
        LOG_SDK("Achievements: %d entries read from the live EAchievements enum "
                "(UEnum::Names at +0x%X)", listed, namesOffset);
        return listed > 0;
    }

    DWORD WINAPI AchievementResolveThread(LPVOID)
    {
        bool resolved = false;
        try { resolved = ResolveCatalog(); }
        catch (...) { LOG_SDK("Achievements: list resolve exception (ignored)"); }

        if (resolved)
        {
            g_achResolveFailed.store(false);
            g_achMapTypeLogged.store(false);   // the next id read logs the map's types again
        }
        else
        {
            MarkResolveFailed();
        }
        InterlockedExchange(&g_achResolveRunning, 0);
        return 0;
    }

    // GAME THREAD ONLY. Static function -> dispatched on the CDO, so no subsystem
    // instance is needed and there is nothing to go stale between frames.
    bool SendAchievementUnlockGameThread(int enumValue)
    {
        try
        {
            UObject*   lib = CachedObject(AH::Obj_GameplayEventUtils);
            UFunction* fn  = CachedFn(AH::Fn_SendAchievementUnlockEvent);
            if (!lib || !fn)
            {
                LOG("Achievements: unlock %d skipped (lib=%p fn=%p)", enumValue, (void*)lib, (void*)fn);
                return false;
            }
            P_SendAchievementUnlock p{};
            p.Achievement = (uint8_t)enumValue;
            if (!lib->ProcessEvent(fn, &p))
            {
                LOG("Achievements: unlock %d refused (SendAchievementUnlockEvent target not live)", enumValue);
                return false;
            }
            return true;
        }
        catch (...) { LOG("Achievements: unlock %d exception (ignored)", enumValue); return false; }
    }

    // GAME THREAD ONLY. Unlike the unlock, this one is an instance method, and the
    // instance is a LocalPlayer subsystem -- so it is fetched fresh each time
    // rather than cached across level loads.
    UObject* GetAchievementSubsystemGameThread()
    {
        try
        {
            UObject*   lib = CachedObject(AH::Obj_SubsystemUtils);
            UFunction* fn  = CachedFn(AH::Fn_GetAchievementSubsystem);
            if (!lib || !fn) return nullptr;
            P_GetAchievementSubsystem p{};
            if (!Dispatch(lib, fn, &p, "GetAchievementSubsystem"))
                return nullptr;
            UObject* subsystem = static_cast<UObject*>(p.ReturnValue);
            // The game handed this back; it has had no validation until now.
            return IsLiveObject(subsystem) ? subsystem : nullptr;
        }
        catch (...) { return nullptr; }
    }

    // GAME THREAD ONLY. Prime the engine's achievement cache from the online
    // subsystem. Async: the results land later, so a read taken straight after
    // this can legitimately come back empty. False when nothing was dispatched.
    bool PrimeAchievementCacheGameThread()
    {
        try
        {
            UObject*   proxy = CachedObject(AH::Obj_AchievementQueryProxy);
            UFunction* fn    = CachedFn(AH::Fn_CacheAchievements);
            UObject*   pc    = GetPlayerController();
            UObject*   world = GetWorld();
            if (!proxy || !fn || !pc || !world)
            {
                LOG("Achievements: cache prime skipped (proxy=%p fn=%p pc=%p world=%p)",
                    (void*)proxy, (void*)fn, (void*)pc, (void*)world);
                return false;
            }
            P_CacheAchievements p{};
            p.WorldContextObject = world;
            p.PlayerController   = pc;
            if (!Dispatch(proxy, fn, &p, "CacheAchievements"))
                return false;
            LOG("Achievements: engine cache prime requested (async)");
            return true;
        }
        catch (...) { LOG("Achievements: cache prime exception (ignored)"); return false; }
    }

    // GAME THREAD ONLY. Turn an id string into the FName the engine's achievement
    // API wants. TryGetFName is tried first because it costs no ProcessEvent, but
    // it can only FIND a name -- and the game keeps these ids as strings, so most
    // have never become names. Conv_StringToName creates the missing ones.
    FName MintFNameGameThread(const std::string& text)
    {
        FName out{};
        if (text.empty()) return out;
        if (TryGetFName(text.c_str(), out) && out.ComparisonIndex != 0)
            return out;

        out = FName{};
        UObject*   lib = CachedObject(AH::Obj_KismetStringLibrary);
        UFunction* fn  = CachedFn(AH::Fn_ConvStringToName);
        if (!lib || !fn) return out;

        try
        {
            // Ids are ASCII, so a straight widen is correct. The wstring stays
            // alive across the synchronous call -- same contract as the console
            // command path, which passes an FString the same way.
            std::wstring wide(text.begin(), text.end());
            P_ConvStringToName p{};
            p.InString.Data  = wide.data();
            p.InString.Count = (int32_t)wide.size() + 1; // include the null terminator
            p.InString.Max   = p.InString.Count;
            if (Dispatch(lib, fn, &p, "Conv_StringToName"))
                out = p.ReturnValue;
        }
        catch (...) { out = FName{}; }
        return out;
    }

    // An id held as an FString in the map. The header is live game memory, so the
    // length and the character buffer are checked before a byte of it is read.
    std::string ReadIdString(const FString* str)
    {
        if (!Mem::IsReadable(str, sizeof(FString))) return {};
        if (str->Count <= 0 || str->Count > kAchMaxIdChars || str->Count > str->Max) return {};
        if (!Mem::IsReadable(str->Data, (size_t)str->Count * sizeof(wchar_t))) return {};
        try { return str->ToString(); } catch (...) { return {}; }
    }

    // GAME THREAD ONLY. Read the EAchievements -> platform-id map off the live
    // subsystem. The ids are FNames taken straight out of the map, so nothing has
    // to build one from a string -- which could not work anyway, since the name
    // pool holds "EAchievements::KillTwins" as a single name and a standalone
    // "KillTwins" need not exist in it.
    int BuildAchievementIdTableGameThread(UObject* subsystem, bool forced)
    {
        Reflect::MapView map = Reflect::OpenMapProperty(subsystem, AH::Prop_AchievementsMap);
        if (!map.valid)
        {
            LOG("Achievements: %s unreadable -- %s. Unlocking still works; only the "
                "Steam-state read needs the ids.", AH::Prop_AchievementsMap, map.error.c_str());
            return 0;
        }

        // Only trusted once it IS what we expect. The key must be a 1-byte enum.
        // The value carries the platform id in one of two shapes: an FName, which
        // the engine's achievement API takes directly, or an FString, which has to
        // be looked up in the name pool first. Anything else and we would be
        // reinterpreting real data as something it is not.
        const bool keyOk = (map.keyType == "EnumProperty" || map.keyType == "ByteProperty") &&
                           map.keySize == 1;
        const bool valueIsName = map.valueType == "NameProperty" &&
                                 map.valueSize == (int)sizeof(FName);
        const bool valueIsText = map.valueType == "StrProperty" &&
                                 map.valueSize == (int)sizeof(FString);
        if (!keyOk || (!valueIsName && !valueIsText))
        {
            LOG("Achievements: %s is TMap<%s(%d), %s(%d)> -- not a shape this knows how "
                "to read (want a 1-byte enum key and an FName or FString value).",
                AH::Prop_AchievementsMap, map.keyType.c_str(), map.keySize,
                map.valueType.c_str(), map.valueSize);
            return 0;
        }
        // Once per resolve: the ids are re-read on every state read, and the value
        // property type (NameProperty vs StrProperty) is what this line is for.
        if (!g_achMapTypeLogged.exchange(true))
            LOG("Achievements: %s is TMap<%s(%d), %s(%d)>, %d slot(s) -- value property type %s",
                AH::Prop_AchievementsMap, map.keyType.c_str(), map.keySize,
                map.valueType.c_str(), map.valueSize, map.count, map.valueType.c_str());

        // Read the whole map out FIRST -- no lock held and no call into game code
        // while a MapView is pointing into live memory.
        struct RawId { int enumValue; std::string text; };
        std::vector<RawId> raw;
        raw.reserve((size_t)map.count);
        for (int i = 0; i < map.count; ++i)
        {
            uint8_t* keyPtr   = map.KeyAt(i);
            uint8_t* valuePtr = map.ValueAt(i);
            if (!keyPtr || !valuePtr) continue;          // a hole in the sparse array

            RawId entry;
            entry.enumValue = (int)*keyPtr;
            if (valueIsName)
                entry.text = SafeFNameToString(*reinterpret_cast<FName*>(valuePtr));
            else
                entry.text = ReadIdString(reinterpret_cast<const FString*>(valuePtr));
            if (!entry.text.empty())
                raw.push_back(std::move(entry));
        }

        int matched = 0, unmapped = 0, unresolved = 0;
        struct ResolvedId { int enumValue; FName id; std::string text; };
        std::vector<ResolvedId> resolved;
        resolved.reserve(raw.size());
        for (const RawId& entry : raw)
        {
            const FName id = MintFNameGameThread(entry.text);
            if (id.ComparisonIndex != 0) resolved.push_back({ entry.enumValue, id, entry.text });
            else                         ++unresolved;
        }

        int listed = 0;
        {
            std::lock_guard<std::mutex> lk(g_achMutex);
            listed = (int)g_achCatalog.size();

            std::unordered_map<int, size_t> slotByValue;
            slotByValue.reserve(g_achCatalog.size());
            for (size_t slot = 0; slot < g_achCatalog.size(); ++slot)
                slotByValue[g_achCatalog[slot].enumValue] = slot;

            std::vector<AchievementReadState> previous;
            previous.swap(g_achStates);
            g_achStates.assign(g_achCatalog.size(), AchievementReadState{});

            for (const ResolvedId& entry : resolved)
            {
                const auto it = slotByValue.find(entry.enumValue);
                // INVALID and _MAX are in the map but filtered out of the menu list.
                if (it == slotByValue.end()) { ++unmapped; continue; }

                const size_t slot = it->second;
                AchievementReadState state;
                // The last answer and the title survive a refresh while the slot's
                // id is unchanged: a title costs an FText reference we cannot
                // release, so it is fetched once, and a kept answer is what lets a
                // read tell whether anything moved. A different id starts clean.
                if (slot < previous.size() && previous[slot].idText == entry.text)
                    state = std::move(previous[slot]);
                state.idKnown = true;
                state.id      = entry.id;
                state.idText  = entry.text;
                g_achStates[slot] = std::move(state);
                ++matched;
            }
        }

        // The table is rebuilt on every read; say so only when it came out different.
        static int s_loggedMatched = -1, s_loggedUnmapped = -1, s_loggedUnresolved = -1;
        if (forced || matched != s_loggedMatched || unmapped != s_loggedUnmapped ||
            unresolved != s_loggedUnresolved)
        {
            s_loggedMatched    = matched;
            s_loggedUnmapped   = unmapped;
            s_loggedUnresolved = unresolved;
            LOG("Achievements: id table built -- %d of %d entries matched (%d map rows not in the "
                "menu list, %d ids that could not be made into an FName)",
                matched, listed, unmapped, unresolved);
        }
        return matched;
    }

    // =====================================================================
    //  Steam, read directly.
    // ---------------------------------------------------------------------
    //  The engine's cached achievement layer only ever answers 0 or 100, and on
    //  build 24534183 it reports UNLOCKED for two achievements Steam itself
    //  shows as locked with partial progress (Apple Pie 3826 at 859/3826 and
    //  Gold Rush at 0/76). Both are counting achievements with a non-zero
    //  counter; every counter sitting at exactly zero reads correctly. That is a
    //  pattern in the derived value, not two coincidences.
    //
    //  steam_api64.dll is already loaded in the process and its flat API is the
    //  same source the engine reads, so ask it directly instead of trusting
    //  something computed on the way out. It also answers a question the engine
    //  cannot: WHEN an achievement was unlocked.
    // =====================================================================
    constexpr const char* kSteamDll                  = "steam_api64.dll";
    constexpr const char* kSteamUserStatsExport      = "SteamAPI_SteamUserStats_v012";
    constexpr const char* kSteamGetAchievementExport = "SteamAPI_ISteamUserStats_GetAchievementAndUnlockTime";
    constexpr const char* kSteamInterfacePiece       = "ISteamUserStats interface";

    enum class SteamResolve { Ok, DllMissing, EntryPointMissing, InterfaceMissing };

    using SteamGetAchievementFn = bool (*)(void* stats, const char* id, bool* achieved, uint32_t* unlockTime);

    // Everything the tab uses from steam_api64's flat API, resolved together so
    // "can Steam be asked at all" is one answer, kept apart from "did Steam answer
    // for this id".
    struct SteamApi
    {
        SteamResolve          result      = SteamResolve::DllMissing;
        const char*           failedPiece = kSteamDll;   // what to blame when result != Ok
        void*                 userStats   = nullptr;
        SteamGetAchievementFn getAchievementAndUnlockTime = nullptr;

        bool Reachable() const { return result == SteamResolve::Ok; }
    };

    void LoadSteamApi(SteamApi& api)
    {
        HMODULE dll = GetModuleHandleA(kSteamDll);
        if (!dll)
        {
            api.result = SteamResolve::DllMissing;
            api.failedPiece = kSteamDll;
            return;
        }
        auto accessor = reinterpret_cast<void* (*)()>(GetProcAddress(dll, kSteamUserStatsExport));
        auto getFn    = reinterpret_cast<SteamGetAchievementFn>(GetProcAddress(dll, kSteamGetAchievementExport));
        // A bad function pointer that merely READS fine still DEP-faults on the
        // call, after control has left our code where catch(...) cannot reach it.
        if (!Mem::IsExecutable(reinterpret_cast<void*>(accessor), 1))
        {
            api.result = SteamResolve::EntryPointMissing;
            api.failedPiece = kSteamUserStatsExport;
            return;
        }
        if (!Mem::IsExecutable(reinterpret_cast<void*>(getFn), 1))
        {
            api.result = SteamResolve::EntryPointMissing;
            api.failedPiece = kSteamGetAchievementExport;
            return;
        }
        void* stats = nullptr;
        try { stats = accessor(); } catch (...) { stats = nullptr; }
        if (!stats)
        {
            api.result = SteamResolve::InterfaceMissing;
            api.failedPiece = kSteamInterfacePiece;
            return;
        }
        if (FirstTime("Steam: ISteamUserStats"))
            LOG("Steam: ISteamUserStats = %p", stats);
        api.userStats = stats;
        api.getAchievementAndUnlockTime = getFn;
        api.result = SteamResolve::Ok;
    }

    // Resolved afresh each read (a few GetProcAddress calls), so a Steam that was
    // not ready on the first read is picked up later. The debug simulation
    // reports a failure without going near the real handles.
    SteamApi ResolveSteamApi()
    {
        SteamApi api;
        const SteamSimulation sim = (SteamSimulation)g_achSteamSim.load();
        if (sim == SteamSimulation::DllMissing)
        {
            api.result = SteamResolve::DllMissing;
            api.failedPiece = kSteamDll;
        }
        else if (sim == SteamSimulation::EntryPointMissing)
        {
            api.result = SteamResolve::EntryPointMissing;
            api.failedPiece = kSteamGetAchievementExport;
        }
        else
        {
            LoadSteamApi(api);
        }

        if (!api.Reachable() && FirstTime(api.failedPiece))
            LOG("Steam: %s unavailable%s -- achievement state falls back to the engine cache",
                api.failedPiece, sim != SteamSimulation::Off ? " (simulated)" : "");
        return api;
    }

    // True when Steam answered for this id. False for "no answer" and for an
    // unreachable Steam alike; the caller knows which from `steam.Reachable()`.
    bool ReadSteamAchievementState(const SteamApi& steam, const char* id, bool& achieved, uint32_t& unlockTime)
    {
        achieved   = false;
        unlockTime = 0;
        if (!steam.Reachable() || !id || !*id) return false;

        try
        {
            bool     ach  = false;
            uint32_t when = 0;
            if (!steam.getAchievementAndUnlockTime(steam.userStats, id, &ach, &when)) return false;
            achieved   = ach;
            unlockTime = when;
            return true;
        }
        catch (...) { return false; }
    }

    // Unix seconds -> something a human can compare against a Steam profile.
    std::string FormatUnlockTime(uint32_t unixSeconds)
    {
        if (unixSeconds == 0) return "never";
        __time64_t t = (__time64_t)unixSeconds;
        std::tm     tm{};
        if (_localtime64_s(&tm, &t) != 0) return "?";
        char buf[32];
        if (std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm) == 0) return "?";
        return buf;
    }

    // GAME THREAD ONLY. The achievement's display name for an id, as the engine
    // cache holds it (Steam's own title).
    //
    // Called once per id and cached, not once per refresh: the FText out-params
    // arrive holding a reference we have no way to release, so repeating this
    // every refresh would leak a text object per achievement per press.
    std::string FetchAchievementTitleGameThread(UObject* world, UObject* pc, const FName& id)
    {
        try
        {
            UObject*   lib    = CachedObject(AH::Obj_AchievementBPLibrary);
            UFunction* descFn = CachedFn(AH::Fn_GetCachedAchievementDescription);
            if (!lib || !descFn) return {};

            P_GetCachedAchievementDescription descParams{};
            descParams.WorldContextObject = world;
            descParams.PlayerController   = pc;
            descParams.AchievementID      = id;
            if (!Dispatch(lib, descFn, &descParams, "GetCachedAchievementDescription")) return {};
            if (!descParams.bFoundID) return {};

            UObject*   textLib = CachedObject(AH::Obj_KismetTextLibrary);
            UFunction* convFn  = CachedFn(AH::Fn_ConvTextToString);
            if (!textLib || !convFn) return {};

            P_ConvTextToString convParams{};
            std::memcpy(convParams.InText, descParams.Title, sizeof(convParams.InText));
            if (!Dispatch(textLib, convFn, &convParams, "Conv_TextToString")) return {};
            return convParams.ReturnValue.ToString();
        }
        catch (...) { return {}; }
    }

    struct ReadTotals
    {
        int queried       = 0;
        int steamAnswered = 0;   // steam_api64 answered for the id
        int engineFound   = 0;   // the engine's cache had an answer for the id
        int unlocked      = 0;   // by IsUnlocked: Steam's word, or the cache's while Steam is unreachable
    };

    // What one read talks to. The engine cache needs its library and function;
    // missing either means "no fallback", not "no state" -- Steam is read
    // regardless.
    struct ReadContext
    {
        UObject*   world = nullptr;
        UObject*   pc    = nullptr;
        UObject*   lib   = nullptr;
        UFunction* progressFn = nullptr;
        SteamApi   steam;

        bool EngineReadable() const { return lib && progressFn; }
    };

    // Whether a read moved anything the log table or the row would show.
    bool AnswerChanged(const AchievementReadState& before, const AchievementReadState& after)
    {
        return !before.queried
            || before.steamReachable != after.steamReachable
            || before.steamResolved  != after.steamResolved
            || before.engineFound    != after.engineFound
            || EngineSaysUnlocked(before) != EngineSaysUnlocked(after)
            || IsUnlocked(before) != IsUnlocked(after);
    }

    // GAME THREAD ONLY. Ask Steam and the engine cache about one slot and store
    // the answers. True when the answer differs from the previous read.
    bool ReadOneAchievement(int slot, const ReadContext& ctx, ReadTotals& totals)
    {
        FName                id{};
        std::string          idText;
        bool                 wantTitle = false;
        AchievementReadState before;
        {
            std::lock_guard<std::mutex> lk(g_achMutex);
            const AchievementReadState& st = g_achStates[slot];
            if (!st.idKnown) return false;
            id        = st.id;
            idText    = st.idText;
            wantTitle = !st.titleAttempted;
            before    = st;
        }

        bool  engineFound    = false;
        float engineProgress = 0.0f;
        if (ctx.EngineReadable())
        {
            P_GetCachedAchievementProgress p{};
            p.WorldContextObject = ctx.world;
            p.PlayerController   = ctx.pc;
            p.AchievementID      = id;
            if (Dispatch(ctx.lib, ctx.progressFn, &p, "GetCachedAchievementProgress"))
            {
                engineFound    = p.bFoundID;
                engineProgress = p.Progress;
            }
        }

        // Outside the lock: these call into game / Steam code.
        std::string title;
        const bool  fetchTitle = wantTitle && engineFound;
        if (fetchTitle)
            title = FetchAchievementTitleGameThread(ctx.world, ctx.pc, id);

        bool     steamAchieved = false;
        uint32_t steamWhen     = 0;
        const bool steamAnswered = ReadSteamAchievementState(ctx.steam, idText.c_str(), steamAchieved, steamWhen);

        std::lock_guard<std::mutex> lk(g_achMutex);
        AchievementReadState& st = g_achStates[slot];
        st.queried         = true;
        st.engineFound     = engineFound;
        st.engineProgress  = engineProgress;
        st.steamReachable  = ctx.steam.Reachable();
        st.steamResolved   = steamAnswered;
        st.steamAchieved   = steamAchieved;
        st.steamUnlockTime = steamWhen;
        if (fetchTitle)
        {
            st.titleAttempted = true;
            if (!title.empty()) st.title = title;
        }

        ++totals.queried;
        if (steamAnswered)  ++totals.steamAnswered;
        if (engineFound)    ++totals.engineFound;
        if (IsUnlocked(st)) ++totals.unlocked;
        return AnswerChanged(before, st);
    }

    struct StateLogLine
    {
        std::string name;
        std::string title;
        const char* engine = "";
        const char* steam  = "";
        uint32_t    unlockTime = 0;
        bool        differs = false;
    };

    // One line per slot with the id actually asked for and the name STEAM gives
    // it -- the internal spelling and the Steam title are not the same string
    // ("GoldenFever" vs "Gold Rush"), and checking a read against a guessed
    // mapping is how a wrong answer passes for right.
    void LogStateTable(const std::vector<int>& slots)
    {
        std::vector<StateLogLine> lines;
        lines.reserve(slots.size());
        {
            std::lock_guard<std::mutex> lk(g_achMutex);
            for (int slot : slots)
            {
                if (slot < 0 || slot >= (int)g_achStates.size() || slot >= (int)g_achCatalog.size())
                    continue;
                const AchievementReadState& st = g_achStates[slot];
                StateLogLine line;
                line.name   = g_achCatalog[slot].name;
                line.title  = st.title;
                line.engine = !st.idKnown ? "NO ID"
                            : !st.engineFound ? "no answer"
                            : EngineSaysUnlocked(st) ? "UNLOCKED" : "locked";
                line.steam  = !st.steamReachable ? "unreachable"
                            : !st.steamResolved  ? "no answer"
                            : st.steamAchieved   ? "UNLOCKED" : "locked";
                line.unlockTime = st.steamUnlockTime;
                line.differs    = st.steamResolved && (st.steamAchieved != EngineSaysUnlocked(st));
                lines.push_back(std::move(line));
            }
        }

        int disagreements = 0;
        for (const StateLogLine& line : lines)
        {
            if (line.differs) ++disagreements;
            LOG("Achievements: %-32s \"%s\"  engine=%-9s steam=%-11s unlocked=%s%s",
                line.name.c_str(),
                line.title.empty() ? "?" : line.title.c_str(),
                line.engine, line.steam, FormatUnlockTime(line.unlockTime).c_str(),
                line.differs ? "   <<< ENGINE AND STEAM DISAGREE" : "");
        }
        if (disagreements)
            LOG("Achievements: %d achievement(s) where the engine's cached value and Steam "
                "itself disagree -- Steam's answer is the one used.", disagreements);
    }

    // One line for the status text and the log. Steam unreachable and Steam
    // reachable-but-silent read differently: only the first is a cache fallback.
    void FormatReadSummary(char* out, size_t outSize, const ReadContext& ctx, const ReadTotals& totals, int total)
    {
        if (!ctx.steam.Reachable())
            std::snprintf(out, outSize,
                          "%d of %d unlocked per engine cache (engine cache answered %d of %d ids).",
                          totals.unlocked, total, totals.engineFound, totals.queried);
        else if (totals.steamAnswered == 0)
            std::snprintf(out, outSize,
                          "Steam answered for none of %d ids -- rows read \"no answer\"; Steam may "
                          "still be loading, so press Refresh again in a second.", totals.queried);
        else
            std::snprintf(out, outSize,
                          "%d of %d unlocked per Steam (Steam answered %d of %d ids).",
                          totals.unlocked, total, totals.steamAnswered, totals.queried);
    }

    // GAME THREAD ONLY. One read of everything the engine cache and Steam know.
    // The per-id table, the id-table line and the summary go to the log on a
    // forced read (the Refresh button) and when they changed since the last read,
    // so the 3 s auto-refresh stays quiet.
    void RefreshAchievementStateGameThread(bool forced)
    {
        try
        {
            ReadContext ctx;
            UObject* subsystem = GetAchievementSubsystemGameThread();
            ctx.world = GetWorld();
            ctx.pc    = GetPlayerController();
            if (!subsystem || !ctx.world || !ctx.pc)
            {
                LOG("Achievements: state read skipped (subsystem=%p world=%p pc=%p)",
                    (void*)subsystem, (void*)ctx.world, (void*)ctx.pc);
                SetAchievementStatus("Achievement subsystem not available: not in a live game yet.");
                return;
            }

            // The id table (re)creates g_achStates, so the per-id reads below run after it.
            if (BuildAchievementIdTableGameThread(subsystem, forced) <= 0)
            {
                SetAchievementStatus("Achievement ids unavailable: the subsystem's id map could not be read -- see the log.");
                return;
            }

            ctx.steam      = ResolveSteamApi();
            ctx.lib        = CachedObject(AH::Obj_AchievementBPLibrary);
            ctx.progressFn = CachedFn(AH::Fn_GetCachedAchievementProgress);
            if (!ctx.EngineReadable() && FirstTime("engine-progress-library"))
                LOG("Achievements: engine progress read unavailable (lib=%p fn=%p) -- no cache fallback",
                    (void*)ctx.lib, (void*)ctx.progressFn);
            g_achSteamUnavailable.store(!ctx.steam.Reachable());
            if (!ctx.steam.Reachable() && !ctx.EngineReadable())
            {
                SetAchievementStatus("Neither Steam nor the engine cache can be read -- see the log.");
                return;
            }

            int total = 0;
            {
                std::lock_guard<std::mutex> lk(g_achMutex);
                total = (int)g_achStates.size();
            }
            ReadTotals       totals;
            std::vector<int> toLog;
            for (int i = 0; i < total; ++i)
            {
                const bool changed = ReadOneAchievement(i, ctx, totals);
                if (changed || forced)
                    toLog.push_back(i);
            }
            LogStateTable(toLog);

            char summary[256];
            FormatReadSummary(summary, sizeof(summary), ctx, totals, total);
            bool summaryChanged = false;
            {
                std::lock_guard<std::mutex> lk(g_achMutex);
                summaryChanged = g_achSteamReadSummary != summary;
                g_achSteamReadSummary = summary;
            }
            if (forced || summaryChanged)
                LOG("Achievements: %s", summary);
            SetAchievementStatus(summary);
        }
        catch (...)
        {
            LOG("Achievements: state read exception (ignored)");
            SetAchievementStatus("Achievement state read failed -- see the log.");
        }
    }

    // Ask the engine's async achievement query to run once the list is up, so
    // the engine's cache is filling before the user presses Refresh and expects
    // an answer. Called every frame the tab is open. Nothing is latched or queued
    // unless the pump exists, and a prime the game thread had to skip unlatches
    // so a later frame tries again.
    void PrimeCacheOnce()
    {
        {
            std::lock_guard<std::mutex> lk(g_achMutex);
            if (g_achCachePrimed) return;
        }
        if (!EnsurePump(false))
            return;
        {
            std::lock_guard<std::mutex> lk(g_achMutex);
            if (g_achCachePrimed) return;
            g_achCachePrimed = true;
        }
        QueueGameThread([]()
        {
            if (PrimeAchievementCacheGameThread())
                return;
            std::lock_guard<std::mutex> lk(g_achMutex);
            g_achCachePrimed = false;
        });
    }

    enum class QueueResult { Queued, InFlight, NoPump };

    // `forced` is the manual button: it re-runs the engine's async achievement
    // query, logs the whole per-id table and skips the pump back-off.
    QueueResult QueueAchievementStateRead(bool forced)
    {
        if (!EnsurePump(forced))
            return QueueResult::NoPump;
        if (InterlockedCompareExchange(&g_achRefreshInFlight, 1, 0) != 0)
            return QueueResult::InFlight;

        InterlockedExchange(&g_achReadWanted, 0);
        const ULONGLONG queuedMs = GetTickCount64();
        LOG("achievements: state read start");
        QueueGameThread([forced, queuedMs]()
        {
            if (forced) PrimeAchievementCacheGameThread();
            RefreshAchievementStateGameThread(forced);
            const ULONGLONG doneMs = GetTickCount64();
            g_achLastReadDoneMs.store(doneMs);
            LOG("achievements: state read done (%llu ms)", (unsigned long long)(doneMs - queuedMs));
            InterlockedExchange(&g_achRefreshInFlight, 0);
        });
        return QueueResult::Queued;
    }

    // Confirmed unlocked by Steam beats Sent (the unlock landed), Sent beats
    // everything else (it is what this menu did). Past that a reachable Steam is
    // the only authority; the engine cache speaks only while Steam is unreachable.
    Row::State RowStateOf(const AchievementReadState* st, bool sentByUs)
    {
        if (st && st->steamReachable && st->steamResolved && st->steamAchieved)
            return Row::State::Unlocked;
        if (sentByUs)              return Row::State::Sent;
        if (!st || !st->queried)   return Row::State::Unknown;

        if (st->steamReachable)
            return st->steamResolved ? Row::State::Locked : Row::State::NoAnswer;
        if (!st->engineFound)
            return Row::State::NoAnswer;
        return EngineSaysUnlocked(*st) ? Row::State::Unlocked : Row::State::Locked;
    }

    const char* BaseStateLabel(Row::State state)
    {
        switch (state)
        {
        case Row::State::Unlocked: return "unlocked";
        case Row::State::Locked:   return "locked";
        case Row::State::NoAnswer: return "no answer";
        case Row::State::Sent:     return "sent";
        case Row::State::Unknown:  break;
        }
        return "?";
    }

    const char* StateTooltip(Row::State state, bool fromEngineCache)
    {
        if (fromEngineCache)
        {
            if (state == Row::State::NoAnswer)
                return "Nothing answered for this id: Steam is unavailable and the engine's cache "
                       "has no entry for it.";
            return "From the engine's cache, because Steam was unavailable. The cache has reported "
                   "part-done achievements as unlocked, so it is not trusted: Unlock stays available.";
        }
        switch (state)
        {
        case Row::State::Unlocked: return "Steam reports this achievement as unlocked.";
        case Row::State::Locked:   return "Steam reports this achievement as not unlocked yet.";
        case Row::State::NoAnswer: return "Nothing answered for this id: Steam is reachable but has no "
                                          "answer for it.";
        case Row::State::Sent:     return "Fired by this menu this session; waiting for Steam to report it "
                                          "unlocked.";
        case Row::State::Unknown:  break;
        }
        return "Not read yet.";
    }

    // Unlocked only disables the button when Steam said so; a cache "unlocked" is
    // not trusted.
    void ApplyUnlockEnablement(Row& row)
    {
        const bool steamUnlocked = row.state == Row::State::Unlocked && row.source == Row::Source::Steam;
        if (steamUnlocked)
        {
            row.canUnlock      = false;
            row.disabledReason = "Already unlocked";
        }
        else if (row.state == Row::State::Sent)
        {
            row.canUnlock      = false;
            row.disabledReason = "Sent, waiting for Steam";
        }
        else
        {
            row.canUnlock      = true;
            row.disabledReason = "";
        }
    }

    // =====================================================================
    //  Achievement API discovery.
    // ---------------------------------------------------------------------
    //  Atomic Heart drives its own achievements through
    //  GameplayEventSubsystem.SendAchievementUnlockEvent -> the achievement
    //  subsystem -> WriteAchievementProgress -> Steam. Calling that path from
    //  here needs three things the exe's strings cannot give us: which class
    //  owns each function, the exact params-block layout, and the numeric
    //  EAchievements values. All three are in the running game's reflection
    //  data, so this reads them straight off it.
    //
    //  Strictly read-only -- names, offsets and flags. It never calls
    //  ProcessEvent, so it can (and does) run on a worker thread, and it
    //  cannot unlock anything by itself.
    // =====================================================================
    volatile LONG g_achDumpRunning = 0;

    constexpr int kAchMaxHits         = 400; // matched objects logged before we stop
    constexpr int kAchMaxOwnerClasses = 8;   // distinct owner classes dumped in full
    constexpr int kAchMaxFields       = 128; // fields listed per struct/class/function
    constexpr int kAchMaxClassFuncs   = 200; // functions listed per class in the pass-2 dump

    // "EnumProperty<EAchievements>" -- the type alone does not say which enum.
    std::string DescribeFieldType(const Reflect::FieldInfo& field)
    {
        std::string type = field.type.empty() ? std::string("?") : field.type;
        if (!field.detail.empty()) type += "<" + field.detail + ">";
        return type;
    }

    // Log a UFunction the way a caller has to see it: the params block it expects,
    // then every parameter with the offset to write it at.
    void LogFunctionSignature(UObject* function, const char* indent)
    {
        const std::string full = SafeObjectFullName(function);
        if (full.empty()) return;

        const int32_t parmsSize = ReadPropertiesSize(function);
        uint32_t      funcFlags = 0;
        if (Mem::IsReadable((uint8_t*)function + Offsets::O_UFunction_FunctionFlags, 4))
            funcFlags = *reinterpret_cast<uint32_t*>((uint8_t*)function + Offsets::O_UFunction_FunctionFlags);

        std::string decoded;
        if (funcFlags & Offsets::FUNC_Static)            decoded += " Static";
        if (funcFlags & Offsets::FUNC_Native)            decoded += " Native";
        if (funcFlags & Offsets::FUNC_BlueprintCallable) decoded += " BlueprintCallable";

        LOG_SDK("AchievementDump: %sFUNC %s  parms=0x%X flags=0x%08X%s",
                indent, full.c_str(), parmsSize, funcFlags,
                decoded.empty() ? " (none decoded)" : decoded.c_str());

        const std::vector<Reflect::FieldInfo> fields =
            Reflect::DescribeStructFields(function, kAchMaxFields);
        int parmCount = 0;
        for (const Reflect::FieldInfo& f : fields)
        {
            if (!f.IsParm()) continue;   // locals are not part of the params block
            ++parmCount;
            LOG_SDK("AchievementDump: %s    %-6s %-28s %-34s @0x%02X size=%d",
                    indent,
                    f.IsReturn() ? "return" : (f.IsOut() ? "out" : "in"),
                    f.name.c_str(), DescribeFieldType(f).c_str(), f.offset, f.size);
        }
        if (parmCount == 0)
            LOG_SDK("AchievementDump: %s    (no parameters -- a null params block is enough)", indent);
    }

    // Every UFunction a class declares itself, via its Children (UField) list.
    // The sweep only sees functions whose own NAME matches; this catches the
    // siblings on the same class that do not (e.g. a bare Unlock()).
    void LogClassApi(UObject* cls, const char* why)
    {
        const std::string full = SafeObjectFullName(cls);
        if (full.empty()) return;

        LOG_SDK("AchievementDump: ---- full API of %s (size=0x%X) -- %s",
                full.c_str(), ReadPropertiesSize(cls), why);

        int  functions = 0;
        bool truncated = false;
        if (Mem::IsReadable((uint8_t*)cls + Offsets::O_UStruct_Children, sizeof(void*)))
        {
            UObject* child = *reinterpret_cast<UObject**>((uint8_t*)cls + Offsets::O_UStruct_Children);
            for (int depth = 0; child && depth < 8192 && G::running.load(); ++depth)
            {
                if (!Mem::IsReadable(child, Offsets::O_UField_Next + sizeof(void*))) break;
                if (SafeClassName(child) == "Function")
                {
                    if (functions >= kAchMaxClassFuncs) { truncated = true; break; }
                    LogFunctionSignature(child, "  ");
                    ++functions;
                }
                child = *reinterpret_cast<UObject**>((uint8_t*)child + Offsets::O_UField_Next);
            }
        }
        if (truncated)
            LOG_SDK("AchievementDump:   (stopped at %d functions -- this class declares more, "
                    "raise kAchMaxClassFuncs to see the rest)", kAchMaxClassFuncs);

        const std::vector<Reflect::FieldInfo> fields = Reflect::DescribeStructFields(cls, kAchMaxFields);
        for (const Reflect::FieldInfo& f : fields)
            LOG_SDK("AchievementDump:   field %-34s %-34s @0x%03X size=%d",
                    f.name.c_str(), DescribeFieldType(f).c_str(), f.offset, f.size);
        if ((int)fields.size() == kAchMaxFields)
            LOG_SDK("AchievementDump:   (field list stopped at the %d-field cap -- there may be more)",
                    kAchMaxFields);
        LOG_SDK("AchievementDump: ---- end of %s: %d function(s), %d field(s)",
                full.c_str(), functions, (int)fields.size());
    }

    void LogEnumValues(UObject* uenum)
    {
        int namesOffset = -1;
        const std::vector<Reflect::EnumValue> values = Reflect::DescribeEnumValues(uenum, &namesOffset);
        const std::string full = SafeObjectFullName(uenum);
        if (values.empty())
        {
            LOG_SDK("AchievementDump: ENUM %s -- values NOT readable: nothing in the "
                    "UEnum::Names probe window (0x%X..0x%X) validated against the enum's own "
                    "name. The entry NAMES are still recoverable from the exe; only the numeric "
                    "values are missing, so widen the window in offsets.h or take them from a dump.",
                    full.c_str(), Offsets::Reflect::UEnum_NamesProbeFirst,
                    Offsets::Reflect::UEnum_NamesProbeLast);
            return;
        }
        LOG_SDK("AchievementDump: ENUM %s -- %d values (UEnum::Names validated at +0x%X; "
                "promote that to offsets.h once a dump agrees)",
                full.c_str(), (int)values.size(), namesOffset);
        for (const Reflect::EnumValue& v : values)
            LOG_SDK("AchievementDump:   %4lld = %s", (long long)v.value, v.name.c_str());
    }

    // An owner class earns a full dump only if it is actually about achievements.
    // Otherwise one incidental match drags in a whole engine library: on build
    // 24534183, KismetSystemLibrary matched once (ShowPlatformSpecificAchievements
    // Screen) and SubsystemUtils once (GetAchievementSubsystem), and dumping the
    // two of them cost ~250 lines of log for nothing pass 1 had not already
    // printed. Pass 2 exists only to reveal SIBLING functions whose own names do
    // not match, which is worth the lines on an achievement class and never on a
    // general-purpose library.
    struct AchievementOwner
    {
        UObject* cls     = nullptr;
        int      matched = 0;   // matched functions this class declares
    };

    bool OwnerEarnsFullDump(const AchievementOwner& owner)
    {
        // Its own name says achievements, or it declares more than one of them.
        if (owner.matched >= 2) return true;
        return LowerCopy(SafeObjectName(owner.cls)).find("achiev") != std::string::npos;
    }

    DWORD WINAPI AchievementApiDumpThread(LPVOID)
    {
        const ULONGLONG startMs = GetTickCount64();
        int functions = 0, enums = 0, types = 0, instances = 0, hits = 0;
        std::vector<AchievementOwner> ownerClasses;

        try
        {
            const int total = NumObjects();
            LOG_SDK("AchievementDump: sweeping %d objects for \"Achiev\" -- read-only reflection, "
                    "no ProcessEvent. This unlocks nothing; it reports what the achievement path "
                    "looks like on this build.", total);

            for (int i = 0; i < total && hits < kAchMaxHits && G::running.load(); ++i)
            {
                UObject* object = GetObjectByIndex(i);
                if (!Mem::IsReadable(object, 0x30)) continue;

                if (LowerCopy(SafeObjectName(object)).find("achiev") == std::string::npos)
                    continue;

                const std::string meta = SafeClassName(object);
                ++hits;

                if (meta == "Function")
                {
                    ++functions;
                    LogFunctionSignature(object, "");

                    // The owner is the class that declares it -- a direct pointer, so
                    // no name lookup can pick the wrong class. This is how we reach the
                    // subsystems themselves: their own names do not contain "Achiev".
                    UObject* owner = nullptr;
                    try { owner = object->Outer(); } catch (...) {}
                    if (Mem::IsReadable(owner, 0x30))
                    {
                        auto it = std::find_if(ownerClasses.begin(), ownerClasses.end(),
                                               [owner](const AchievementOwner& o) { return o.cls == owner; });
                        if (it != ownerClasses.end()) ++it->matched;
                        else                          ownerClasses.push_back({ owner, 1 });
                    }
                }
                else if (meta == "Enum")
                {
                    ++enums;
                    LogEnumValues(object);
                }
                else if (IsStructLike(object))
                {
                    ++types;
                    LOG_SDK("AchievementDump: TYPE %s  size=0x%X",
                            SafeObjectFullName(object).c_str(), ReadPropertiesSize(object));
                    for (const Reflect::FieldInfo& f : Reflect::DescribeStructFields(object, kAchMaxFields))
                        LOG_SDK("AchievementDump:     %-34s %-34s @0x%03X size=%d",
                                f.name.c_str(), DescribeFieldType(f).c_str(), f.offset, f.size);
                }
                else
                {
                    // A live instance or a property -- an instance is what a call would
                    // be dispatched ON, so its pointer is worth having.
                    ++instances;
                    LOG_SDK("AchievementDump: OBJ  %s  @%p", SafeObjectFullName(object).c_str(), (void*)object);
                }
            }

            if (hits >= kAchMaxHits)
                LOG_SDK("AchievementDump: hit the %d-object cap -- the list above is TRUNCATED and "
                        "later matches were not logged.", kAchMaxHits);

            // Pass 2: the owning classes in full. Everything the achievement path can
            // do lives here, including the functions whose names never say "Achiev"
            // (the Get/Set/Write siblings on the same subsystem).
            int dumped = 0, skippedAsIncidental = 0, skippedByCap = 0;
            for (const AchievementOwner& owner : ownerClasses)
            {
                if (!G::running.load()) break;
                if (!OwnerEarnsFullDump(owner))
                {
                    // Named, not dumped -- so a class that turns out to matter can
                    // still be found and the filter can be argued with.
                    ++skippedAsIncidental;
                    LOG_SDK("AchievementDump: owner %s declares %d matched function(s) and is "
                            "not an achievement class -- signature already printed above, full "
                            "dump skipped as noise.",
                            SafeObjectFullName(owner.cls).c_str(), owner.matched);
                    continue;
                }
                if (dumped >= kAchMaxOwnerClasses) { ++skippedByCap; continue; }
                LogClassApi(owner.cls, "declares a matched achievement function");
                ++dumped;
            }
            if (skippedByCap)
                LOG_SDK("AchievementDump: %d qualifying owner class(es) NOT dumped (cap is %d) "
                        "-- raise kAchMaxOwnerClasses if one of them is the class you need.",
                        skippedByCap, kAchMaxOwnerClasses);
            LOG_SDK("AchievementDump: pass 2 dumped %d of %d owner class(es) (%d incidental, %d capped)",
                    dumped, (int)ownerClasses.size(), skippedAsIncidental, skippedByCap);
        }
        catch (...) { LOG_SDK("AchievementDump: exception (ignored)"); }

        LOG_SDK("AchievementDump: DONE in %llums -- %d function(s), %d enum(s), %d type(s), "
                "%d live object(s), %d owner class(es) seen.",
                (unsigned long long)(GetTickCount64() - startMs),
                functions, enums, types, instances, (int)ownerClasses.size());
        if (functions == 0)
            LOG_SDK("AchievementDump: nothing matched. Either the engine globals are stale (check "
                    "for \"ResolveGlobals: VALID\" above) or the achievement classes are not loaded "
                    "yet -- get into the game world and run it again.");
        else
            LOG_SDK("AchievementDump: the Achievements tab drives this same path "
                    "(GameplayEventUtils.SendAchievementUnlockEvent). Re-run this after a game "
                    "patch: if a signature above stops matching the params structs in "
                    "achievements.h, the injection-time VerifyParams check will have said so too.");

        InterlockedExchange(&g_achDumpRunning, 0);
        return 0;
    }
}

// ---- Public API ---------------------------------------------------------------
// Every one of these is a thin, guarded entry point: the UI calls them from the
// render thread, and the actual ProcessEvent always happens on the game thread.

int Achievements::Count()
{
    std::lock_guard<std::mutex> lk(g_achMutex);
    return (int)g_achCatalog.size();
}

void Achievements::ResolveList()
{
    if (!G::sdkReady.load()) return;

    bool listUp = false;
    {
        std::lock_guard<std::mutex> lk(g_achMutex);
        listUp = !g_achCatalog.empty();
    }
    if (listUp)
    {
        PrimeCacheOnce();
        return;
    }

    // Called every frame the tab is open, so a scan in flight or a recent failure
    // must stop it spawning another: without that an open tab would start a
    // GObjects scan per frame.
    const ULONGLONG failedAt = g_achResolveFailedMs.load();
    if (failedAt && GetTickCount64() - failedAt < kAchResolveRetryMs)
        return;
    if (InterlockedCompareExchange(&g_achResolveRunning, 1, 0) != 0)
        return;

    HANDLE t = CreateThread(nullptr, 0, AchievementResolveThread, nullptr, 0, nullptr);
    if (!t)
    {
        MarkResolveFailed();
        InterlockedExchange(&g_achResolveRunning, 0);
        LOG_SDK("Achievements: list resolve thread spawn failed err=%lu", GetLastError());
        return;
    }
    CloseHandle(t);
}

bool Achievements::ListResolveFailed()
{
    return g_achResolveFailed.load() && Count() == 0;
}

void Achievements::RetryResolve()
{
    g_achResolveFailedMs.store(0);
    ResolveList();
}

bool Achievements::ReadInFlight()
{
    return g_achRefreshInFlight != 0;
}

void Achievements::RefreshState()
{
    if (!G::sdkReady.load()) { LOG("Achievements: state read skipped, SDK not ready"); return; }
    switch (QueueAchievementStateRead(true))
    {
    case QueueResult::InFlight: LOG("Achievements: state read already in flight"); break;
    case QueueResult::NoPump:   LOG("Achievements: state read skipped, no game-thread pump"); break;
    case QueueResult::Queued:   break;
    }
}

void Achievements::AutoRefresh()
{
    if (!G::sdkReady.load()) return;
    if (Count() == 0) return;                  // list not up yet; nothing to fill

    if (ReadInFlight()) return;
    const ULONGLONG lastDoneMs = g_achLastReadDoneMs.load();
    const bool      soon       = g_achReadWanted != 0;
    if (!soon && lastDoneMs && GetTickCount64() - lastDoneMs < kAchAutoRefreshMs)
        return;
    QueueAchievementStateRead(false);
}

unsigned int Achievements::AutoRefreshSeconds()
{
    return (unsigned int)(kAchAutoRefreshMs / 1000);
}

unsigned int Achievements::SecondsSinceRefresh()
{
    const ULONGLONG lastDoneMs = g_achLastReadDoneMs.load();
    if (lastDoneMs == 0) return kNeverRefreshed;
    return (unsigned int)((GetTickCount64() - lastDoneMs) / 1000);
}

bool Achievements::RowAt(int index, Row& out)
{
    std::lock_guard<std::mutex> lk(g_achMutex);
    if (index < 0 || index >= (int)g_achCatalog.size())
        return false;

    const AchievementCatalogEntry& entry = g_achCatalog[index];
    // The state vector only exists once the first id table has been built.
    const AchievementReadState* st = index < (int)g_achStates.size() ? &g_achStates[index] : nullptr;
    const bool fromEngineCache = st && st->queried && !st->steamReachable;

    out = Row{};
    out.enumValue = entry.enumValue;
    out.name      = entry.name.c_str();
    std::snprintf(out.unlockText, sizeof(out.unlockText), "not read");
    if (st)
    {
        std::snprintf(out.title, sizeof(out.title), "%s", st->title.c_str());
        if (fromEngineCache)
            std::snprintf(out.unlockText, sizeof(out.unlockText), "Steam unavailable");
        else if (st->steamResolved)
            std::snprintf(out.unlockText, sizeof(out.unlockText), "%s",
                          FormatUnlockTime(st->steamUnlockTime).c_str());
        else if (st->queried)
            std::snprintf(out.unlockText, sizeof(out.unlockText), "no Steam answer");
    }
    out.titlePending = !st || !st->titleAttempted;
    out.state        = RowStateOf(st, g_achSent.find(entry.enumValue) != g_achSent.end());

    // A sent row is this menu's own word, not a cache reading, so only the states
    // that came from the cache carry the mark.
    const bool cacheState = fromEngineCache && out.state != Row::State::Sent && out.state != Row::State::Unknown;
    out.source = cacheState ? Row::Source::Engine : Row::Source::Steam;
    std::snprintf(out.stateLabel, sizeof(out.stateLabel), "%s%s", BaseStateLabel(out.state),
                  cacheState ? " (cache)" : "");
    out.stateTooltip = StateTooltip(out.state, cacheState);
    ApplyUnlockEnablement(out);
    return true;
}

Achievements::Tally Achievements::GetTally()
{
    Tally tally;
    std::lock_guard<std::mutex> lk(g_achMutex);
    tally.total           = (int)g_achCatalog.size();
    tally.fromEngineCache = g_achSteamUnavailable.load();
    for (const AchievementReadState& st : g_achStates)
        if (st.queried && IsUnlocked(st))
            ++tally.unlocked;
    return tally;
}

bool Achievements::SteamUnavailable()
{
    return g_achSteamUnavailable.load();
}

void Achievements::SetSteamSimulation(SteamSimulation mode)
{
    g_achSteamSim.store((int)mode);
}

Achievements::SteamSimulation Achievements::GetSteamSimulation()
{
    return (SteamSimulation)g_achSteamSim.load();
}

const char* Achievements::SteamReadSummary()
{
    static std::string summary;
    std::lock_guard<std::mutex> lk(g_achMutex);
    summary = g_achSteamReadSummary;
    return summary.c_str();
}

void Achievements::Unlock(int enumValue)
{
    if (!G::sdkReady.load()) { LOG("Achievements: unlock skipped, SDK not ready"); return; }
    if (enumValue < 0 || enumValue > kMaxAchievementValue)
    {
        LOG("Achievements: unlock %d out of range", enumValue);
        return;
    }
    if (!InstallProcessEventHook())
    {
        LOG("Achievements: unlock %d skipped, no game-thread pump", enumValue);
        SetAchievementStatus(kAchNoPumpStatus);
        return;
    }

    std::string label = std::to_string(enumValue);
    {
        std::lock_guard<std::mutex> lk(g_achMutex);
        for (const AchievementCatalogEntry& e : g_achCatalog)
            if (e.enumValue == enumValue) { label = e.name; break; }
    }

    LOG("Achievements: unlock requested -- %s (%d)", label.c_str(), enumValue);
    QueueGameThread([enumValue]()
    {
        if (!SendAchievementUnlockGameThread(enumValue))
            return;
        std::lock_guard<std::mutex> lk(g_achMutex);
        g_achSent.insert(enumValue);
    });
    SetAchievementStatus("Sent unlock: " + label + ".");
    // Let the next auto tick pick it up rather than making the user press refresh.
    // Steam may not have committed it that fast, but the recurring cadence will
    // catch it either way.
    InterlockedExchange(&g_achReadWanted, 1);
}

const char* Achievements::LastActionStatus()
{
    static std::string status;
    std::lock_guard<std::mutex> lk(g_achMutex);
    status = g_achStatus;
    return status.c_str();
}

void Achievements::DebugDumpApi()
{
    if (!G::sdkReady.load()) { LOG_SDK("AchievementDump: SDK not ready"); return; }
    if (InterlockedCompareExchange(&g_achDumpRunning, 1, 0) != 0)
    {
        LOG_SDK("AchievementDump: already running");
        return;
    }
    HANDLE t = CreateThread(nullptr, 0, AchievementApiDumpThread, nullptr, 0, nullptr);
    if (!t)
    {
        InterlockedExchange(&g_achDumpRunning, 0);
        LOG_SDK("AchievementDump: thread spawn failed err=%lu", GetLastError());
        return;
    }
    CloseHandle(t);
}
