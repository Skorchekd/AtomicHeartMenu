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
#include "ue4.h"
#include "scanner.h"
#include "../core/globals.h"
#include "../core/log.h"
#include "../core/memory.h"
#include <Windows.h>
#include <atomic>
#include <cstring>
#include <cstdio>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace
{
    uint8_t* g_GObjects = nullptr; // FUObjectArray*
    uint8_t* g_GNames   = nullptr; // FNamePool*
    UE::UObject** g_GWorld = nullptr;
    std::mutex g_findObjectMutex;
    std::unordered_map<std::string, UE::UObject*> g_findObjectCache;
    std::unordered_map<std::string, int> g_findObjectIndexCache;
    std::mutex g_nameIndexMutex;
    std::unordered_map<std::string, std::vector<UE::UObject*>> g_nameIndex;
    std::atomic<bool> g_nameIndexBuilding{ false };
    std::atomic<bool> g_nameIndexReady{ false };
    std::atomic<int> g_nameIndexObjectCount{ 0 };
    std::atomic<UE::UObject*> g_nameIndexWorld{ nullptr };
    std::mutex g_fnamePoolMutex;
    std::unordered_map<std::string, UE::FName> g_fnamePoolIndex;
    bool g_fnamePoolIndexed = false;

    template <typename T> T Read(void* base, int off)
    { return *reinterpret_cast<T*>((uint8_t*)base + off); }

    struct ObjectIndexHint
    {
        const char* Name;
        int Index;
    };

    constexpr ObjectIndexHint kObjectIndexHints[] =
    {
        { "Function /Script/EasyBallistics.EBBarrel.GetAmmoCount", 0x156F },
        { "Function /Script/EasyBallistics.EBBarrel.SetAmmo", 0x1575 },
        { "Function /Script/Engine.Actor.K2_GetActorLocation", 0x16DE },
        { "Function /Script/Engine.Actor.K2_SetActorLocation", 0x16E5 },
        { "Function /Script/AtomicHeart.AHBaseCharacter.GetCurrentWeapon", 0x3681 },
        { "Function /Script/AtomicHeart.AHBaseCharacter.GetWeaponInventoryAmmoCount", 0x36AC },
        { "Function /Script/Engine.Controller.GetControlRotation", 0x3738 },
        { "Function /Script/Engine.Controller.SetIgnoreLookInput", 0x374C },
        { "Function /Script/Engine.Controller.SetIgnoreMoveInput", 0x374D },
        { "Function /Script/AtomicHeart.AHInventory.AddItemsToInventory", 0x398E },
        { "Function /Script/AtomicHeart.AHInventory.GetItemsCount", 0x3991 },
        { "Function /Script/AtomicHeart.AHInventory.SetIgnoreOverWeight", 0x3994 },
        { "Function /Script/AtomicHeart.AHPlayerCharacter.EquipWeaponByDataAsset", 0x3A7A },
        { "Function /Script/AtomicHeart.AHPlayerCharacter.FindWeaponByDataAsset", 0x3A7B },
        { "Function /Script/AtomicHeart.AHPlayerCharacter.GetCurrentWeaponInventoryAmmoCount", 0x3A80 },
        { "Function /Script/AtomicHeart.AHPlayerCharacter.GetInventoryPlayer", 0x3A84 },
        { "Function /Script/AtomicHeart.AHPlayerCharacter.InstantTakeWeapon", 0x3A92 },
        { "Function /Script/AtomicHeart.AHPlayerCharacter.TakeWeapon", 0x3AA2 },
        { "Function /Script/AtomicHeart.AHWorldStreamingSubsystem.EnableLevelStreaming", 0x3C43 },
        { "Function /Script/AtomicHeart.BaseWeapon.GetAmmoCount", 0x3E6F },
        { "Function /Script/AtomicHeart.BaseWeapon.GetAmmoInPossession", 0x3E70 },
        { "Function /Script/AtomicHeart.BaseWeapon.GetAmmoSize", 0x3E71 },
        { "Function /Script/AtomicHeart.StreamingUtils.InvalidateStreaming", 0x46CF },
        { "Function /Script/AtomicHeart.SubsystemUtils.GetAHWorldStreamingSubsystem", 0x46E7 },
        { "Function /Script/AtomicHeart.DebugSubsystem.InstantLockUnlock", 0x4098 },
        { "Function /Script/AtomicHeart.DebugSubsystem.SetInstantPuzzleResolve", 0x40BD },
        { "Function /Script/AtomicHeart.DebugSubsystem.WinQTE", 0x40F1 },
        // --- Prewarm cold-scan eliminators -----------------------------------
        // These /Script (engine-native) UFunctions were the ones the runtime log
        // showed Prewarm resolving via a FULL GObjects scan (650-900ms each) on a
        // cold cache, because they were missing from the hint table. Native
        // /Script functions register at engine init in a fixed order, so their
        // GObjects index is deterministic across launches -- verified identical
        // across every captured diagnostics snapshot AND the gobjects.tsv for this
        // build. Hinting them makes Prewarm resolve them instantly (index lookup,
        // no sweep). Still validated by full-name match before use, so a future
        // game patch that shifts indices just falls back to the scan -- never a
        // misresolve. (See AtomicHeartMenu_gobjects.tsv to refresh these.)
        { "Function /Script/Engine.Character.LaunchCharacter", 0x33EF },
        { "Function /Script/AIModule.AIController.MoveToActor", 0x375D },
        { "Function /Script/AtomicHeart.BaseWeapon.FullUpgrade", 0x3E6D },
        { "Function /Script/AtomicHeart.DebugSubsystem.CompleteAllActiveQuests", 0x407E },
        { "Function /Script/AtomicHeart.DebugSubsystem.PromoteAllActiveQuests", 0x40A9 },
        { "Function /Script/AIModule.AIBlueprintHelperLibrary.SimpleMoveToActor", 0x55B4 },
        { "StreamingUtils AtomicHeart.Default__StreamingUtils", 0x10855 },
        { "SubsystemUtils AtomicHeart.Default__SubsystemUtils", 0x10858 },
        { "DebugSubsystem_0", 0x12A5C },
        { "BP_WorldStreamingSubsystem_C_0", 0x134F8 },

        { "DA_Item_Shved.DA_Item_Shved", 0x12FB7 },
        { "DA_Item_Zvezdochka_DLC1.DA_Item_Zvezdochka_DLC1", 0x12FC7 },
        { "DA_Item_ShotgunKS23.DA_Item_ShotgunKS23", 0x12FD6 },
        { "DA_Item_PM_DLC1.DA_Item_PM_DLC1", 0x12FE1 },
        { "DA_Item_Zvezdochka.DA_Item_Zvezdochka", 0x13074 },
        { "DA_Item_ShotgunKS23_DLC1.DA_Item_ShotgunKS23_DLC1", 0x1300E },
        { "DA_Item_Abzac.DA_Item_Abzac", 0x13018 },
        { "DA_Item_Axe.DA_Item_Axe", 0x1301C },
        { "DA_Item_Bober.DA_Item_Bober", 0x13020 },
        { "DA_Item_Dikobraz.DA_Item_Dikobraz", 0x13022 },
        { "DA_Item_Hipar.DA_Item_Hipar", 0x1302D },
        { "DA_Item_Hirurg.DA_Item_Hirurg", 0x13032 },
        { "DA_Item_Karusel.DA_Item_Karusel", 0x13036 },
        { "DA_Item_Kilka.DA_Item_Kilka", 0x13047 },
        { "DA_Item_Lapta.DA_Item_Lapta", 0x1304D },
        { "DA_Item_Lisa.DA_Item_Lisa", 0x13052 },
        { "DA_Item_Molot.DA_Item_Molot", 0x13054 },
        { "DA_Item_Neptun.DA_Item_Neptun", 0x13058 },
        { "DA_Item_Pashtet.DA_Item_Pashtet", 0x13065 },
        { "DA_Item_Petuh.DA_Item_Petuh", 0x1306A },
        { "DA_Item_Snejok.DA_Item_Snejok", 0x13070 },
        { "DA_Item_Zinger.DA_Item_Zinger", 0x13072 },
        { "DA_Item_Klusha.DA_Item_Klusha", 0x13081 },
        { "DA_Item_AK47.DA_Item_AK47", 0x13088 },
        { "DA_Item_BasePistol.DA_Item_BasePistol", 0x1308C },
        { "DA_Item_Bidonist.DA_Item_Bidonist", 0x13092 },
        { "DA_Item_Electro.DA_Item_Electro", 0x1309B },
        { "DA_Item_Flamethrower.DA_Item_Flamethrower", 0x1309F },
        { "DA_Item_Krepysh.DA_Item_Krepysh", 0x130A9 },
        { "DA_Item_Machinegun.DA_Item_Machinegun", 0x130AD },
        { "DA_Item_PM.DA_Item_PM", 0x130B0 },
        { "DA_Item_PTRD.DA_Item_PTRD", 0x130B2 },
        { "DA_Item_Shprits.DA_Item_Shprits", 0x130BA },
        { "DA_Item_Signalka.DA_Item_Signalka", 0x130BC },
        { "DA_Item_Dominator.DA_Item_Dominator", 0x13152 },
        { "DA_Item_Railgun.DA_Item_Railgun", 0x1316C },
        { "DA_Item_Plasmagun.DA_Item_Plasmagun", 0x13184 },

        { "DA_PlasmagunAmmo.DA_PlasmagunAmmo", 0x13227 },
        { "DA_Item_PistolAmmo.DA_Item_PistolAmmo", 0x1322E },
        { "DA_Item_ShotgunAmmo.DA_Item_ShotgunAmmo", 0x13236 },
        { "DA_Item_AK47Ammo.DA_Item_AK47Ammo", 0x134BF },
        { "DA_Item_MachinegunAmmo.DA_Item_MachinegunAmmo", 0x134C9 },
        { "DA_Item_MediumAmmo.DA_Item_MediumAmmo", 0x134D3 },
        { "DA_Item_StrongAmmo.DA_Item_StrongAmmo", 0x134DD },
        { "DA_Item_WeakAmmo.DA_Item_WeakAmmo", 0x134E1 },
    };

    std::string LastObjectNameToken(const std::string& name)
    {
        size_t pos = name.find_last_of("./ ");
        if (pos == std::string::npos)
            return name;
        if (pos + 1 >= name.size())
            return {};
        return name.substr(pos + 1);
    }

    int PreferredObjectIndex(const std::string& name)
    {
        for (const auto& hint : kObjectIndexHints)
        {
            if (std::strcmp(hint.Name, name.c_str()) == 0)
                return hint.Index;
        }
        return -1;
    }

    bool BuildAuxPath(const char* fileName, char out[MAX_PATH])
    {
        char exePath[MAX_PATH]{};
        DWORD len = GetModuleFileNameA(nullptr, exePath, MAX_PATH);
        if (len > 0 && len < MAX_PATH)
        {
            strcpy_s(out, MAX_PATH, exePath);
            char* slash = strrchr(out, '\\');
            char* slash2 = strrchr(out, '/');
            if (slash2 && (!slash || slash2 > slash)) slash = slash2;
            if (slash)
            {
                slash[1] = '\0';
                strcat_s(out, MAX_PATH, fileName);
                return true;
            }
        }

        char tempPath[MAX_PATH]{};
        if (GetTempPathA(MAX_PATH, tempPath) > 0)
        {
            strcpy_s(out, MAX_PATH, tempPath);
            strcat_s(out, MAX_PATH, fileName);
            return true;
        }
        return false;
    }

    void SanitizeTsv(std::string& text)
    {
        for (char& c : text)
        {
            if (c == '\t' || c == '\r' || c == '\n')
                c = ' ';
        }
    }

    bool DecodeNamePoolEntry(uint8_t* entry, uint32_t remainingBytes, std::string& out, uint32_t& advanceBytes)
    {
        out.clear();
        advanceBytes = Offsets::O_NamePool_Stride;
        if (remainingBytes < 2 || !Mem::IsReadable(entry, 2))
            return false;

        uint16_t header = *reinterpret_cast<uint16_t*>(entry + Offsets::O_NameEntry_Header);
        int nameLen = header >> 6;
        bool wide = (header & 1) != 0;
        if (nameLen <= 0 || nameLen > 1024)
            return false;

        uint32_t dataBytes = (uint32_t)nameLen * (wide ? 2u : 1u);
        uint32_t rawSize = 2u + dataBytes;
        uint32_t stride = (uint32_t)Offsets::O_NamePool_Stride;
        advanceBytes = (rawSize + stride - 1u) & ~(stride - 1u);
        if (advanceBytes > remainingBytes)
            return false;

        char* strData = reinterpret_cast<char*>(entry + 2);
        if (!Mem::IsReadable(strData, dataBytes))
            return false;

        if (wide)
        {
            int n = WideCharToMultiByte(CP_UTF8, 0, reinterpret_cast<wchar_t*>(strData),
                                        nameLen, nullptr, 0, nullptr, nullptr);
            if (n <= 0)
                return false;
            out.resize(n);
            WideCharToMultiByte(CP_UTF8, 0, reinterpret_cast<wchar_t*>(strData),
                                nameLen, out.data(), n, nullptr, nullptr);
        }
        else
        {
            out.assign(strData, nameLen);
        }
        return !out.empty();
    }

    bool BuildFNamePoolIndex()
    {
        std::lock_guard<std::mutex> lock(g_fnamePoolMutex);
        if (g_fnamePoolIndexed)
            return true;
        if (!Mem::IsReadable(g_GNames, Offsets::O_NamePool_BlocksOffset + sizeof(void*)))
            return false;

        constexpr uint32_t kMaxBlocks = 8192;
        constexpr uint32_t kBlockBytes = 0x20000; // FNameEntryId offsets are 2-byte aligned in this build.
        uint32_t currentBlock = Read<uint32_t>(g_GNames, 0x08);
        uint32_t currentCursor = Read<uint32_t>(g_GNames, 0x0C);
        if (currentBlock >= kMaxBlocks)
            return false;
        if (currentCursor > kBlockBytes)
            currentCursor = kBlockBytes;

        ULONGLONG startMs = GetTickCount64();
        uint8_t** blocks = reinterpret_cast<uint8_t**>(g_GNames + Offsets::O_NamePool_BlocksOffset);
        size_t entries = 0;

        for (uint32_t block = 0; block <= currentBlock; ++block)
        {
            if (!Mem::IsReadable(blocks + block, sizeof(void*)))
                break;
            uint8_t* blockPtr = blocks[block];
            if (!Mem::IsReadable(blockPtr, 0x100))
                break;

            uint32_t end = (block == currentBlock) ? currentCursor : kBlockBytes;
            for (uint32_t byte = 0; byte + 2 <= end; )
            {
                std::string name;
                uint32_t advance = Offsets::O_NamePool_Stride;
                if (DecodeNamePoolEntry(blockPtr + byte, end - byte, name, advance))
                {
                    UE::FName fname{ (int32_t)((block << 16) | (byte / (uint32_t)Offsets::O_NamePool_Stride)), 0 };
                    g_fnamePoolIndex.emplace(std::move(name), fname);
                    ++entries;
                    byte += advance;
                }
                else
                {
                    byte += Offsets::O_NamePool_Stride;
                }
            }
        }

        g_fnamePoolIndexed = !g_fnamePoolIndex.empty();
        LOG("FName pool index %s in %llums (entries=%zu unique=%zu)",
            g_fnamePoolIndexed ? "ready" : "failed",
            GetTickCount64() - startMs,
            entries,
            g_fnamePoolIndex.size());
        return g_fnamePoolIndexed;
    }

    void CacheResolvedObject(const std::string& needle, UE::UObject* object)
    {
        if (!object)
            return;
        std::lock_guard<std::mutex> lock(g_findObjectMutex);
        g_findObjectCache[needle] = object;
        int index = object->Index();
        if (index >= 0)
            g_findObjectIndexCache[needle] = index;
    }

    UE::UObject* TryCachedObjectIndex(const std::string& needle)
    {
        int index = -1;
        {
            std::lock_guard<std::mutex> lock(g_findObjectMutex);
            auto it = g_findObjectIndexCache.find(needle);
            if (it == g_findObjectIndexCache.end())
                return nullptr;
            index = it->second;
        }

        UE::UObject* object = UE::GetObjectByIndex(index);
        if (object && object->GetFullName().find(needle) != std::string::npos)
        {
            CacheResolvedObject(needle, object);
            return object;
        }
        return nullptr;
    }

    UE::UObject* TryObjectIndexHint(const std::string& needle)
    {
        int preferredIndex = PreferredObjectIndex(needle);
        if (preferredIndex < 0)
            return nullptr;

        UE::UObject* hinted = UE::GetObjectByIndex(preferredIndex);
        if (hinted && hinted->GetFullName().find(needle) != std::string::npos)
        {
            CacheResolvedObject(needle, hinted);
            return hinted;
        }

        static bool loggedHintMismatch = false;
        if (!loggedHintMismatch)
        {
            LOG("FindObject: dumped index hint mismatch for %s at index=0x%X; using name index/fallback", needle.c_str(), preferredIndex);
            loggedHintMismatch = true;
        }
        return nullptr;
    }

    UE::UObject* TryObjectNameIndex(const std::string& needle)
    {
        if (!g_nameIndexReady.load())
            return nullptr;

        std::string token = LastObjectNameToken(needle);
        std::vector<UE::UObject*> candidates;
        {
            std::lock_guard<std::mutex> lock(g_nameIndexMutex);
            auto it = g_nameIndex.find(token);
            if (it == g_nameIndex.end())
                return nullptr;
            candidates = it->second;
        }

        for (UE::UObject* object : candidates)
        {
            if (!Mem::IsReadable(object, 0x30))
                continue;
            if (object->GetFullName().find(needle) != std::string::npos)
            {
                CacheResolvedObject(needle, object);
                return object;
            }
        }
        return nullptr;
    }

    DWORD WINAPI BuildNameIndexThread(LPVOID)
    {
        ULONGLONG startMs = GetTickCount64();
        std::unordered_map<std::string, std::vector<UE::UObject*>> local;
        int n = UE::NumObjects();
        local.reserve((size_t)n / 2);

        for (int i = 0; i < n && G::running.load(); ++i)
        {
            try
            {
                UE::UObject* object = UE::GetObjectByIndex(i);
                if (!Mem::IsReadable(object, 0x30))
                    continue;

                std::string name = object->GetName();
                if (!name.empty())
                    local[name].push_back(object);
            }
            catch (...) { /* one bad UObject slot must not kill the index thread */ }
        }

        {
            std::lock_guard<std::mutex> lock(g_nameIndexMutex);
            g_nameIndex.swap(local);
        }
        g_nameIndexReady = true;
        g_nameIndexObjectCount = n;
        g_nameIndexWorld = UE::GetWorld();
        g_nameIndexBuilding = false;
        LOG("Object name index ready in %llums (objects=%d names=%zu)",
            GetTickCount64() - startMs,
            n,
            g_nameIndex.size());
        return 0;
    }
}

