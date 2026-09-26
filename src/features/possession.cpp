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
#include "possession.h"
#include "features.h"
#include "../sdk/offsets.h"
#include "../sdk/reflect.h"
#include "../sdk/reflect_call.h"
#include "../core/globals.h"
#include "../core/log.h"
#include "../core/memory.h"
#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <functional>
#include <mutex>

namespace
{
    using namespace UE;
    using RefCall::Call;
    using RefCall::ObjectRef;
    namespace AH = Offsets::AH;

    std::mutex           g_statusMutex;
    Possession::Status   g_status;   // guarded by g_statusMutex
    Possession::Settings g_settings; // written by the menu, read by the drive

    // Game-thread state.
    ObjectRef g_target;       // the character being played
    ObjectRef g_body;         // the player's own character
    ObjectRef g_aiController; // the controller the target had before
    ObjectRef g_camera;       // third-person camera component added to the target
    std::vector<ObjectRef> g_abilityClasses;
    bool g_jumping = false;
    bool g_cameraUnavailable = false; // AddComponentByClass refused once: stay first person

    struct DamageBackup { ObjectRef owner; bool valid = false; float base = 1.0f, current = 1.0f; };
    DamageBackup g_bodyDamage, g_targetDamage;
    struct WalkBackup { ObjectRef owner; bool valid = false; float speed = 0.0f; };
    WalkBackup g_sprint;

    // Shared with the render thread and the window procedure.
    std::atomic<bool>     g_active{ false };
    std::atomic<UObject*> g_controlled{ nullptr };
    std::atomic<bool>     g_busy{ false };          // a take/return is queued
    std::atomic<bool>     g_driveInFlight{ false };
    std::atomic<long>     g_mouseDx{ 0 }, g_mouseDy{ 0 };
    std::atomic<int>      g_pendingAbility{ -1 };

    enum Key : uint32_t { KeyForward = 1, KeyBack = 2, KeyLeft = 4, KeyRight = 8, KeyJump = 16, KeyDescend = 32, KeySprint = 64 };

    constexpr float kUnitsPerMetre = 100.0f;
    constexpr float kDegToRad = 0.01745329251994329577f;

    void SetMessage(const std::string& text, bool notify = true)
    {
        {
            std::lock_guard<std::mutex> lock(g_statusMutex);
            g_status.message = text;
        }
        LOG("PlayAs: %s", text.c_str());
        if (notify)
            Features::Notify("%s", text.c_str());
    }

    UObject* PawnController(UObject* pawn)
    {
        UObject* controller = nullptr;
        RefCall::ReadAt(pawn, Offsets::O_Pawn_Controller, controller);
        return IsLiveObject(controller) ? controller : nullptr;
    }

    bool CharacterDown(UObject* character)
    {
        uint8_t* b = reinterpret_cast<uint8_t*>(character);
        if (Mem::IsReadable(b + AH::Char_bIsDead, 1) && *reinterpret_cast<bool*>(b + AH::Char_bIsDead))
            return true;
        float cur = 0.0f, mx = 0.0f;
        return Features::CharacterHealth(character, cur, mx) && cur <= 0.0f;
    }

    std::string ShortName(UObject* object)
    {
        std::string name;
        try { name = object ? object->GetName() : std::string(); } catch (...) {}
        if (name.size() > 2 && name.compare(name.size() - 2, 2, "_C") == 0) name.erase(name.size() - 2);
        for (const char* prefix : { "Default__", "BP_", "GA_AI_", "GA_" })
            if (name.rfind(prefix, 0) == 0) name.erase(0, strlen(prefix));
        std::replace(name.begin(), name.end(), '_', ' ');
        return name.empty() ? std::string("?") : name;
    }

