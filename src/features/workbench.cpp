#include "../../deps/nlohmann/json.hpp"
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Skorchekd. Preserve attribution to Skorchekd, Dumper-7
// (Encryqed), MinHook (Tsuda Kageyu), and Dear ImGui (ocornut). See LICENSE/NOTICE.
#include "workbench.h"
#include "features.h"
#include "../sdk/reflect.h"
#include "../core/memory.h"
#include "../core/globals.h"
#include "../core/log.h"
#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <functional>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <vector>

namespace
{
    using namespace UE;
    std::mutex statusMutex;
    Workbench::Status status;
    std::atomic<bool> tickPending{ false };
    std::atomic<bool> active{ false };

    void Report(const std::string& text)
    {
        std::lock_guard<std::mutex> lock(statusMutex);
        status.message = text;
        LOG("Workbench: %s", text.c_str());
    }

    // Index alone cannot distinguish an object recycled into the same slot.
    // These references also verify the object's engine-assigned name.
    struct ObjectRef
    {
        UObject* ptr = nullptr;
        int index = -1;
        FName name{};
        ObjectRef() = default;
        explicit ObjectRef(UObject* object)
        {
            if (IsLiveObject(object))
            {
                ptr = object;
                index = object->Index();
                name = *object->NamePtr();
            }
        }
        UObject* Get() const
        {
            if (!IsLiveObject(ptr) || ptr->Index() != index) return nullptr;
            FName current = *ptr->NamePtr();
            return current.ComparisonIndex == name.ComparisonIndex && current.Number == name.Number ? ptr : nullptr;
        }
    };
    ObjectRef nora;
    ObjectRef noraWorld;
    ULONGLONG noraSpawnMs = 0;
    ObjectRef skillOwner;
    ObjectRef objectiveOwner;
    ObjectRef originalObjective;
    bool objectiveHidden = false;
    int nextSkillCategory = 0;
    int stageSearchIndex = -1;
    bool requestOpenWorld = false;
    ObjectRef stageWidget;
    std::atomic<UObject*> stageReceiver{ nullptr };
    std::atomic<UFunction*> shippingQuery{ nullptr };
    std::atomic<int> shippingReturnOffset{ -1 };

    template<typename T> bool ReadAt(const void* object, int offset, T& out)
    {
        if (!object || offset < 0) return false;
        const auto* field = static_cast<const uint8_t*>(object) + offset;
        if (!Mem::IsReadable(field, sizeof(T))) return false;
        std::memcpy(&out, field, sizeof(T));
        return true;
    }

    UFunction* Function(UObject* receiver, const char* name)
    {
        if (!IsLiveObject(receiver)) return nullptr;
        UObject* cls = receiver->Class();
        for (int depth = 0; depth < 64 && IsLiveObject(cls); ++depth)
        {
            if (auto* fn = FindFunctionInClass(cls, name))
                return IsLiveObject(fn) ? fn : nullptr;
            UObject* parent = nullptr;
            if (!ReadAt(cls, Offsets::O_UStruct_SuperStruct, parent)) break;
            cls = parent;
        }
        return nullptr;
    }

    // Allocate the runtime reflected frame, including Blueprint local storage.
    // Parameters are resolved by name, so a shifted member is not a guessed write.
    struct Call
    {
        UObject* receiver;
        UFunction* fn;
        std::vector<uint8_t> data;
        bool valid = false;
        Call(UObject* object, const char* name) : receiver(object), fn(Function(object, name))
        {
            int size = 0;
            if (!fn || !ReadAt(fn, Offsets::O_UStruct_PropertiesSize, size) || size < 0 || size > 65536) return;
            data.resize((std::max)(size, 1));
            valid = true;
        }
        template<typename T> bool Set(const char* name, const T& value)
        {
            int off = -1, size = 0, dim = 0;
            if (!valid || !Reflect::PropertyLayoutInStruct(fn, name, off, size, dim) ||
                dim != 1 || size != sizeof(T) || size_t(off) + sizeof(T) > data.size())
            { valid = false; return false; }
            std::memcpy(data.data() + off, &value, sizeof(T));
            return true;
        }
        template<typename T> bool Get(const char* name, T& value) const
        {
            int off = -1, size = 0, dim = 0;
            if (!valid || !Reflect::PropertyLayoutInStruct(fn, name, off, size, dim) ||
                dim != 1 || size != sizeof(T) || size_t(off) + sizeof(T) > data.size()) return false;
            std::memcpy(&value, data.data() + off, sizeof(T));
            return true;
        }
        bool Run()
        {
            if (!valid) LOG("Workbench call unavailable: receiver=%p function=%p", receiver, fn);
            return valid && IsLiveObject(receiver) && IsLiveObject(fn) && receiver->ProcessEvent(fn, data.data());
        }
    };

    bool BoolResult(UObject* object, const char* method, bool& result)
    {
        Call call(object, method);
        return call.Run() && call.Get("ReturnValue", result);
    }
    bool BoolField(UObject* object, const char* name, bool& value)
    {
        return IsLiveObject(object) && ReadAt(object, Reflect::FindPropertyOffset(object, name), value);
    }
    bool WriteBoolField(UObject* object, const char* name, bool value)
    {
        int off = IsLiveObject(object) ? Reflect::FindPropertyOffset(object, name) : -1;
        if (off < 0 || !Mem::IsReadable(reinterpret_cast<uint8_t*>(object) + off, 1)) return false;
        // Only full-byte Blueprint bool fields from BP_Base_CraftMachine are used.
        std::memcpy(reinterpret_cast<uint8_t*>(object) + off, &value, 1);
        return true;
    }

    bool NoraBusy(UObject* actor)
    {
        bool inScene = false;
        if (!BoolField(actor, "InScen", inScene) || inScene) return true;
        UObject* craft = Reflect::ReadNamedObjectProperty(actor, "CraftWindow");
        if (IsLiveObject(craft))
        {
            bool visible = false;
            if (!BoolResult(craft, "IsInViewport", visible) || visible) return true;
        }
        UObject* dialogue = Reflect::ReadNamedObjectProperty(actor, "DialogueCharacter");
        if (!IsLiveObject(dialogue)) return true;
        // InScen covers the crafting scene, but remains false in Nora's choices.
        UObject* current = nullptr;
        Call playerDialogue(GetLocalPawn(), "GetCurrentDialogComponent");
        if (!playerDialogue.Run() || !playerDialogue.Get("ReturnValue", current)) return true;
        if (current == dialogue) return true;
        UObject* widget = nullptr;
        Call dialogueWidget(dialogue, "GetCurrentWidget");
        if (!dialogueWidget.Run() || !dialogueWidget.Get("ReturnValue", widget)) return true;
        if (!IsLiveObject(widget)) return false;
        bool visible = false;
        return !BoolResult(widget, "IsVisible", visible) || visible;
    }