// ---------------------------------------------------------------------------
//  FName / FString -> std::string
// ---------------------------------------------------------------------------
std::string UE::FString::ToString() const
{
    if (!Data || Count <= 0) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, Data, Count, nullptr, 0, nullptr, nullptr);
    std::string out(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, Data, Count, out.data(), len, nullptr, nullptr);
    if (!out.empty() && out.back() == '\0') out.pop_back();
    return out;
}

std::string UE::FName::ToString() const
{
    using namespace Offsets;
    if (!Mem::IsReadable(g_GNames, 0x20)) return {};

    int32_t index  = ComparisonIndex;
    if (index < 0) return {};
    int32_t block  = index >> 16;
    int32_t offset = index & 0xFFFF;
    if (block < 0 || block >= 8192) return {};

    uint8_t** blocks = reinterpret_cast<uint8_t**>(g_GNames + O_NamePool_BlocksOffset);
    if (!Mem::IsReadable(blocks + block, sizeof(void*))) return {};
    uint8_t*  blockPtr = blocks[block];
    if (!Mem::IsReadable(blockPtr, (size_t)offset * O_NamePool_Stride + 0x10)) return {};

    uint8_t* entry = blockPtr + (offset * O_NamePool_Stride);
    uint16_t header = *reinterpret_cast<uint16_t*>(entry + O_NameEntry_Header);
    int  nameLen = header >> 6;       // UE4.25+ : Len = Header >> 6
    bool wide    = header & 1;
    if (nameLen <= 0 || nameLen > 1024) return {};

    char* strData = reinterpret_cast<char*>(entry + 2);
    if (!Mem::IsReadable(strData, (size_t)nameLen * (wide ? 2 : 1))) return {};

    std::string result;
    if (wide)
    {
        wchar_t* w = reinterpret_cast<wchar_t*>(strData);
        int n = WideCharToMultiByte(CP_UTF8, 0, w, nameLen, nullptr, 0, nullptr, nullptr);
        result.resize(n);
        WideCharToMultiByte(CP_UTF8, 0, w, nameLen, result.data(), n, nullptr, nullptr);
    }
    else
    {
        result.assign(strData, nameLen);
    }

    if (Number > 0) result += "_" + std::to_string(Number - 1);
    return result;
}