    // ---- damage: the multiplier god mode uses, captured and put back ------------
    uint8_t* AttributeSet(UObject* character)
    {
        uint8_t* b = reinterpret_cast<uint8_t*>(character);
        if (!IsLiveObject(character) || !Mem::IsReadable(b + AH::Char_AttributeSet, sizeof(void*)))
            return nullptr;
        uint8_t* set = *reinterpret_cast<uint8_t**>(b + AH::Char_AttributeSet);
        return Mem::IsReadable(set, AH::Set_IncomingDamageMult + AH::Attr_CurrentValue + sizeof(float)) ? set : nullptr;
    }

    void ReleaseInvulnerable(DamageBackup& backup)
    {
        if (backup.valid)
            if (UObject* owner = backup.owner.Get())
                if (uint8_t* set = AttributeSet(owner))
                {
                    *reinterpret_cast<float*>(set + AH::Set_IncomingDamageMult + AH::Attr_BaseValue) = backup.base;
                    *reinterpret_cast<float*>(set + AH::Set_IncomingDamageMult + AH::Attr_CurrentValue) = backup.current;
                }
        backup = DamageBackup{};
    }

    void HoldInvulnerable(DamageBackup& backup, UObject* character)
    {
        uint8_t* set = AttributeSet(character);
        if (!set)
            return;
        if (!backup.valid || backup.owner.Get() != character)
        {
            ReleaseInvulnerable(backup);
            backup.owner = ObjectRef(character);
            backup.base = *reinterpret_cast<float*>(set + AH::Set_IncomingDamageMult + AH::Attr_BaseValue);
            backup.current = *reinterpret_cast<float*>(set + AH::Set_IncomingDamageMult + AH::Attr_CurrentValue);
            // A multiplier already at 0 (god mode) is not an original.
            if (!(backup.current > 0.0f) || !std::isfinite(backup.current))
                backup.base = backup.current = 1.0f;
            backup.valid = true;
        }
        *reinterpret_cast<float*>(set + AH::Set_IncomingDamageMult + AH::Attr_BaseValue) = 0.0f;
        *reinterpret_cast<float*>(set + AH::Set_IncomingDamageMult + AH::Attr_CurrentValue) = 0.0f;
    }

    // ---- sprint: a temporary walk-speed raise, put back afterwards -------------
    uint8_t* Movement(UObject* character)
    {
        uint8_t* b = reinterpret_cast<uint8_t*>(character);
        if (!Mem::IsReadable(b + AH::Char_CharacterMovement, sizeof(void*)))
            return nullptr;
        uint8_t* mv = *reinterpret_cast<uint8_t**>(b + AH::Char_CharacterMovement);
        return IsLiveObject(reinterpret_cast<UObject*>(mv)) && Mem::IsReadable(mv + AH::Move_MaxWalkSpeed, sizeof(float)) ? mv : nullptr;
    }

    void StopSprint()
    {
        if (g_sprint.valid)
            if (UObject* owner = g_sprint.owner.Get())
                if (uint8_t* mv = Movement(owner))
                    *reinterpret_cast<float*>(mv + AH::Move_MaxWalkSpeed) = g_sprint.speed;
        g_sprint = WalkBackup{};
    }

    void Sprint(UObject* character, bool on)
    {
        if (!on) { StopSprint(); return; }
        uint8_t* mv = Movement(character);
        if (!mv) return;
        float& maxWalk = *reinterpret_cast<float*>(mv + AH::Move_MaxWalkSpeed);
        if (!g_sprint.valid)
        {
            if (!std::isfinite(maxWalk) || maxWalk <= 0.0f) return;
            g_sprint.owner = ObjectRef(character);
            g_sprint.speed = maxWalk;
            g_sprint.valid = true;
        }
        maxWalk = (std::max)(g_sprint.speed * 1.8f, 900.0f);
    }

    // ---- abilities: the character's own GameplayAbilitySystem grants -----------
    UObject* AbilitySystem(UObject* character)
    {
        UObject* asc = nullptr;
        if (!RefCall::ReadAt(character, AH::Char_AbilitySystemComp, asc) || !IsLiveObject(asc))
            return nullptr;
        static UObject* ascClass = nullptr;
        if (!IsLiveObject(ascClass))
            ascClass = FindObjectFast("Class /Script/GameplayAbilities.AbilitySystemComponent");
        return !IsLiveObject(ascClass) || asc->IsA(ascClass) ? asc : nullptr;
    }