    bool NoraReady(UObject* actor)
    {
        bool loaded = false;
        UObject* mesh = Reflect::ReadNamedObjectProperty(actor, "CraftMachineSkeletal");
        return IsLiveObject(actor) && IsLiveObject(mesh) &&
            IsLiveObject(Reflect::ReadNamedObjectProperty(mesh, "SkeletalMesh")) &&
            IsLiveObject(Reflect::ReadNamedObjectProperty(mesh, "AnimScriptInstance")) &&
            IsLiveObject(Reflect::ReadNamedObjectProperty(actor, "DialogueCharacter")) &&
            IsLiveObject(Reflect::ReadNamedObjectProperty(actor, "Interactive")) &&
            BoolResult(actor, "AreItemsFromSettingsLoaded", loaded) && loaded;
    }

    void Queue(std::function<void()> action)
    {
        {
            std::lock_guard<std::mutex> lock(statusMutex);
            if (status.busy) return;
            status.busy = true;
        }
        if (!Features::QueueGameAction([action = std::move(action)]()
        {
            try { action(); }
            catch (...) { Report("Game action failed. No automatic retry; inspect the log."); }
            std::lock_guard<std::mutex> lock(statusMutex);
            status.busy = false;
        }))
        {
            std::lock_guard<std::mutex> lock(statusMutex);
            status.busy = false;
            status.message = "Game-thread queue unavailable. Try again in a loaded game.";
        }
    }

    struct alignas(16) Transform
    {
        float rotation[4]{ 0, 0, 0, 1 };
        FVector translation{};
        float pad = 0;
        FVector scale{ 1, 1, 1 };
        float pad2 = 0;
    };
    static_assert(sizeof(Transform) == 0x30, "September 2026 SDK FTransform");

    struct PlacementHit
    {
        int face = 0;
        float time = 0, distance = 0;
        FVector location{}, impact{}, normal{}, impactNormal{}, traceStart{}, traceEnd{};
        float penetration = 0;
        int item = 0;
        uint8_t element = 0, flags = 0, padding[2]{};
        uint8_t references[0x28]{};
    };
    static_assert(sizeof(PlacementHit) == 0x88, "Fresh SDK FHitResult size");

    bool TracePlacement(const FVector& start, const FVector& end, PlacementHit& hit, bool& blocked)
    {
        UObject* library = FindObjectFast("KismetSystemLibrary /Script/Engine.Default__KismetSystemLibrary");
        UObject* ignored[] = { GetLocalPawn(), nora.Get() };
        TArray<UObject*> ignore{};
        ignore.Data = ignored; ignore.Count = ignore.Max = ignored[1] ? 2 : 1;
        Call trace(library, "LineTraceSingle");
        trace.Set("WorldContextObject", GetWorld());
        trace.Set("Start", start); trace.Set("End", end);
        trace.Set("TraceChannel", uint8_t(0));
        trace.Set("bTraceComplex", false);
        trace.Set("ActorsToIgnore", ignore);
        trace.Set("DrawDebugType", uint8_t(0));
        trace.Set("bIgnoreSelf", true);
        return trace.Run() && trace.Get("OutHit", hit) && trace.Get("ReturnValue", blocked);
    }

    bool Placement(Transform& transform)
    {
        UObject* pawn = GetLocalPawn();
        Call location(pawn, "K2_GetActorLocation");
        Call rotation(GetPlayerController(), "GetControlRotation");
        FVector pos{};
        FRotator rot{};
        if (!location.Run() || !location.Get("ReturnValue", pos) ||
            !rotation.Run() || !rotation.Get("ReturnValue", rot)) return false;
        float halfHeight = 0;
        UObject* capsule = Reflect::ReadNamedObjectProperty(pawn, "CapsuleComponent");
        Call height(capsule, "GetScaledCapsuleHalfHeight");
        if (!height.Run() || !height.Get("ReturnValue", halfHeight) ||
            !std::isfinite(halfHeight) || halfHeight <= 0 || halfHeight > 1000) return false;
        constexpr float radians = 0.01745329252f;
        float yaw = rot.Yaw * radians;
        transform.translation = { pos.X + std::cos(yaw) * 300, pos.Y + std::sin(yaw) * 300, pos.Z - halfHeight + 5 };
        if (!std::isfinite(pos.X) || !std::isfinite(pos.Y) || !std::isfinite(pos.Z) || !std::isfinite(yaw)) return false;
        // Find an actual surface near the player's floor, including on slopes.
        PlacementHit floor{}; bool blocked = false;
        FVector above = transform.translation, below = transform.translation;
        above.Z += 180; below.Z -= 240;
        if (!TracePlacement(above, below, floor, blocked) || !blocked ||
            (floor.flags & 2) || !std::isfinite(floor.impact.Z) || floor.impactNormal.Z < 0.8f) return false;
        transform.translation.Z = floor.impact.Z + 5;
        // Reject placement through a wall/window. Construction still owns final
        // overlap adjustment for Nora's composite trigger boxes.
        FVector sightStart = pos, sightEnd = transform.translation;
        sightStart.Z += 30; sightEnd.Z += halfHeight + 30;
        PlacementHit obstruction{};
        if (!TracePlacement(sightStart, sightEnd, obstruction, blocked) || blocked) return false;
        transform.rotation[2] = std::sin((yaw + 3.141592654f) * 0.5f);
        transform.rotation[3] = std::cos((yaw + 3.141592654f) * 0.5f);
        return std::isfinite(pos.X) && std::isfinite(pos.Y) && std::isfinite(pos.Z) && std::isfinite(yaw);
    }

