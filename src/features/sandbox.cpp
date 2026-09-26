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
#include "sandbox.h"
#include "features.h"
#include "../sdk/ue4.h"
#include "../sdk/offsets.h"
#include "../sdk/reflect_call.h"
#include "../core/globals.h"
#include "../core/log.h"
#include "../core/memory.h"
#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <functional>
#include <mutex>

namespace
{
    using namespace UE;
    using RefCall::Call;

    // FAssetData (CoreUObject_structs.hpp): 5 FNames then pad to 0x60.
    struct FAssetDataLite
    {
        FName ObjectPath; FName PackageName; FName PackagePath; FName AssetName; FName AssetClass;
        uint8_t pad[0x38];
    };
    static_assert(sizeof(FAssetDataLite) == 0x60, "FAssetData must be 0x60");

    struct MapRecord { Sandbox::MapEntry entry; FName name{}; };

    std::mutex             g_mutex;
    std::vector<MapRecord> g_maps;     // guarded by g_mutex
    Sandbox::Status        g_status;   // guarded by g_mutex
    // A sandbox load on its way (guarded by g_mutex): the requested package, when and
    // from which world it was asked for, and whether the save block was already up
    // then (a sandbox before it).
    std::string            g_pendingPackage;
    ULONGLONG              g_pendingSinceMs = 0;
    UObject*               g_requestWorld = nullptr;
    bool                   g_heldBeforeRequest = false;
    std::atomic<bool>      g_busy{ false };
    std::atomic<bool>      g_blockSaves{ true };  // the setting
    std::atomic<bool>      g_holding{ false };    // the sandbox's save block is up
    std::atomic<UObject*>  g_sandboxWorld{ nullptr }; // the world a sandbox load produced