// ---------------------------------------------------------------------------
//  UObject helpers
// ---------------------------------------------------------------------------
std::string UE::UObject::GetName()
{
    if (!Mem::IsReadable(this, Offsets::O_UObject_Name + sizeof(FName))) return {};
    return NamePtr()->ToString();
}

std::string UE::UObject::GetFullName()
{
    if (!Mem::IsReadable(this, 0x30)) return {};
    UObject* cls = Class();
    if (!Mem::IsReadable(cls, 0x30)) return {};

    std::string name = cls->GetName() + " ";
    std::string path;
    // hard depth cap: a garbage / cyclic Outer chain must never spin forever.
    UObject* o = Outer();
    for (int depth = 0; o && depth < 64; ++depth)
    {
        if (!Mem::IsReadable(o, 0x30)) break;
        path = o->GetName() + (path.empty() ? "" : ".") + path;
        o = o->Outer();
    }
    if (!path.empty()) name += path + ".";
    name += GetName();
    return name;
}

bool UE::UObject::IsA(UObject* cmpClass)
{
    using namespace Offsets;
    if (!Mem::IsReadable(this, 0x30) || !Mem::IsReadable(cmpClass, 0x30))
        return false;

    UObject* c = Class();
    for (int depth = 0; c && depth < 256; ++depth)
    {
        if (!Mem::IsReadable(c, O_UStruct_SuperStruct + sizeof(void*)))
            return false;
        if (c == cmpClass) return true;
        c = Read<UObject*>(c, O_UStruct_SuperStruct);
    }
    return false;
}