    void SpawnNoraImpl()
    {
        Transform transform;
        if (!Placement(transform)) { Report("No clear, level spot for Nora ahead. Face open floor nearby and retry."); return; }
        if (UObject* actor = nora.Get())
        {
            if (noraWorld.Get() != GetWorld()) { nora = {}; }
            else
            {
                if (NoraBusy(actor)) { Report("Close Nora's crafting/skills screen before moving her."); return; }
                Call move(actor, "K2_SetActorTransform");
                move.Set("NewTransform", transform);
                move.Set("bSweep", true);
                move.Set("bTeleport", true);
                bool moved = false;
                if (move.Run() && move.Get("ReturnValue", moved) && moved)
                    Report("Portable Nora moved nearby. Interact normally to use her.");
                else Report("Nora could not move here. Choose a clear, level space.");
                return;
            }
        }
        UObject* cls = FindObjectFast("BP_Base_CraftMachine_C");
        UObject* nativeClass = FindObjectFast("Class /Script/AtomicHeart.AHCraftMachine");
        UObject* statics = FindObjectFast("GameplayStatics /Script/Engine.Default__GameplayStatics");
        // Never spawn the unconfigured native AHCraftMachine fallback.
        if (!IsLiveObject(cls) || cls->GetName() != "BP_Base_CraftMachine_C" ||
            !IsLiveObject(nativeClass) || !IsLiveObject(statics))
        { Report("Nora's Blueprint is not loaded. Visit a normal Nora once, then retry."); return; }
        Call begin(statics, "BeginDeferredActorSpawnFromClass");
        begin.Set("WorldContextObject", GetWorld());
        begin.Set("ActorClass", cls);
        begin.Set("SpawnTransform", transform);
        begin.Set("CollisionHandlingOverride", uint8_t(2)); // engine-adjusted placement; composite Nora trigger boxes can overlap floor
        begin.Set("Owner", static_cast<UObject*>(nullptr));
        UObject* actor = nullptr;
        if (!begin.Run() || !begin.Get("ReturnValue", actor) || !IsLiveObject(actor) || !actor->IsA(nativeClass))
        { Report("Nora spawn refused. Choose a clear, level space."); return; }
        nora = ObjectRef(actor);
        noraWorld = ObjectRef(GetWorld());
        // Blueprint defaults own mesh, anim class, Wwise and dialogue components.
        // Construction/BeginPlay performs their normal initialization.
        WriteBoolField(actor, "bPlayGreetings", true);
        Call finish(statics, "FinishSpawningActor");
        finish.Set("Actor", actor);
        finish.Set("SpawnTransform", transform);
        UObject* finished = nullptr;
        bool constructionRan = finish.Run();
        bool constructionReturned = finish.Get("ReturnValue", finished);
        LOG("Nora construction: actor=%p result=%p ran=%d returned=%d live=%d flags=0x%X", actor, finished,
            constructionRan, constructionReturned, IsLiveObject(actor),
            Mem::IsReadable(actor, 0x10) ? *reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(actor) + Offsets::O_UObject_Flags) : 0);
        if (!constructionRan || !constructionReturned || finished != actor || !IsLiveObject(actor))
        { Report("Nora construction did not complete; remove her before retrying."); active = true; return; }
        // Reversed on build 24534183: the loader creates the two streamable
        // handles checked by AreItemsFromSettingsLoaded; no wait loop here.
        UObject* settings = Reflect::ReadNamedObjectProperty(actor, "Settings");
        if (!IsLiveObject(settings))
        { Report("Nora has no valid crafting settings; interaction is disabled."); active = true; return; }
        Call preload(actor, "LoadItemsFromSettings");
        ULONGLONG start = GetTickCount64();
        if (!preload.Run()) Report("Nora asset preloading was unavailable.");
        LOG("Nora: LoadItemsFromSettings returned in %llums", GetTickCount64() - start);
        if (!IsLiveObject(actor)) { nora = {}; Report("Nora was rejected during construction. Move to a clear open space and retry."); return; }
        noraSpawnMs = GetTickCount64();
        active = true;
        Report("Nora spawned. Waiting for her normal asset initialization.");
    }

    struct PricePatch
    {
        ObjectRef owner;
        const char* property = nullptr;
        uint8_t* arrayData = nullptr;
        int count = 0;
        std::vector<uint64_t> identities; // ItemDataAsset pointer and ItemId
        std::vector<int32_t> originals;
        std::vector<int32_t> applied;
    };
    std::unordered_map<uint8_t*, PricePatch> patches;
    float multiplier = 1.0f;
    int restoredValues = 0, skippedRestoreValues = 0, restoreMismatches = 0;

    struct PriceHint
    {
        int index = -1;
        int kind = 0;
        std::string name;
        unsigned generation = 0;
    };
    const char* PriceProperty(int kind)
    {
        static const char* names[] = { "ItemsToCraft", "Price", "PriceToBeOwned" };
        return kind >= 0 && kind < 3 ? names[kind] : nullptr;
    }
    std::mutex priceQueueMutex;
    std::vector<PriceHint> priceQueue;
    std::atomic<unsigned> priceGeneration{ 0 };
    std::atomic<bool> priceDiscoveryEnabled{ false };
    std::atomic<int> priceScanProgress{ 100 };
    bool refreshCrafting = false;
    bool unlockRecipes = false;
    ObjectRef recipeInventory;
    int recipesLearned = 0, recipeFailures = 0;

    UObject* PlayerInventory()
    {
        UObject* inventory = nullptr;
        Call get(GetLocalPawn(), "GetInventoryPlayer");
        return get.Run() && get.Get("ReturnValue", inventory) && IsLiveObject(inventory) ? inventory : nullptr;
    }
    bool HasRecipe(UObject* inventory, UObject* item, bool& result)
    {
        Call query(inventory, "HasRecipeFor");
        query.Set("InItemDataAsset", item);
        return query.Run() && query.Get("ReturnValue", result);
    }
    void LearnRecipe(UObject* asset)
    {
        if (!unlockRecipes || !IsLiveObject(asset) || asset->GetName().find("Default__") == 0) return;
        uint8_t category = 0;
        if (!ReadAt(asset, Reflect::FindPropertyOffset(asset, "ItemCategory"), category) || category != 10) return;
        UObject* item = Reflect::ReadNamedObjectProperty(asset, "RecipeDataAsset");
        UObject* inventory = recipeInventory.Get();
        bool known = false;
        if (!IsLiveObject(item) || !inventory || !HasRecipe(inventory, item, known) || known) return;
        // Build 24534183: AHInventoryPlayer virtual +0x4A0, RVA 0x1EE87E0,
        // category Recipe=10 appends RecipeDataAsset to saved PlayerItemsToCraft.
        // Let the engine allocate/notify; do not replace its TArray or fake UI flags.
        Call add(inventory, "AddItemsToInventory");
        add.Set("ItemDataAsset", asset);
        add.Set("InCount", int32_t(1));
        bool ran = add.Run();
        bool learned = false;
        if (ran && HasRecipe(inventory, item, learned) && learned)
        { ++recipesLearned; refreshCrafting = true; }
        else ++recipeFailures;
        LOG("Recipe unlock: %s learned=%d", asset->GetName().c_str(), learned);
    }

    // Called by the existing idle worker. Only names and slot hints go to disk;
    // the game thread validates identity again before changing any price.
    void DiscoverPricesWorker()
    {
        if (!priceDiscoveryEnabled.load()) return;
        static bool initialized = false;
        static std::filesystem::path cachePath;
        static std::string buildKey;
        static std::unordered_map<std::string, PriceHint> catalog;
        static std::unordered_map<int, std::string> published;
        static unsigned generation = 0;
        static int cursor = 0, end = 0;
        static ULONGLONG nextSweep = 0;
        static bool dirty = false;
        static UObject* world = nullptr;
        if (!initialized)
        {
            wchar_t path[32768]{};
            DWORD length = GetModuleFileNameW(static_cast<HMODULE>(G::hModule), path, 32768);
            if (length && length < 32768)
                cachePath = std::filesystem::path(path).parent_path() / "AtomicHeartMenu.cache" / "crafting.json";
            IMAGE_DOS_HEADER dos{};
            if (!ReadAt(G::moduleBase, 0, dos) || dos.e_magic != IMAGE_DOS_SIGNATURE ||
                dos.e_lfanew < 0 || dos.e_lfanew > 0x100000) return;
            IMAGE_NT_HEADERS64 nt{};
            if (!ReadAt(G::moduleBase, dos.e_lfanew, nt) || nt.Signature != IMAGE_NT_SIGNATURE) return;
            buildKey = "AHM-CRAFT-1 " + std::to_string(nt.FileHeader.TimeDateStamp) + " " +
                std::to_string(nt.OptionalHeader.SizeOfImage);
            std::error_code cacheError;
            auto bytes = std::filesystem::file_size(cachePath, cacheError);
            if (!cacheError && bytes <= 8 * 1024 * 1024)
            {
                std::ifstream input(cachePath);
                auto cached = nlohmann::json::parse(input, nullptr, false);
                if (cached.is_object() && cached.contains("build") && cached["build"].is_string() && cached.value("build", std::string{}) == buildKey &&
                    cached.contains("assets") && cached["assets"].is_array() && cached["assets"].size() <= 4096)
                {
                    for (const auto& row : cached["assets"])
                    {
                        if (!row.is_object() || !row.contains("index") || !row["index"].is_number_integer() ||
                            !row.contains("kind") || !row["kind"].is_number_integer() ||
                            !row.contains("name") || !row["name"].is_string()) continue;
                        PriceHint hint;
                        hint.index = row.value("index", -1);
                        hint.kind = row.value("kind", -1);
                        hint.name = row.value("name", std::string{});
                        if (hint.index < 0 || !PriceProperty(hint.kind) || hint.name.empty() || hint.name.size() > 2048) continue;
                        catalog[hint.name] = std::move(hint);
                    }
                }
            }
            LOG("Crafting cache: loaded %zu build-matched asset hints", catalog.size());
            initialized = true;
        }
        unsigned requested = priceGeneration.load();
        auto publish = [&](PriceHint hint)
        {
            hint.generation = requested;
            std::lock_guard<std::mutex> lock(priceQueueMutex);
            if (priceQueue.size() < 8192) priceQueue.push_back(std::move(hint));
        };
        if (requested != generation)
        {
            generation = requested;
            published.clear();
            cursor = end = 0; nextSweep = 0;
            for (const auto& entry : catalog) publish(entry.second);
        }
        if (world != GetWorld())
        {
            world = GetWorld();
            published.clear();
            cursor = end = 0; nextSweep = 0;
        }
        if (cursor >= end)
        {
            if (GetTickCount64() < nextSweep) return;
            cursor = 0;
            end = NumObjects();
        }
        UObject* classes[] = {
            FindObjectFast("Class /Script/AtomicHeart.DA_ItemBase"),
            FindObjectFast("Class /Script/AtomicHeart.WeaponUpgradeData"),
            FindObjectFast("Class /Script/AtomicHeart.SkillOwningConditionsForResources")
        };
        if (!IsLiveObject(classes[0]) || !IsLiveObject(classes[1]) || !IsLiveObject(classes[2])) return;
        auto start = std::chrono::steady_clock::now();
        for (int visited = 0; cursor < end && visited < 4096; ++visited, ++cursor)
        {
            UObject* object = GetObjectByIndex(cursor);
            if (IsLiveObject(object))
            {
                int kind = -1;
                for (int k = 0; k < 3; ++k) if (object->IsA(classes[k])) { kind = k; break; }
                if (kind >= 0)
                {
                    std::string name = object->GetFullName();
                    if (published[cursor] != name)
                    {
                        PriceHint hint{ cursor, kind, name, requested };
                        publish(hint);
                        published[cursor] = name;
                        auto old = catalog.find(name);
                        if (old == catalog.end() || old->second.index != cursor || old->second.kind != kind)
                        {
                            if (catalog.size() < 4096 || old != catalog.end())
                            { catalog[name] = std::move(hint); dirty = true; }
                        }
                    }
                }
            }
            if (std::chrono::steady_clock::now() - start > std::chrono::milliseconds(2))
            { ++cursor; break; }
        }
        // Once warm, keep the visible progress complete while a low-budget
        // worker sweep notices reused UObject slots and newly streamed assets.
        int progress = end > 0 ? int(100LL * cursor / end) : 100;
        priceScanProgress = (std::max)(priceScanProgress.load(), progress);
        if (cursor >= end)
        {
            nextSweep = GetTickCount64() + 5000;
            if (dirty && !cachePath.empty())
            {
                std::error_code error;
                std::filesystem::create_directories(cachePath.parent_path(), error);
                auto temp = cachePath; temp += ".tmp";
                nlohmann::json cached = { { "build", buildKey }, { "assets", nlohmann::json::array() } };
                for (const auto& entry : catalog)
                    cached["assets"].push_back({ { "index", entry.second.index }, { "kind", entry.second.kind }, { "name", entry.first } });
                std::ofstream output(temp, std::ios::trunc);
                output << cached.dump(2) << '\n';
                output.close();
                if (output.good() && MoveFileExW(temp.c_str(), cachePath.c_str(), MOVEFILE_REPLACE_EXISTING))
                    dirty = false;
            }
        }
    }

    constexpr size_t inventoryStride = 0x50; // FInventoryData from fresh SDK
    constexpr int amountOffset = 0x30;

    bool PriceArray(UObject* owner, const char* property, TArray<uint8_t>& array)
    {
        return IsLiveObject(owner) && ReadAt(owner, Reflect::FindPropertyOffset(owner, property), array) &&
            array.Count >= 0 && array.Count <= 128 && array.Max >= array.Count && array.Max <= 4096 &&
            (!array.Count || Mem::IsReadable(array.Data, size_t(array.Count) * inventoryStride));
    }
    void RestorePrices()
    {
        restoredValues = skippedRestoreValues = restoreMismatches = 0;
        for (auto& entry : patches)
        {
            auto& patch = entry.second;
            TArray<uint8_t> array{};
            if (!PriceArray(patch.owner.Get(), patch.property, array) || array.Data != patch.arrayData || array.Count != patch.count)
            { skippedRestoreValues += patch.count; continue; }
            for (int i = 0; i < array.Count; ++i)
            {
                auto* amount = reinterpret_cast<int32_t*>(array.Data + i * inventoryStride + amountOffset);
                uint64_t asset = 0, id = 0;
                std::memcpy(&asset, array.Data + i * inventoryStride + 0x28, 8);
                std::memcpy(&id, array.Data + i * inventoryStride + 0x38, 8);
                if (asset == patch.identities[i * 2] && id == patch.identities[i * 2 + 1] &&
                    *amount == patch.applied[i])
                {
                    *amount = patch.originals[i];
                    ++restoredValues;
                    if (*amount != patch.originals[i]) ++restoreMismatches;
                }
                else ++skippedRestoreValues;
            }
        }
        patches.clear();
        refreshCrafting = true;
        LOG("Price restoration: values=%d skipped=%d mismatches=%d", restoredValues, skippedRestoreValues, restoreMismatches);
    }
    void ChangePrice(UObject* owner, const char* property)
    {
        TArray<uint8_t> array{};
        if (!PriceArray(owner, property, array) || !array.Count || patches.size() >= 4096) return;
        auto old = patches.find(array.Data);
        if (old != patches.end())
        {
            TArray<uint8_t> previous{};
            if (PriceArray(old->second.owner.Get(), old->second.property, previous) &&
                previous.Data == array.Data && previous.Count == old->second.count) return;
            patches.erase(old); // allocator reused a dead owner's array
        }
        PricePatch patch;
        patch.owner = ObjectRef(owner);
        patch.property = property;
        patch.arrayData = array.Data;
        patch.count = array.Count;
        for (int i = 0; i < array.Count; ++i)
        {
            int32_t amount = *reinterpret_cast<int32_t*>(array.Data + i * inventoryStride + amountOffset);
            if (amount < 0 || amount > 100000000) return;
            uint64_t asset = 0, id = 0;
            std::memcpy(&asset, array.Data + i * inventoryStride + 0x28, 8);
            std::memcpy(&id, array.Data + i * inventoryStride + 0x38, 8);
            patch.identities.push_back(asset);
            patch.identities.push_back(id);
            patch.originals.push_back(amount);
            patch.applied.push_back(static_cast<int32_t>(std::ceil(double(amount) * multiplier)));
        }
        auto inserted = patches.emplace(array.Data, std::move(patch));
        refreshCrafting = true;
        for (int i = 0; i < array.Count; ++i)
            *reinterpret_cast<int32_t*>(array.Data + i * inventoryStride + amountOffset) = inserted.first->second.applied[i];
    }

    void PriceScanStep()
    {
        if (multiplier == 1.0f && !unlockRecipes) return;
        if (unlockRecipes)
        {
            UObject* inventory = PlayerInventory();
            if (!inventory) return;
            if (recipeInventory.Get() != inventory)
            {
                recipeInventory = ObjectRef(inventory);
                recipesLearned = recipeFailures = 0;
                ++priceGeneration; // replay cached assets for this player's inventory
            }
        }
        std::vector<PriceHint> batch;
        {
            std::lock_guard<std::mutex> lock(priceQueueMutex);
            size_t count = (std::min)(size_t(unlockRecipes ? 4 : 32), priceQueue.size());
            batch.insert(batch.end(), priceQueue.end() - count, priceQueue.end());
            priceQueue.resize(priceQueue.size() - count);
        }
        for (const auto& hint : batch)
        {
            if (hint.generation != priceGeneration.load()) continue;
            UObject* object = GetObjectByIndex(hint.index);
            if (!IsLiveObject(object) || object->GetFullName() != hint.name) continue;
            const char* property = PriceProperty(hint.kind);
            if (property && multiplier != 1.0f) ChangePrice(object, property);
            if (hint.kind == 0) LearnRecipe(object);
        }
    }

    UObject* ActiveCraftWindow()
    {
        UObject* actor = nora.Get();
        UObject* widget = actor ? Reflect::ReadNamedObjectProperty(actor, "CraftWindow") : nullptr;
        if (!IsLiveObject(widget)) widget = FindObjectFast("WBP_CraftWindowMain_C_0");
        bool visible = false;
        return IsLiveObject(widget) && BoolResult(widget, "IsInViewport", visible) && visible ? widget : nullptr;
    }

    void RefreshCraftingDisplay()
    {
        if (!refreshCrafting) return;
        UObject* widget = ActiveCraftWindow();
        if (!widget) return;
        int count = 0;
        bool holding = false;
        if (!ReadAt(widget, Reflect::FindPropertyOffset(widget, "ItemsToCraftCount"), count) ||
            count <= 0 || count > 100000 ||
            !BoolField(widget, "bIsCreateButtonHolded", holding) || holding) return;
        // Use the game's resource calculation and recipe checks. Never force
        // bCanCraft/CanCraftResult or consume resources to update the display.
        Call refresh(widget, "UpdateCraftItemCount");
        refresh.Set("InCount", count);
        if (refresh.Run())
        {
            bool available = false;
            BoolField(widget, "bCanCraft", available);
            LOG("Crafting display refreshed: resource check=%s", available ? "available" : "unavailable");
            refreshCrafting = false;
        }
    }

    int SkillCount(UObject* component)
    {
        // Fresh SDK TSet -> TSparseArray: TArray(0x10), FBitArray(0x20),
        // FirstFreeIndex(4), NumFreeIndices(4). Count entries, never mutate it.
        int offset = IsLiveObject(component) ? Reflect::FindPropertyOffset(component, "Skills") : -1;
        int allocated = 0, capacity = 0, free = 0;
        if (offset < 0 || !ReadAt(component, offset + 8, allocated) ||
            !ReadAt(component, offset + 12, capacity) || !ReadAt(component, offset + 0x34, free) ||
            allocated < 0 || allocated > 10000 || capacity < allocated || capacity > 100000 ||
            free < 0 || free > allocated) return -1;
        return allocated - free;
    }
    int skillsBefore = -1;

    void SkillStep()
    {
        if (!nextSkillCategory) return;
        UObject* component = skillOwner.Get();
        UObject* current = Reflect::ReadNamedObjectProperty(GetLocalPawn(), "SkillsComponent");
        if (!component || component != current)
        { nextSkillCategory = 0; Report("Skill unlock stopped because the player changed."); return; }
        Call grant(component, "TryToGiveAllSkillsInCategory");
        grant.Set("InSkillCategory", static_cast<uint8_t>(nextSkillCategory));
        if (!grant.Run())
        { nextSkillCategory = 0; Report("The character skill-grant function was unavailable."); return; }
        ++nextSkillCategory;
        if (nextSkillCategory >= 20)
        {
            nextSkillCategory = 0;
            Report("Skill categories processed; owned entries " + std::to_string(skillsBefore) + " -> " +
                std::to_string(SkillCount(component)) + ". Check upgrades and equipped abilities in Nora.");
        }
    }

    void StageStep()
    {
        if (stageSearchIndex < 0) return;
        UObject* pc = GetPlayerController();
        if (!IsLiveObject(pc) || pc->Class()->GetName() != "BP_MainMenuPlayerController_C")
        { stageSearchIndex = -1; Report("Return to the title's main menu to use stage selection."); return; }
        UObject* mainClass = FindObjectFast("Class /Script/AtomicHeart.MainMenuWidget");
        if (!IsLiveObject(mainClass)) { stageSearchIndex = -1; Report("Main-menu class unavailable."); return; }
        auto start = std::chrono::steady_clock::now();
        int count = NumObjects();
        for (int visited = 0; stageSearchIndex < count && visited < 4096; ++visited, ++stageSearchIndex)
        {
            UObject* main = GetObjectByIndex(stageSearchIndex);
            bool visible = false;
            if (IsLiveObject(main) && main->IsA(mainClass) &&
                main->GetName().find("Default__") != 0 && BoolResult(main, "IsInViewport", visible) && visible)
            {
                stageSearchIndex = -1;
                if (requestOpenWorld)
                {
                    Call play(main, "PlayOpenWorld");
                    if (play.Run()) { G::menuOpen = false; Report("Requested the game's existing open-world save. The game requires an eligible save."); }
                    else Report("Open-world continuation is unavailable.");
                    return;
                }
                UObject* cls = FindObjectFast("WBP_MainMenuLevelSwitcher_C");
                UObject* library = FindObjectFast("WidgetBlueprintLibrary /Script/UMG.Default__WidgetBlueprintLibrary");
                if (!IsLiveObject(cls) || cls->GetName() != "WBP_MainMenuLevelSwitcher_C")
                { Report("The game's stage-selector Blueprint is not loaded."); return; }
                Call create(library, "Create");
                create.Set("WorldContextObject", GetWorld());
                create.Set("WidgetType", cls);
                create.Set("OwningPlayer", pc);
                UObject* widget = nullptr;
                if (!create.Run() || !create.Get("ReturnValue", widget) || !IsLiveObject(widget))
                { Report("Stage selector could not be created."); return; }
                int refOffset = Reflect::FindPropertyOffset(widget, "MainMenuRef");
                if (refOffset < 0 || !Mem::IsReadable(reinterpret_cast<uint8_t*>(widget) + refOffset, sizeof(void*)))
                { Report("Stage selector is missing its main-menu reference field."); return; }
                std::memcpy(reinterpret_cast<uint8_t*>(widget) + refOffset, &main, sizeof(main));
                stageWidget = ObjectRef(widget);
                UFunction* query = Function(widget, "GetIsShippingBuild");
                shippingReturnOffset = query ? Reflect::FindPropertyOffsetInStruct(query, "ReturnValue") : -1;
                stageReceiver = widget;
                shippingQuery = query;
                Call add(widget, "AddToViewport");
                add.Set("ZOrder", 100);
                if (add.Run() && BoolResult(widget, "IsInViewport", visible) && visible)
                { G::menuOpen = false; Report("Game stage selector opened. Choose a level and player start there."); }
                else Report("Stage selector did not enter the viewport.");
                return;
            }
            if (std::chrono::steady_clock::now() - start > std::chrono::milliseconds(2)) { ++stageSearchIndex; return; }
        }
        if (stageSearchIndex >= count) { stageSearchIndex = -1; Report("No active main-menu widget was found."); }
    }

    void Poll()
    {
        if (noraWorld.ptr && noraWorld.Get() != GetWorld()) nora = {};
        if (nora.ptr && !nora.Get())
        { nora = {}; Report("Portable Nora was removed by the game. Retry in clear space."); }
        UObject* actor = nora.Get();
        bool ready = actor && NoraReady(actor);
        bool busy = actor && NoraBusy(actor);
        PriceScanStep();
        RefreshCraftingDisplay();
        SkillStep();
        StageStep();
        std::lock_guard<std::mutex> lock(statusMutex);
        if (ready && !status.noraReady) status.message = "Nora assets ready. Interact normally for her native visuals and dialogue.";
        else if (actor && !ready && noraSpawnMs && GetTickCount64() - noraSpawnMs > 30000)
            status.message = "Nora assets are still unavailable. No blocking load was forced; visit a normal Nora and retry.";
        status.objectiveHidden = objectiveHidden;
        status.noraPresent = actor != nullptr;
        status.noraReady = ready;
        status.noraInUse = busy;
        status.pricesChanged = static_cast<int>(patches.size());
        status.priceMultiplier = multiplier;
        status.scanPercent = priceScanProgress.load();
        status.unlockRecipes = unlockRecipes;
        status.recipesLearned = recipesLearned;
        status.recipeFailures = recipeFailures;
        active = actor || multiplier != 1.0f || unlockRecipes || nextSkillCategory || stageSearchIndex >= 0;
    }

}