    // ActivatableAbilities.Items[i].Ability, laid out from the game's reflection.
    std::vector<UObject*> ListAbilityClasses(UObject* character)
    {
        std::vector<UObject*> out;
        UObject* asc = AbilitySystem(character);
        if (!asc)
            return out;
        UObject* containerStruct = FindObjectFast("ScriptStruct /Script/GameplayAbilities.GameplayAbilitySpecContainer");
        UObject* specStruct = FindObjectFast("ScriptStruct /Script/GameplayAbilities.GameplayAbilitySpec");
        const int containerOffset = Reflect::FindPropertyOffset(asc, "ActivatableAbilities");
        if (containerOffset < 0 || !IsLiveObject(containerStruct) || !IsLiveObject(specStruct))
            return out;
        const int itemsOffset = Reflect::FindPropertyOffsetInStruct(containerStruct, "Items");
        const int abilityOffset = Reflect::FindPropertyOffsetInStruct(specStruct, "Ability");
        int specSize = 0;
        if (itemsOffset < 0 || abilityOffset < 0 ||
            !RefCall::ReadAt(specStruct, Offsets::O_UStruct_PropertiesSize, specSize) ||
            specSize <= abilityOffset || specSize > 0x1000)
            return out;
        TArray<uint8_t> items{};
        if (!RefCall::ReadAt(asc, containerOffset + itemsOffset, items) || items.Count <= 0 || items.Count > 256 ||
            !Mem::IsReadable(items.Data, static_cast<size_t>(items.Count) * specSize))
            return out;
        for (int i = 0; i < items.Count; ++i)
        {
            UObject* ability = nullptr;
            if (!RefCall::ReadAt(items.Data + static_cast<size_t>(i) * specSize, abilityOffset, ability) || !IsLiveObject(ability))
                continue;
            UObject* cls = ability->Class();
            if (IsLiveObject(cls) && std::find(out.begin(), out.end(), cls) == out.end())
                out.push_back(cls);
        }
        return out;
    }

    void FaceAndTarget(UObject* character)
    {
        // What the crosshair is on becomes the character's target, so an ability
        // that aims at its owner's target aims where you look.
        UObject* target = Features::AimedCharacter(character, false);
        if (!target)
            return;
        Call setTarget(character, "SetTargetEnemy");
        setTarget.Set("TargetEnemy", target);
        setTarget.Set("bForceUpdate", true);
        setTarget.Run();
        uint8_t* b = reinterpret_cast<uint8_t*>(character);
        if (Mem::IsReadable(b + AH::AICh_CachedTargetEnemy, sizeof(void*)))
            *reinterpret_cast<UObject**>(b + AH::AICh_CachedTargetEnemy) = target;
        FVector from{}, to{};
        if (RefCall::CallReturning(character, "K2_GetActorLocation", from) &&
            RefCall::CallReturning(target, "K2_GetActorLocation", to))
        {
            FRotator facing{ 0.0f, atan2f(to.Y - from.Y, to.X - from.X) / kDegToRad, 0.0f };
            Call turn(character, "K2_SetActorRotation");
            turn.Set("NewRotation", facing);
            turn.Set("bTeleportPhysics", false);
            turn.Run();
        }
    }

    void UseAbilityGameThread(UObject* character, int index)
    {
        if (index < 0 || index >= static_cast<int>(g_abilityClasses.size()))
            return;
        UObject* abilityClass = g_abilityClasses[index].Get();
        UObject* asc = AbilitySystem(character);
        if (!abilityClass || !asc)
            return;
        FaceAndTarget(character);
        Call activate(asc, "TryActivateAbilityByClass");
        activate.Set("InAbilityToActivate", abilityClass);
        activate.Set("bAllowRemoteActivation", true);
        bool activated = false;
        if (!activate.Run() || !activate.Get("ReturnValue", activated) || !activated)
            SetMessage("Ability " + std::to_string(index + 1) + " (" + ShortName(abilityClass) +
                       ") did not start: on cooldown, blocked, or it needs the character's own AI.");
    }