void UE::UObject::ProcessEvent(UObject* function, void* params)
{
    using Fn = void(*)(void*, void*, void*);
    if (!Mem::IsReadable(this, sizeof(void*)) || !Mem::IsReadable(function, sizeof(void*)))
        return;
    void** vt = VTable();
    if (!Mem::IsReadable(vt, (Offsets::VFUNC_PROCESSEVENT + 1) * sizeof(void*))) return;
    void* target = vt[Offsets::VFUNC_PROCESSEVENT];
    if (!Mem::IsReadable(target, 1)) return; // must point at executable code
    reinterpret_cast<Fn>(target)(this, function, params);
}

// ---------------------------------------------------------------------------
//  Globals resolution
// ---------------------------------------------------------------------------
namespace
{
    // Confirm GObjects+GNames really point at the engine by checking that the
    // first batch of objects resolve to the well-known core UE4 type names.
    // Garbage addresses will essentially never produce these strings.
    bool ValidateSdk()
    {
        if (!g_GObjects || !g_GNames || !g_GWorld) return false;

        int n = UE::NumObjects();
        if (n < 64 || n > 20'000'000) { LOG("ValidateSdk: NumObjects=%d out of range", n); return false; }

        int checked = 0, coreHits = 0;
        for (int i = 0; i < 512 && i < n; ++i)
        {
            UE::UObject* o = UE::GetObjectByIndex(i);
            if (!Mem::IsReadable(o, 0x30)) continue;
            std::string name = o->GetName();
            if (name.empty()) continue;
            ++checked;
            if (name == "Object" || name == "Class" || name == "Package" ||
                name == "Field"  || name == "Struct"|| name == "Function"||
                name == "Property" || name == "ArrayProperty" || name == "Enum")
                ++coreHits;
        }
        LOG("ValidateSdk: checked=%d coreHits=%d", checked, coreHits);
        return checked > 32 && coreHits >= 3;
    }
}