    void SetMessage(const std::string& text, bool notify = true)
    {
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_status.message = text;
        }
        LOG("Sandbox: %s", text.c_str());
        if (notify)
            Features::Notify("%s", text.c_str());
    }

    std::string Lower(std::string text)
    {
        for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return text;
    }

    bool EndsWith(const std::string& text, const char* suffix)
    {
        const size_t n = strlen(suffix);
        return text.size() >= n && text.compare(text.size() - n, n, suffix) == 0;
    }

    // Top-level maps, as opposed to the streamed sublevels they pull in. Only a
    // naming heuristic; the full list stays one checkbox away.
    bool LooksMain(const std::string& lower)
    {
        return EndsWith(lower, "_p") || lower.find("persistent") != std::string::npos ||
            EndsWith(lower, "_main") || lower.find("openworld") != std::string::npos ||
            lower.find("mainmenu") != std::string::npos;
    }

    UObject* FindStatic(const char* fastName, const char* slowName)
    {
        UObject* object = FindObjectFast(fastName);
        if (!IsLiveObject(object))
            object = FindObject(slowName);
        return IsLiveObject(object) ? object : nullptr;
    }

    std::string WorldPackage(UObject* world)
    {
        UObject* outer = IsLiveObject(world) ? world->Outer() : nullptr;
        if (!IsLiveObject(outer))
            return {};
        try { return outer->GetName(); } catch (...) { return {}; }
    }

    // The loaded world is the requested map. A short name ("Map_P") names the last
    // element of the package path.
    bool SamePackage(const std::string& worldPackage, const std::string& requested)
    {
        const std::string a = Lower(worldPackage), b = Lower(requested);
        if (a == b)
            return !a.empty();
        return !b.empty() && b.find('/') == std::string::npos && a.size() > b.size() &&
            a.compare(a.size() - b.size(), b.size(), b) == 0 && a[a.size() - b.size() - 1] == '/';
    }

    // The title screen: its own player controller, spawned in the running world.
    bool AtTitleMenu(UObject* world)
    {
        UObject* pc = GetPlayerController();
        if (!IsLiveObject(pc) || !IsLiveObject(pc->Class()))
            return false;
        UObject* level = pc->Outer();
        if (!IsLiveObject(level) || level->Outer() != world)
            return false;
        try { return pc->Class()->GetName() == "BP_MainMenuPlayerController_C"; }
        catch (...) { return false; }
    }

    // Game thread: SetSaveBlock resolves the save functions there.
    void HoldSaves()
    {
        Features::SetSaveBlock(Features::SaveBlockSandbox, true);
        g_holding = true;
    }

    void ReleaseSaves()
    {
        g_holding = false;
        Features::SetSaveBlock(Features::SaveBlockSandbox, false);
    }

    void Queue(std::function<void()> action)
    {
        if (g_busy.exchange(true))
            return;
        if (!Features::QueueGameAction([action = std::move(action)]()
        {
            try { action(); }
            catch (...) { LOG("Sandbox: game action faulted (ignored)"); }
            g_busy = false;
        }))
        {
            g_busy = false;
            SetMessage("Game-thread queue unavailable. Try again in a moment.");
        }
    }

    void ReadMapListGameThread()
    {
        UObject* helpers = FindStatic("AssetRegistryHelpers /Script/AssetRegistry.Default__AssetRegistryHelpers",
                                      "AssetRegistry.Default__AssetRegistryHelpers");
        Call get(helpers, "GetAssetRegistry");
        struct { void* object; void* iface; } registry{};
        UObject* reg = nullptr;
        if (get.Run() && get.Get("ReturnValue", registry))
            reg = static_cast<UObject*>(registry.object);
        UObject* worldClass = FindObjectFast("Class /Script/Engine.World");
        if (!IsLiveObject(worldClass))
            worldClass = FindObjectFast("Engine.World");
        if (!IsLiveObject(reg) || !IsLiveObject(worldClass))
        { SetMessage("The game's asset registry is unavailable."); return; }

        Call byClass(reg, "GetAssetsByClass");
        byClass.Set("ClassName", *worldClass->NamePtr());
        byClass.Set("bSearchSubClasses", true);
        TArray<FAssetDataLite> found{};
        if (!byClass.Run() || !byClass.Get("OutAssetData", found))
        { SetMessage("The asset registry did not answer the map query."); return; }
        if (found.Count <= 0 || found.Count > 20000 ||
            !Mem::IsReadable(found.Data, static_cast<size_t>(found.Count) * sizeof(FAssetDataLite)))
        { SetMessage("The asset registry returned no maps."); return; }

        std::vector<MapRecord> maps;
        maps.reserve(found.Count);
        for (int i = 0; i < found.Count; ++i)
        {
            const FAssetDataLite& asset = found.Data[i];
            std::string package;
            try { package = asset.PackageName.ToString(); } catch (...) { continue; }
            if (package.size() < 3 || package.rfind("/Engine/", 0) == 0 || package.rfind("/Script/", 0) == 0)
                continue;
            MapRecord record;
            record.entry.package = package;
            record.entry.likelyMain = LooksMain(Lower(package));
            record.name = asset.PackageName;
            maps.push_back(record);
        }
        // The game's array is engine-allocated and is left to the engine (read once).
        std::sort(maps.begin(), maps.end(), [](const MapRecord& a, const MapRecord& b)
        {
            if (a.entry.likelyMain != b.entry.likelyMain) return a.entry.likelyMain;
            return a.entry.package < b.entry.package;
        });
        const int count = static_cast<int>(maps.size());
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_maps.swap(maps);
            g_status.listed = true;
            g_status.mapCount = count;
        }
        SetMessage("Found " + std::to_string(count) + " maps. Pick one and load it.", false);
    }

    // A listed map's own FName, matched without regard to case.
    bool ListedName(const std::string& package, FName& out, std::string& exact)
    {
        const std::string wanted = Lower(package);
        std::lock_guard<std::mutex> lock(g_mutex);
        for (const MapRecord& record : g_maps)
            if (Lower(record.entry.package) == wanted)
            {
                out = record.name;
                exact = record.entry.package;
                return true;
            }
        return false;
    }

    // KismetStringLibrary.Conv_StringToName, for a name typed by hand when the map
    // list could not be read. ASCII package paths only.
    bool NameFromText(const std::string& text, FName& out)
    {
        for (char c : text)
            if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '/' || c == '_' || c == '-'))
                return false;
        UObject* strings = FindStatic("KismetStringLibrary /Script/Engine.Default__KismetStringLibrary",
                                      "Engine.Default__KismetStringLibrary");
        if (!strings)
            return false;
        // The engine copies the string during the synchronous call; the buffer stays ours.
        std::wstring wide(text.begin(), text.end());
        FString value{};
        value.Data = wide.data();
        value.Count = static_cast<int32_t>(wide.size()) + 1; // include the null terminator
        value.Max = value.Count;
        Call convert(strings, "Conv_StringToName");
        convert.Set("InString", value);
        FName name{};
        if (!convert.Run() || !convert.Get("ReturnValue", name) || name.ComparisonIndex == 0)
            return false;
        out = name;
        return true;
    }

    void OpenLevelGameThread(const std::string& package, const FName& levelName)
    {
        UObject* statics = FindStatic("GameplayStatics /Script/Engine.Default__GameplayStatics", "Engine.Default__GameplayStatics");
        UObject* world = GetWorld();
        if (!statics || !IsLiveObject(world))
        { SetMessage("Level loading is unavailable right now."); return; }
        // Saves are blocked before the load starts, so nothing is written on the way
        // out, in the sandbox, or after it until the title menu (Sandbox::Tick).
        const bool heldBefore = g_holding.load();
        if (g_blockSaves.load())
            HoldSaves();
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_pendingPackage = package;
            g_pendingSinceMs = GetTickCount64();
            g_requestWorld = world;
            g_heldBeforeRequest = heldBefore;
        }
        Call open(statics, "OpenLevel");
        open.Set("WorldContextObject", world);
        open.Set("LevelName", levelName);
        open.Set("bAbsolute", true);
        if (!open.Run())
        {
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                g_pendingPackage.clear();
                g_requestWorld = nullptr;
            }
            if (!heldBefore)
                ReleaseSaves();
            SetMessage("The game refused to load " + package + ".");
            return;
        }
        G::menuOpen = false;
        SetMessage("Loading " + package + " ...");
    }

    void OpenMapGameThread(const std::string& package)
    {
        FName levelName{};
        std::string exact;
        if (!ListedName(package, levelName, exact))
        { SetMessage("That map is not in the list; read the list again."); return; }
        OpenLevelGameThread(exact, levelName);
    }

    void OpenNamedMapGameThread(const std::string& typed)
    {
        // An object path ("/Game/Maps/X.X") names its package before the dot; a dot
        // left in would make the engine read the name as a network address.
        const std::string package = typed.substr(0, typed.find('.'));
        FName levelName{};
        std::string exact;
        if (ListedName(package, levelName, exact))
        { OpenLevelGameThread(exact, levelName); return; }
        if (!NameFromText(package, levelName))
        { SetMessage("\"" + typed + "\" is not a map name the game can use (letters, digits and / _ - only)."); return; }
        OpenLevelGameThread(package, levelName);
    }
}