    // ---- camera: a camera component behind the character ------------------------
    struct FTransformLite { float rotation[4]; float translation[3]; float pad0; float scale[3]; float pad1; };
    static_assert(sizeof(FTransformLite) == 0x30, "FTransform is 0x30 bytes");

    void AttachCamera(UObject* character)
    {
        UObject* cameraClass = FindObjectFast("Class /Script/Engine.CameraComponent");
        if (!IsLiveObject(cameraClass))
            return;
        Call add(character, "AddComponentByClass");
        const FTransformLite identity{ { 0.0f, 0.0f, 0.0f, 1.0f }, { 0.0f, 0.0f, 0.0f }, 0.0f, { 1.0f, 1.0f, 1.0f }, 0.0f };
        add.Set("Class", cameraClass);
        add.Set("bManualAttachment", false);
        add.Set("RelativeTransform", identity);
        add.Set("bDeferredFinish", false);
        UObject* camera = nullptr;
        if (add.Run() && add.Get("ReturnValue", camera) && IsLiveObject(camera))
            g_camera = ObjectRef(camera);
        else
        {
            g_cameraUnavailable = true;
            LOG("PlayAs: no third-person camera on this build; viewing through the character's eyes");
        }
    }

    void DetachCamera()
    {
        if (UObject* camera = g_camera.Get())
        {
            Call destroy(camera, "K2_DestroyComponent");
            destroy.Set("Object", camera);
            destroy.Run();
        }
        g_camera = ObjectRef{};
    }

    void UpdateCamera(UObject* character, const FRotator& view)
    {
        UObject* camera = g_camera.Get();
        if (!camera)
            return;
        FVector at{};
        if (!RefCall::CallReturning(character, "K2_GetActorLocation", at))
            return;
        const float cp = cosf(view.Pitch * kDegToRad), sp = sinf(view.Pitch * kDegToRad);
        const float cy = cosf(view.Yaw * kDegToRad), sy = sinf(view.Yaw * kDegToRad);
        const float distance = (std::max)(0.5f, g_settings.cameraDistanceM) * kUnitsPerMetre;
        const float height = g_settings.cameraHeightM * kUnitsPerMetre;
        FVector position{ at.X - cp * cy * distance, at.Y - cp * sy * distance, at.Z + height - sp * distance };
        Call place(camera, "K2_SetWorldLocationAndRotation");
        place.Set("NewLocation", position);
        place.Set("NewRotation", view);
        place.Set("bSweep", false);
        place.Set("bTeleport", true);
        place.Run();
    }

    // ---- taking and returning control -----------------------------------------
    void PublishStatus()
    {
        Possession::Status next;
        next.active = g_active.load();
        next.busy = g_busy.load();
        if (UObject* target = g_target.Get())
        {
            next.character = ShortName(target);
            float cur = 0.0f, mx = 0.0f;
            if (Features::CharacterHealth(target, cur, mx))
                next.healthFrac = (std::max)(0.0f, (std::min)(1.0f, cur / mx));
        }
        for (const ObjectRef& ability : g_abilityClasses)
            next.abilities.push_back(ShortName(ability.Get()));
        std::lock_guard<std::mutex> lock(g_statusMutex);
        next.message = g_status.message;
        g_status = next;
    }