Workbench::Status Workbench::GetStatus()
{
    std::lock_guard<std::mutex> lock(statusMutex);
    return status;
}
void Workbench::Tick()
{
    if (!active.load() || tickPending.exchange(true)) return;
    static ULONGLONG lastMs = 0;
    ULONGLONG now = GetTickCount64();
    if (now - lastMs < 100) { tickPending = false; return; }
    lastMs = now;
    if (!Features::QueueGameAction([]()
    {
        try { Poll(); } catch (...) { Report("Workbench polling stopped after a game-memory fault."); active = false; }
        tickPending = false;
    })) tickPending = false;
}
void Workbench::SpawnNora() { Queue(SpawnNoraImpl); }
void Workbench::RemoveNora()
{
    Queue([]()
    {
        UObject* actor = nora.Get();
        if (!actor) { nora = {}; Report("No portable Nora is present."); return; }
        if (NoraBusy(actor)) { Report("Close Nora's crafting/skills screen before removing her."); return; }
        Call remove(actor, "K2_DestroyActor");
        if (remove.Run()) { nora = {}; Report("Portable Nora removed."); }
        else Report("Nora removal was refused.");
    });
}
void Workbench::UseNora()
{
    Queue([]()
    {
        UObject* actor = nora.Get();
        UObject* user = Reflect::ReadNamedObjectProperty(GetLocalPawn(), "ActiveUserComponent");
        if (!NoraReady(actor) || NoraBusy(actor) || !IsLiveObject(user))
        { Report("Nora is not ready for interaction yet."); return; }
        FVector playerPos{}, noraPos{};
        Call playerLocation(GetLocalPawn(), "K2_GetActorLocation"), noraLocation(actor, "K2_GetActorLocation");
        if (noraWorld.Get() != GetWorld() || !playerLocation.Run() || !playerLocation.Get("ReturnValue", playerPos) ||
            !noraLocation.Run() || !noraLocation.Get("ReturnValue", noraPos))
        { Report("Nora's world or location is unavailable."); return; }
        const double dx = double(playerPos.X) - noraPos.X, dy = double(playerPos.Y) - noraPos.Y, dz = double(playerPos.Z) - noraPos.Z;
        const double distanceSquared = dx * dx + dy * dy + dz * dz;
        if (!std::isfinite(distanceSquared) || distanceSquared > 600.0 * 600.0)
        { Report("Move Nora nearby before using her (within 6 metres)."); return; }
        Call canUse(actor, "CanBeUsed");
        canUse.Set("ActiveUserComponent", user);
        canUse.Set("bPreCheck", true);
        bool allowed = false;
        if (!canUse.Run() || !canUse.Get("ReturnValue", allowed) || !allowed)
        { Report("The game currently blocks Nora interaction. Finish the active ability or scene first."); return; }
        Call use(actor, "Used");
        use.Set("ActiveUserComponent", user);
        if (use.Run()) { G::menuOpen = false; Report("Nora's native interaction started."); }
        else Report("Nora interaction was refused.");
    });
}
void Workbench::ChooseNoraResponse(int choice)
{
    if (choice < 1 || choice > 6) return;
    Queue([choice]()
    {
        UObject* actor = nora.Get();
        UObject* dialogue = actor ? Reflect::ReadNamedObjectProperty(actor, "DialogueCharacter") : nullptr;
        UObject* widget = nullptr;
        Call current(dialogue, "GetCurrentWidget");
        bool visible = false, blocked = true;
        if (!current.Run() || !current.Get("ReturnValue", widget) || !IsLiveObject(widget) ||
            !BoolResult(widget, "IsVisible", visible) || !visible ||
            !BoolField(widget, "bInputBlocked", blocked) || blocked)
        { Report("No active portable Nora response is ready."); return; }
        static const char* methods[] = { "OnVariant1Pressed", "OnVariant2Pressed", "OnVariant3Pressed",
            "OnVariant4Pressed", "OnVariant5Pressed", "OnVariant6Pressed" };
        Call select(widget, methods[choice - 1]);
        if (select.Run()) { G::menuOpen = false; Report("Nora response requested; check the resulting screen."); }
        else Report("Nora response was unavailable.");
    });
}