Sandbox::Status Sandbox::GetStatus()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    Status copy = g_status;
    copy.busy = g_busy.load();
    copy.loading = !g_pendingPackage.empty();
    copy.holdingSaves = g_holding.load();
    copy.savesBlocked = Features::SavesBlocked();
    UObject* sandbox = g_sandboxWorld.load();
    copy.sandboxActive = sandbox && sandbox == GetWorld();
    return copy;
}

std::vector<Sandbox::MapEntry> Sandbox::Maps(const char* filter, int maxCount, bool mainOnly)
{
    std::vector<MapEntry> out;
    const std::string needle = Lower(filter ? filter : "");
    std::lock_guard<std::mutex> lock(g_mutex);
    for (const MapRecord& record : g_maps)
    {
        if ((int)out.size() >= maxCount) break;
        if (mainOnly && !record.entry.likelyMain) continue;
        if (!needle.empty() && Lower(record.entry.package).find(needle) == std::string::npos) continue;
        out.push_back(record.entry);
    }
    return out;
}

void Sandbox::RefreshMaps()
{
    if (!G::sdkReady.load()) return;
    SetMessage("Reading the game's map list...", false);
    Queue([]() { ReadMapListGameThread(); });
}

void Sandbox::OpenMap(const std::string& package)
{
    if (!G::sdkReady.load() || package.empty()) return;
    Queue([package]() { OpenMapGameThread(package); });
}