    void EndGameThread(const char* why)
    {
        UObject* target = g_target.Get();
        UObject* body = g_body.Get();
        UObject* pc = GetPlayerController();
        DetachCamera();
        StopSprint();
        if (g_jumping && target) RefCall::CallNoArgs(target, "StopJumping");
        g_jumping = false;
        ReleaseInvulnerable(g_targetDamage);

        // Only take the controller back from the character we gave it: if the game
        // has moved it on (a cutscene pawn, a respawn), that is the game's call.
        UObject* current = GetLocalPawn();
        bool back = current == body && body;
        if (IsLiveObject(pc) && body && (current == target || !current))
        {
            Call possess(pc, "Possess");
            possess.Set("InPawn", body);
            back = possess.Run() && GetLocalPawn() == body;
        }
        else if (body && current && current != body)
        {
            back = true; // the game owns the controller now; nothing of ours to undo
        }
        // The character goes back to its own AI (or a fresh one), so a robot you
        // leave is a robot again rather than a statue.
        if (target && !CharacterDown(target) && !PawnController(target))
        {
            UObject* ai = g_aiController.Get();
            if (ai)
            {
                Call possess(ai, "Possess");
                possess.Set("InPawn", target);
                possess.Run();
            }
            if (!PawnController(target))
                RefCall::CallNoArgs(target, "SpawnDefaultController");
        }
        ReleaseInvulnerable(g_bodyDamage);
        Features::SetSaveBlock(Features::SaveBlockPlayAs, false);

        g_target = ObjectRef{};
        g_body = ObjectRef{};
        g_aiController = ObjectRef{};
        g_abilityClasses.clear();
        g_controlled = nullptr;
        g_active = false;
        g_mouseDx = 0;
        g_mouseDy = 0;
        g_pendingAbility = -1;
        if (body && !back)
            SetMessage(std::string("Could not return to your character") + why + ". Use Player > Rebind controls.");
        else
            SetMessage(std::string("Back in your own character") + why + ".");
        PublishStatus();
    }

    void BeginGameThread(UObject* target)
    {
        if (g_active.load())
            EndGameThread(" (switching character)");
        UObject* pc = GetPlayerController();
        UObject* body = GetLocalPawn();
        if (!IsLiveObject(pc) || !IsLiveObject(body))
        { SetMessage("No player character in control right now."); return; }
        if (!Features::IsAiCharacter(target) || target == body)
        { SetMessage("That is not a character you can play as."); return; }
        if (CharacterDown(target))
        { SetMessage("That character is dead."); return; }

        UObject* aiController = PawnController(target);
        Call possess(pc, "Possess");
        possess.Set("InPawn", target);
        if (!possess.Run() || GetLocalPawn() != target)
        {
            // Whatever happened, the player's own character keeps control.
            if (GetLocalPawn() != body)
            {
                Call back(pc, "Possess");
                back.Set("InPawn", body);
                back.Run();
            }
            if (IsLiveObject(aiController) && !PawnController(target))
            {
                Call ai(aiController, "Possess");
                ai.Set("InPawn", target);
                ai.Run();
            }
            SetMessage("The game refused control of that character.");
            return;
        }

        g_body = ObjectRef(body);
        g_target = ObjectRef(target);
        g_aiController = ObjectRef(aiController);
        g_cameraUnavailable = false;
        g_controlled = target;
        g_active = true;
        g_mouseDx = 0;
        g_mouseDy = 0;
        Features::SetSaveBlock(Features::SaveBlockPlayAs, true);
        HoldInvulnerable(g_bodyDamage, body);
        if (g_settings.invulnerable)
            HoldInvulnerable(g_targetDamage, target);
        if (g_settings.thirdPerson)
            AttachCamera(target);
        g_abilityClasses.clear();
        for (UObject* cls : ListAbilityClasses(target))
            g_abilityClasses.push_back(ObjectRef(cls));
        SetMessage("Playing as " + ShortName(target) + ". Num . or Return in the menu takes you back.");
        LOG("PlayAs: took %s (%d abilities), body %p kept invulnerable", ShortName(target).c_str(),
            (int)g_abilityClasses.size(), (void*)body);
        PublishStatus();
    }