bool UE::ResolveGlobals()
{
    using namespace Offsets;

    if (USE_STATIC_OFFSETS)
    {
        g_GObjects = G::moduleBase + GObjects_RVA;
        g_GNames   = G::moduleBase + GNames_RVA;
        g_GWorld   = reinterpret_cast<UObject**>(G::moduleBase + GWorld_RVA);
    }
    else
    {
        g_GObjects = Scanner::FindRipRel(SIG_GOBJECTS, 3, 7);
        g_GNames   = Scanner::FindRipRel(SIG_GNAMES,   3, 7);
        g_GWorld   = reinterpret_cast<UObject**>(Scanner::FindRipRel(SIG_GWORLD, 3, 7));
    }

    LOG("ResolveGlobals: GObjects=%p GNames=%p GWorld=%p", g_GObjects, g_GNames, (void*)g_GWorld);

    bool ok = false;
    try { ok = ValidateSdk(); }   // /EHa: also traps access violations
    catch (...) { ok = false; LOG("ResolveGlobals: exception during validation"); }

    G::sdkReady = ok;
    LOG("ResolveGlobals: %s", ok ? "VALID" : "INVALID (signatures/offsets wrong for this build)");
    return ok;
}

UE::UObject* UE::GetWorld()
{
    return Mem::IsReadable(g_GWorld, sizeof(UObject*)) ? *g_GWorld : nullptr;
}