void Sandbox::OpenMapByName(const std::string& name)
{
    std::string trimmed = name;
    while (!trimmed.empty() && std::isspace(static_cast<unsigned char>(trimmed.back()))) trimmed.pop_back();
    size_t start = 0;
    while (start < trimmed.size() && std::isspace(static_cast<unsigned char>(trimmed[start]))) ++start;
    trimmed.erase(0, start);
    if (!G::sdkReady.load() || trimmed.empty()) return;
    Queue([trimmed]() { OpenNamedMapGameThread(trimmed); });
}

void Sandbox::SetBlockSaves(bool on)
{
    g_blockSaves = on;
    if (!on)
    {
        if (g_holding.load())
        {
            ReleaseSaves();
            SetMessage("Game saves are allowed again.", false);
        }
        return;
    }
    UObject* sandbox = g_sandboxWorld.load();
    if (sandbox && sandbox == GetWorld())
        Features::QueueGameAction([]() { if (g_blockSaves.load()) HoldSaves(); });
}

bool Sandbox::BlockSaves() { return g_blockSaves.load(); }

void Sandbox::Tick()
{
    // Follows world changes. The first world to arrive after a sandbox load is the
    // sandbox, even when it is not the requested map (a failed load falls back to
    // the title menu). The save block then stays up until the title menu: a level
    // the game moves on to from the sandbox, or a campaign save loaded from its
    // pause menu, must not autosave sandbox progress over the campaign.
    static UObject* lastWorld = nullptr;
    static int32_t lastIndex = -1;
    static std::string lastPackage;
    static bool sawGap = false;
    UObject* world = GetWorld();
    if (!IsLiveObject(world))
    {
        sawGap = true; // mid-load: the next live world is a new one, even at the same address
        return;
    }
    const int32_t index = world->Index();
    const std::string package = WorldPackage(world);
    bool unblock = false, stillHeld = false;
    std::string notice;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        const ULONGLONG now = GetTickCount64();
        const bool changed = sawGap || world != lastWorld || index != lastIndex || package != lastPackage;
        if (changed)
        {
            lastWorld = world;
            lastIndex = index;
            lastPackage = package;
            sawGap = false;
            g_status.currentMap = package;
            if (!g_pendingPackage.empty())
            {
                const bool requested = SamePackage(package, g_pendingPackage);
                g_status.message = requested
                    ? "Sandbox map loaded: " + package + "."
                    : "Arrived in " + package + " instead of " + g_pendingPackage + "; the load may have failed.";
                if (g_holding.load())
                    g_status.message += " Game saves stay blocked until you return to the title menu.";
                notice = g_status.message;
                g_pendingPackage.clear();
                g_requestWorld = nullptr;
                g_sandboxWorld = world;
            }
            else if (g_sandboxWorld.load() && g_sandboxWorld.load() != world)
            {
                g_sandboxWorld = nullptr;
                stillHeld = g_holding.load();
            }
        }
        else if (!g_pendingPackage.empty() && world == g_requestWorld && now - g_pendingSinceMs > 60000)
        {
            // Still in the world the load was asked from a minute later: the game
            // dropped the request.
            g_pendingPackage.clear();
            g_requestWorld = nullptr;
            unblock = !g_heldBeforeRequest && g_holding.load();
            g_status.message = "The game never started loading the map.";
        }
        // The title menu ends the sandbox and its save block.
        if (g_holding.load() && g_pendingPackage.empty() && AtTitleMenu(world))
        {
            unblock = true;
            stillHeld = false;
            g_sandboxWorld = nullptr;
            g_status.message = "At the title menu: game saves are allowed again.";
        }
    }
    if (unblock)
    {
        ReleaseSaves();
        LOG("Sandbox: save block lifted");
    }
    if (!notice.empty())
        Features::Notify("%s", notice.c_str());
    if (stillHeld)
        Features::Notify("Game saves stay blocked after the sandbox until you return to the title menu.");
}