    void Drive(uint32_t keys, long dx, long dy, int ability)
    {
        if (!g_active.load())
            return;
        UObject* target = g_target.Get();
        UObject* pc = GetPlayerController();
        if (!target || !IsLiveObject(pc))
        { EndGameThread(" (the character is gone)"); return; }
        if (CharacterDown(target))
        { EndGameThread(" (the character died)"); return; }
        if (GetLocalPawn() != target)
        { EndGameThread(" (the game took control back)"); return; }

        // Look: AI characters bind no player input, so the mouse is fed in here.
        const float k = 0.07f * (std::max)(0.05f, g_settings.lookSensitivity);
        if (dx)
        {
            Call yaw(pc, "AddYawInput");
            yaw.Set("Val", static_cast<float>(dx) * k);
            yaw.Run();
        }
        if (dy)
        {
            Call pitch(pc, "AddPitchInput");
            pitch.Set("Val", static_cast<float>(g_settings.invertY ? -dy : dy) * k);
            pitch.Run();
        }
        FRotator view{};
        RefCall::CallReturning(pc, "GetControlRotation", view);

        // Move relative to where the camera faces.
        const float cy = cosf(view.Yaw * kDegToRad), sy = sinf(view.Yaw * kDegToRad);
        FVector direction{ 0.0f, 0.0f, 0.0f };
        if (keys & KeyForward) { direction.X += cy; direction.Y += sy; }
        if (keys & KeyBack)    { direction.X -= cy; direction.Y -= sy; }
        if (keys & KeyRight)   { direction.X -= sy; direction.Y += cy; }
        if (keys & KeyLeft)    { direction.X += sy; direction.Y -= cy; }
        if (keys & KeyJump)    direction.Z += 1.0f; // fliers climb; walkers ignore it
        if (keys & KeyDescend) direction.Z -= 1.0f;
        const float length = sqrtf(direction.X * direction.X + direction.Y * direction.Y + direction.Z * direction.Z);
        if (length > 0.001f)
        {
            direction = { direction.X / length, direction.Y / length, direction.Z / length };
            Call move(target, "AddMovementInput");
            move.Set("WorldDirection", direction);
            move.Set("ScaleValue", 1.0f);
            move.Set("bForce", false);
            move.Run();
        }
        const bool jump = (keys & KeyJump) != 0;
        if (jump && !g_jumping) RefCall::CallNoArgs(target, "Jump");
        if (!jump && g_jumping) RefCall::CallNoArgs(target, "StopJumping");
        g_jumping = jump;
        Sprint(target, (keys & KeySprint) != 0 && length > 0.001f);

        // The camera setting can change while playing.
        if (g_settings.thirdPerson && !g_camera.Get() && !g_cameraUnavailable) AttachCamera(target);
        if (!g_settings.thirdPerson && g_camera.Get()) DetachCamera();
        UpdateCamera(target, view);
        if (UObject* body = g_body.Get())
            HoldInvulnerable(g_bodyDamage, body);
        if (g_settings.invulnerable) HoldInvulnerable(g_targetDamage, target);
        else ReleaseInvulnerable(g_targetDamage);
        if (ability >= 0)
            UseAbilityGameThread(target, ability);

        static ULONGLONG lastStatusMs = 0;
        ULONGLONG now = GetTickCount64();
        if (now - lastStatusMs > 250)
        {
            lastStatusMs = now;
            PublishStatus();
        }
    }

    void Queue(std::function<void()> action)
    {
        if (g_busy.exchange(true))
            return;
        {
            std::lock_guard<std::mutex> lock(g_statusMutex);
            g_status.busy = true;
        }
        if (!Features::QueueGameAction([action = std::move(action)]()
        {
            try { action(); }
            catch (...) { LOG("PlayAs: game action faulted (ignored)"); }
            g_busy = false;
            std::lock_guard<std::mutex> lock(g_statusMutex);
            g_status.busy = false;
        }))
        {
            g_busy = false;
            std::lock_guard<std::mutex> lock(g_statusMutex);
            g_status.busy = false;
            g_status.message = "Game-thread queue unavailable. Try again in a loaded game.";
        }
    }

    bool KeyDown(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }
}

Possession::Settings& Possession::Config() { return g_settings; }