// ---------------------------------------------------------------------------
//  GObjects iteration
// ---------------------------------------------------------------------------
int UE::NumObjects()
{
    if (!Mem::IsReadable(g_GObjects, Offsets::O_ObjArray_NumElements + 4)) return 0;
    return Read<int32_t>(g_GObjects, Offsets::O_ObjArray_NumElements);
}

UE::UObject* UE::GetObjectByIndex(int index)
{
    using namespace Offsets;
    if (!Mem::IsReadable(g_GObjects, O_ObjArray_NumChunks + 4) || index < 0) return nullptr;

    uint8_t* objectsPtr = Read<uint8_t*>(g_GObjects, O_ObjArray_Objects); // FUObjectItem**
    int chunkIndex = index / NumElementsPerChunk;
    int within     = index % NumElementsPerChunk;
    int numObjects = NumObjects();
    int numChunks = Read<int32_t>(g_GObjects, O_ObjArray_NumChunks);
    if (index >= numObjects || chunkIndex >= numChunks) return nullptr;

    if (!Mem::IsReadable(objectsPtr + chunkIndex * sizeof(void*), sizeof(void*))) return nullptr;
    uint8_t* chunk = reinterpret_cast<uint8_t**>(objectsPtr)[chunkIndex];
    if (!Mem::IsReadable(chunk, SIZE_FUObjectItem)) return nullptr;

    uint8_t* item = chunk + (size_t)within * SIZE_FUObjectItem;
    if (!Mem::IsReadable(item, O_ObjectItem_Object + 8)) return nullptr;
    return Read<UObject*>(item, O_ObjectItem_Object);
}