void Workbench::CloseNoraDialogue()
{
    Queue([]()
    {
        UObject* actor = nora.Get();
        UObject* dialogue = actor ? Reflect::ReadNamedObjectProperty(actor, "DialogueCharacter") : nullptr;
        if (!IsLiveObject(dialogue)) { Report("No portable Nora dialogue is available."); return; }
        bool inScene = false;
        if (!BoolField(actor, "InScen", inScene) || inScene)
        { Report("Close Nora's crafting screen normally before closing her dialogue."); return; }
        Call stop(dialogue, "StopDialog");
        if (stop.Run()) Report("Requested Nora dialogue close. Removal waits until her interaction ends.");
        else Report("Nora dialogue close was unavailable.");
    });
}

void Workbench::SetPrices(float value)
{
    if (!std::isfinite(value) || value < 0 || value > 2) return;
    Queue([value]()
    {
        RestorePrices();
        multiplier = value;
        priceScanProgress = value == 1.0f ? 100 : 0;
        ++priceGeneration;
        priceDiscoveryEnabled = value != 1.0f || unlockRecipes;
        active = true;
        Report(value == 1.0f ? "Original live prices restored." : "Applying prices in bounded batches. Reopen crafting afterward to refresh its display.");
    });
}
void Workbench::SetRecipeUnlocks(bool enabled)
{
    Queue([enabled]()
    {
        if (enabled && !PlayerInventory()) { Report("Load a player save before unlocking recipes."); return; }
        unlockRecipes = enabled;
        ++priceGeneration;
        priceDiscoveryEnabled = enabled || multiplier != 1.0f;
        active = true;
        Report(enabled ? "Learning loaded recipes in small batches. Learned recipes follow normal save behavior; reopen crafting afterward."
            : "Automatic recipe learning stopped. Recipes already learned remain unlocked.");
    });
}
void Workbench::UnlockSkills()
{
    Queue([]()
    {
        UObject* component = Reflect::ReadNamedObjectProperty(GetLocalPawn(), "SkillsComponent");
        if (!IsLiveObject(component)) { Report("No live player skills component is available."); return; }
        skillOwner = ObjectRef(component);
        skillsBefore = SkillCount(component);
        nextSkillCategory = 1;
        active = true;
        Report("Unlocking the current skill tree one category at a time.");
    });
}
void Workbench::ToggleDebugMenu()
{
    Queue([]()
    {
        if (UObject* widget = stageWidget.Get())
        {
            bool visible = false;
            if (BoolResult(widget, "IsInViewport", visible) && visible)
            { Report("The stage selector is already open."); return; }
        }
        requestOpenWorld = false;
        stageSearchIndex = 0;
        active = true;
        Report("Finding the main-menu stage selector.");
    });
}
void Workbench::CloseDebugMenu()
{
    Queue([]()
    {
        Call remove(stageWidget.Get(), "RemoveFromParent");
        if (remove.Run()) Report("Stage selector closed.");
        stageWidget = {};
        stageReceiver = nullptr;
        shippingQuery = nullptr;
    });
}
void Workbench::ContinueOpenWorld()
{
    Queue([]() { requestOpenWorld = true; stageSearchIndex = 0; active = true; });
}
void Workbench::SetObjectiveHidden(bool hidden)
{
    Queue([hidden]()
    {
        UObject* component = Reflect::ReadNamedObjectProperty(GetLocalPawn(), "QuestComponent");
        if (!IsLiveObject(component)) { Report("Quest display unavailable in this world."); return; }
        if (hidden && !objectiveHidden)
        {
            Call current(component, "GetTrackedQuestInfo");
            UObject* quest = nullptr;
            if (!current.Run() || !current.Get("ReturnValue", quest)) return;
            originalObjective = ObjectRef(quest);
            objectiveOwner = ObjectRef(component);
            Call clear(component, "ClearTrackedQuest");
            if (!clear.Run()) return;
            objectiveHidden = true;
        }
        else if (!hidden && objectiveHidden)
        {
            if (objectiveOwner.Get() == component)
            {
                Call restore(component, "SetTrackedQuestInfo");
                restore.Set("InQuestInfo", originalObjective.Get());
                if (!restore.Run()) return;
            }
            objectiveHidden = false;
            objectiveOwner = {}; originalObjective = {};
        }
        active = true;
        Report(hidden ? "Tracked objective hidden. Mission scripts still run; use open-world continuation for the game's free-roam state."
                      : "Tracked objective restored.");
    });
}