Possession::Status Possession::GetStatus()
{
    std::lock_guard<std::mutex> lock(g_statusMutex);
    return g_status;
}

bool Possession::IsActive() { return g_active.load(); }

bool Possession::IsControlled(UE::UObject* actor)
{
    return actor && actor == g_controlled.load();
}

void Possession::BeginAimed()
{
    if (!G::sdkReady.load()) return;
    UObject* target = Features::AimedCharacter(GetLocalPawn(), true);
    if (!target)
    {
        Features::Notify("No character under the crosshair");
        return;
    }
    const int32_t index = target->Index();
    Queue([target, index]()
    {
        if (IsLiveObject(target) && target->Index() == index)
            BeginGameThread(target);
        else
            SetMessage("That character is gone.");
    });
}

void Possession::BeginListed(unsigned long long id)
{
    if (!G::sdkReady.load()) return;
    UObject* target = Features::AiFromListId(id);
    if (!target)
    {
        Features::Notify("That character is no longer available");
        return;
    }
    const int32_t index = target->Index();
    Queue([target, index]()
    {
        if (IsLiveObject(target) && target->Index() == index)
            BeginGameThread(target);
        else
            SetMessage("That character is gone.");
    });
}

void Possession::End()
{
    if (!g_active.load()) return;
    Queue([]() { if (g_active.load()) EndGameThread(""); });
}

void Possession::Toggle()
{
    if (g_active.load()) End();
    else BeginAimed();
}

void Possession::UseAbility(int index)
{
    if (g_active.load() && index >= 0)
        g_pendingAbility = index;
}

void Possession::NoteMouseDelta(long dx, long dy)
{
    if (!g_active.load()) return;
    g_mouseDx.fetch_add(dx);
    g_mouseDy.fetch_add(dy);
}

void Possession::Tick()
{
    if (!g_active.load())
        return;
    const bool focused = G::hGameWindow && GetForegroundWindow() == static_cast<HWND>(G::hGameWindow) &&
        !G::menuOpen.load();
    uint32_t keys = 0;
    // Abilities are edge-triggered here, before the in-flight check, so a press
    // made while a drive is still queued is not lost.
    static bool wasDown[11] = {};
    const int abilityKeys[11] = { VK_LBUTTON, VK_RBUTTON, '1', '2', '3', '4', '5', '6', '7', '8', '9' };
    const int abilityIndex[11] = { 0, 1, 0, 1, 2, 3, 4, 5, 6, 7, 8 };
    for (int i = 0; i < 11; ++i)
    {
        const bool down = focused && KeyDown(abilityKeys[i]);
        if (down && !wasDown[i])
            g_pendingAbility = abilityIndex[i];
        wasDown[i] = down;
    }
    if (focused)
    {
        if (KeyDown('W')) keys |= KeyForward;
        if (KeyDown('S')) keys |= KeyBack;
        if (KeyDown('A')) keys |= KeyLeft;
        if (KeyDown('D')) keys |= KeyRight;
        if (KeyDown(VK_SPACE)) keys |= KeyJump;
        if (KeyDown(VK_CONTROL)) keys |= KeyDescend;
        if (KeyDown(VK_SHIFT)) keys |= KeySprint;
    }
    if (g_driveInFlight.load())
        return;
    long dx = g_mouseDx.exchange(0), dy = g_mouseDy.exchange(0);
    if (!focused)
        dx = dy = 0;
    const int ability = g_pendingAbility.exchange(-1);
    g_driveInFlight = true;
    if (!Features::QueueGameAction([keys, dx, dy, ability]()
    {
        try { Drive(keys, dx, dy, ability); }
        catch (...) { LOG("PlayAs: drive faulted (ignored)"); }
        g_driveInFlight = false;
    }))
        g_driveInFlight = false;
}

void Possession::CleanupGameThread()
{
    if (!g_active.load()) return;
    try { EndGameThread(" (menu unloading)"); }
    catch (...) { LOG("PlayAs: cleanup faulted (ignored)"); }
}