UE::UObject* UE::FindObject(const char* name)
{
    if (!name || !*name)
        return nullptr;

    std::string needle(name);
    if (UObject* fast = FindObjectFast(name))
        return fast;

    std::string token = LastObjectNameToken(needle);
    ULONGLONG startMs = GetTickCount64();
    int candidates = 0;

    std::lock_guard<std::mutex> lock(g_findObjectMutex);
    auto it = g_findObjectCache.find(needle);
    if (it != g_findObjectCache.end() && Mem::IsReadable(it->second, 0x30))
        return it->second;

    int n = NumObjects();
    for (int i = 0; i < n; ++i)
    {
        // Per-slot guard: every read below is IsReadable-gated, but the region
        // cache trades a tiny staleness window for its huge speedup, so a slot
        // freed mid-scan could still fault. /EHa turns that into a catchable
        // throw here -> we skip the one poisoned slot instead of aborting the
        // whole resolve. Matches the per-object guards in the other scan loops.
        try
        {
            UObject* o = GetObjectByIndex(i);
            if (!o) continue;

            if (!token.empty())
            {
                std::string objectName = o->GetName();
                if (objectName != token)
                    continue;
            }

            ++candidates;
            if (o->GetFullName().find(needle) != std::string::npos)
            {
                g_findObjectCache[needle] = o;
                int objectIndex = o->Index();
                if (objectIndex >= 0)
                    g_findObjectIndexCache[needle] = objectIndex;
                ULONGLONG elapsed = GetTickCount64() - startMs;
                if (elapsed > 250)
                    LOG("FindObject slow: %s took %llums scanned=%d candidates=%d", needle.c_str(), elapsed, n, candidates);
                return o;
            }
        }
        catch (...) { /* one poisoned slot must not abort the whole resolve */ }
    }

    ULONGLONG elapsed = GetTickCount64() - startMs;
    if (elapsed > 250)
        LOG("FindObject missing slow: %s took %llums scanned=%d candidates=%d", needle.c_str(), elapsed, n, candidates);
    return nullptr;
}

UE::UObject* UE::FindObjectFast(const char* name)
{
    if (!name || !*name)
        return nullptr;

    std::string needle(name);
    {
        std::lock_guard<std::mutex> lock(g_findObjectMutex);
        auto it = g_findObjectCache.find(needle);
        if (it != g_findObjectCache.end() && Mem::IsReadable(it->second, 0x30))
            return it->second;
    }

    if (UObject* indexedCache = TryCachedObjectIndex(needle))
        return indexedCache;

    if (UObject* hinted = TryObjectIndexHint(needle))
        return hinted;

    if (UObject* indexed = TryObjectNameIndex(needle))
        return indexed;

    StartObjectNameIndex();
    return nullptr;
}

bool UE::WriteObjectMapDump(const char* reason)
{
    char path[MAX_PATH]{};
    if (!BuildAuxPath("AtomicHeartMenu_gobjects.tsv", path))
        return false;

    FILE* f = nullptr;
    fopen_s(&f, path, "w");
    if (!f)
    {
        LOG("Object map dump failed: cannot open %s", path);
        return false;
    }

    ULONGLONG startMs = GetTickCount64();
    int n = NumObjects();
    fprintf(f, "# reason=%s\n", reason ? reason : "");
    fprintf(f, "# module_base=%p module_size=0x%zX objects=%d\n", (void*)G::moduleBase, G::moduleSize, n);
    fprintf(f, "index\tobject\tclass\tname\tfull_name\n");

    int written = 0;
    for (int i = 0; i < n && G::running.load(); ++i)
    {
        UObject* object = GetObjectByIndex(i);
        if (!Mem::IsReadable(object, 0x30))
            continue;

        std::string className;
        std::string objectName;
        std::string fullName;
        try
        {
            UObject* cls = object->Class();
            if (Mem::IsReadable(cls, 0x30))
                className = cls->GetName();
            objectName = object->GetName();
            fullName = object->GetFullName();
        }
        catch (...) { continue; }

        SanitizeTsv(className);
        SanitizeTsv(objectName);
        SanitizeTsv(fullName);
        fprintf(f, "%d\t%p\t%s\t%s\t%s\n",
            i, (void*)object, className.c_str(), objectName.c_str(), fullName.c_str());
        ++written;
    }

    fclose(f);
    LOG("Object map dump written: %s rows=%d time=%llums",
        path, written, GetTickCount64() - startMs);
    return true;
}