bool Workbench::FilterProcessEvent(void* object, void* function, void* params)
{
    if (!function || function != shippingQuery.load() || object != stageReceiver.load()) return false;
    int offset = shippingReturnOffset.load();
    if (offset < 0 || offset > 256 || !Mem::IsReadable(params, offset + 1)) return false;
    // Reveal only this owned widget's debug controls. Entitlement checks remain
    // in the original widget/game launch path.
    static_cast<uint8_t*>(params)[offset] = 0;
    return true;
}
bool Workbench::CleanupGameThread()
{
    if (UObject* actor = nora.Get())
    {
        if (NoraBusy(actor)) { Report("Close Nora before ejecting the menu."); return false; }
        Call remove(actor, "K2_DestroyActor");
        if (!remove.Run()) return false;
    }
    nora = {};
    if (UObject* widget = stageWidget.Get()) { Call remove(widget, "RemoveFromParent"); remove.Run(); }
    stageWidget = {};
    stageReceiver = nullptr;
    shippingQuery = nullptr;
    stageSearchIndex = -1;
    nextSkillCategory = 0;
    if (objectiveHidden && objectiveOwner.Get() == Reflect::ReadNamedObjectProperty(GetLocalPawn(), "QuestComponent"))
    {
        Call restore(objectiveOwner.Get(), "SetTrackedQuestInfo");
        restore.Set("InQuestInfo", originalObjective.Get());
        if (!restore.Run()) return false;
    }
    objectiveHidden = false;
    objectiveOwner = {}; originalObjective = {};
    unlockRecipes = false;
    recipeInventory = {};
    priceDiscoveryEnabled = false;
    ++priceGeneration;
    RestorePrices();
    multiplier = 1;
    active = false;
    return true;
}