void UE::StartObjectNameIndex()
{
    if (g_nameIndexReady.load() || g_nameIndexBuilding.exchange(true))
        return;

    g_nameIndexReady = false;

    HANDLE thread = CreateThread(nullptr, 0, BuildNameIndexThread, nullptr, 0, nullptr);
    if (thread)
    {
        CloseHandle(thread);
    }
    else
    {
        g_nameIndexBuilding = false;
        LOG("Object name index thread create failed err=%lu", GetLastError());
    }
}

bool UE::TryGetFName(const char* shortName, FName& out)
{
    if (!shortName || !*shortName)
        return false;

    // The name index keys on GetName(), which is exactly the short name for a
    // Number==0 FName, so an exact-key hit gives us an object carrying that FName.
    if (!g_nameIndexReady.load())
    {
        StartObjectNameIndex();
    }
    else
    {
        std::vector<UObject*> candidates;
        {
            std::lock_guard<std::mutex> lock(g_nameIndexMutex);
            auto it = g_nameIndex.find(shortName);
            if (it != g_nameIndex.end())
                candidates = it->second;
        }

        for (UObject* o : candidates)
        {
            if (!Mem::IsReadable(o, Offsets::O_UObject_Name + (int)sizeof(FName)))
                continue;
            FName fn = *o->NamePtr();
            // Only accept a Number==0 FName so the parameter name matches exactly
            // (GetName() appends "_N" for Number>0, so an exact key already implies 0,
            // but guard anyway).
            if (fn.Number == 0)
            {
                out = fn;
                return true;
            }
        }
    }

    // Material parameter names often exist only as FNames embedded in loaded
    // material packages, not as standalone UObjects named "EmissiveColor", etc.
    if (BuildFNamePoolIndex())
    {
        std::lock_guard<std::mutex> lock(g_fnamePoolMutex);
        auto it = g_fnamePoolIndex.find(shortName);
        if (it != g_fnamePoolIndex.end())
        {
            out = it->second;
            return true;
        }
    }
    return false;
}

UE::UClass* UE::FindClass(const char* className)
{
    int n = NumObjects();
    for (int i = 0; i < n; ++i)
    {
        UObject* o = GetObjectByIndex(i);
        if (!o || !o->Class()) continue;
        if (o->Class()->GetName() == "Class" && o->GetName() == className) return o;
    }
    return nullptr;
}

UE::UFunction* UE::FindFunction(const char* funcFullName)
{
    return FindObject(funcFullName); // full name already disambiguates Function
}

UE::UFunction* UE::FindFunctionInClass(UClass* cls, const char* shortName)
{
    using namespace Offsets;
    if (!shortName || !*shortName) return nullptr;
    if (!Mem::IsReadable(cls, O_UStruct_Children + sizeof(void*))) return nullptr;

    // Children is a singly-linked UField list (functions/enums/nested structs),
    // traversed via UField::Next. Functions defined on this exact class live here.
    UObject* child = Read<UObject*>(cls, O_UStruct_Children);
    for (int depth = 0; child && depth < 8192; ++depth)
    {
        if (!Mem::IsReadable(child, O_UField_Next + sizeof(void*))) break;
        if (child->GetName() == shortName)
            return child;
        child = Read<UObject*>(child, O_UField_Next);
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
//  Local player chain (engine-generic; offsets in offsets.h)
// ---------------------------------------------------------------------------
UE::UObject* UE::GetGameInstance()
{
    UObject* world = GetWorld();
    if (!Mem::IsReadable(world, Offsets::O_World_GameInstance + 8)) return nullptr;
    return Read<UObject*>(world, Offsets::O_World_GameInstance);
}

UE::UObject* UE::GetLocalPlayer()
{
    UObject* gi = GetGameInstance();
    if (!Mem::IsReadable(gi, Offsets::O_GameInst_LocalPlayers + sizeof(TArray<UObject*>))) return nullptr;
    auto* players = reinterpret_cast<TArray<UObject*>*>((uint8_t*)gi + Offsets::O_GameInst_LocalPlayers);
    if (!Mem::IsReadable(players->Data, sizeof(void*)) || !players->IsValid(0)) return nullptr;
    return (*players)[0];
}

UE::UObject* UE::GetPlayerController()
{
    UObject* lp = GetLocalPlayer();
    if (!Mem::IsReadable(lp, Offsets::O_Player_PlayerController + 8)) return nullptr;
    return Read<UObject*>(lp, Offsets::O_Player_PlayerController);
}

UE::UObject* UE::GetLocalPawn()
{
    UObject* pc = GetPlayerController();
    if (!Mem::IsReadable(pc, Offsets::O_Controller_Pawn + 8)) return nullptr;
    return Read<UObject*>(pc, Offsets::O_Controller_Pawn);
}