std::string Workbench::TestSnapshotJson()
{
    const auto current = GetStatus();
    std::ostringstream out;
    out << std::boolalpha << "{\"busy\":" << current.busy << ",\"nora_present\":" << current.noraPresent
        << ",\"nora_ready\":" << current.noraReady << ",\"nora_in_use\":" << current.noraInUse
        << ",\"price_multiplier\":" << current.priceMultiplier << ",\"price_lists\":" << current.pricesChanged
        << ",\"recipe_unlocks\":" << unlockRecipes << ",\"recipes_learned\":" << recipesLearned << ",\"recipe_failures\":" << recipeFailures
        << ",\"scan_percent\":" << current.scanPercent << ",\"skill_category_pending\":" << nextSkillCategory;
    int checked = 0, mismatches = 0;
    for (auto& entry : patches)
    {
        if (checked >= 64) break;
        auto& patch = entry.second; TArray<uint8_t> array{};
        if (!PriceArray(patch.owner.Get(), patch.property, array) || array.Data != patch.arrayData || array.Count != patch.count) continue;
        ++checked;
        for (int i = 0; i < array.Count; ++i)
            if (*reinterpret_cast<int32_t*>(array.Data + i * inventoryStride + amountOffset) != patch.applied[i]) ++mismatches;
    }
    UObject* actor = nora.Get(); bool greetings = false, canUse = false;
    if (actor) { BoolField(actor, "bPlayGreetings", greetings); BoolField(actor, "CanUse", canUse); }
    out << ",\"price_lists_sampled\":" << checked << ",\"price_value_mismatches\":" << mismatches
        << ",\"nora_greetings_enabled\":" << greetings << ",\"nora_can_use\":" << canUse
        << ",\"objective_hidden\":" << objectiveHidden
        << ",\"price_values_restored\":" << restoredValues << ",\"price_restore_skipped\":" << skippedRestoreValues
        << ",\"price_restore_mismatches\":" << restoreMismatches
        << ",\"owned_skill_entries\":" << SkillCount(Reflect::ReadNamedObjectProperty(GetLocalPawn(), "SkillsComponent"));
    UObject* mesh = actor ? Reflect::ReadNamedObjectProperty(actor, "CraftMachineSkeletal") : nullptr;
    out << ",\"nora_mesh_loaded\":" << IsLiveObject(Reflect::ReadNamedObjectProperty(mesh, "SkeletalMesh"))
        << ",\"nora_animation_loaded\":" << IsLiveObject(Reflect::ReadNamedObjectProperty(mesh, "AnimScriptInstance"));
    // A visible widget is evidence of creation, not evidence a level was loaded.
    bool stageVisible = false; if (stageWidget.Get()) BoolResult(stageWidget.Get(), "IsInViewport", stageVisible);
    out << ",\"stage_visible\":" << stageVisible << '}';
    return out.str();
}

void Workbench::WorkerTick()
{
    try { DiscoverPricesWorker(); }
    catch (...)
    {
        static ULONGLONG lastError = 0;
        if (GetTickCount64() - lastError > 10000)
        { lastError = GetTickCount64(); LOG("Crafting discovery skipped a stale object or cache I/O failure."); }
    }
}
