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
#include <cstdint>

// ===========================================================================
//  BUILD-SPECIFIC OFFSETS / SIGNATURES
// ---------------------------------------------------------------------------
//  Atomic Heart ships on Unreal Engine 4.26/4.27. The engine *layout* below is
//  generic UE4, but the exact addresses and a few member offsets MUST be
//  verified against YOUR game build. The reliable way to get them is to inject
//  Dumper-7 (https://github.com/Encryqed/Dumper-7) once, which prints the
//  GObjects / GNames / ProcessEvent addresses and a full C++ SDK.
//
//  Two ways to feed the engine globals to this menu:
//    1. AOB scan at runtime (default; patterns below). Robust across patches.
//    2. Hard-code the absolute addresses from a dump (set USE_STATIC_OFFSETS).
//
//  The AOB patterns below are the common UE4.27 ones. If a scan fails, the menu
//  will say "SDK: not resolved" and you fix the pattern/offset here.
// ===========================================================================

namespace Offsets
{
    // ---- Toggle: scan vs. static ------------------------------------------
    // Using the addresses dumped by Dumper-7 for THIS build (UE4.27.2 Atomic
    // Heart, 4.27.2-18319896). Reliable and instant; no AOB scan needed.
    constexpr bool USE_STATIC_OFFSETS = true;

    // RVAs relative to module base 0x140000000 (from Dumper-7 Basic.hpp).
    constexpr uintptr_t GObjects_RVA   = 0x06EB7BD0; // FUObjectArray
    constexpr uintptr_t GNames_RVA     = 0x070ECBC0; // FNamePool
    constexpr uintptr_t GWorld_RVA     = 0x070E93C0; // UWorld**

    // ---- AOB patterns (UE4.27 typical) ------------------------------------
    // GObjects: lea/ mov referencing the FUObjectArray (GUObjectArray).
    //   mov rcx, GUObjectArray ; ... pattern around StaticFindObject/AddObject.
    constexpr const char* SIG_GOBJECTS = "48 8B 05 ?? ?? ?? ?? 48 8B 0C C8 48 8D 04 D1 EB";

    // GNames: FName::GetNames / NamePool reference.
    constexpr const char* SIG_GNAMES   = "48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? C6 05";

    // GWorld: mov [rip+x], rax / mov rax, [rip+x] to the UWorld* global.
    constexpr const char* SIG_GWORLD   = "48 8B 05 ?? ?? ?? ?? 48 3B C8 0F 84 ?? ?? ?? ?? 48 8B";

    // ---- Engine member layout (UE4.27 defaults; verify with dump) ----------
    // UObject
    constexpr int O_UObject_VTable         = 0x00;
    constexpr int O_UObject_Flags          = 0x08; // EObjectFlags
    constexpr int O_UObject_InternalIndex  = 0x0C;
    constexpr int O_UObject_Class          = 0x10; // UClass*
    constexpr int O_UObject_Name           = 0x18; // FName
    constexpr int O_UObject_Outer          = 0x20; // UObject*
    // NOTE: in this build UObject has an 8-byte tail pad (Dumper-7: Pad_28[0x8]),
    // so sizeof(UObject)=0x30 and the whole UField/UStruct chain below sits +0x8
    // vs the stock UE4.27 layout. These values are taken straight from
    // dumped-sdk/CppSDK/SDK/CoreUObject_classes.hpp -- do not "correct" them back
    // to the textbook 0x28/0x40/0x48 offsets.

    // UField (UObject + Next) -> lets us walk a class's function list directly
    // instead of substring-scanning GObjects for a UFunction full name.
    constexpr int O_UField_Next            = 0x30; // UField* (next field/function in the owner)

    // UStruct / UClass / UFunction chain (dump-verified, see note above)
    constexpr int O_UStruct_SuperStruct    = 0x48;
    constexpr int O_UStruct_Children       = 0x50;
    constexpr int O_UStruct_ChildProps     = 0x58;
    constexpr int O_UStruct_PropertiesSize = 0x60;
    // UFunction (verified Dumper-7, sizeof 0xE8): the textbook 0xB0/0xD8 were WRONG
    // for this build. FunctionFlags @0xB8; ExecFunction (FNativeFuncPtr) @0xE0 = the
    // native C++ implementation pointer -- the DETOUR TARGET for native UFunctions
    // (for BP functions it points at the BP VM, so detour ProcessEvent + filter instead).
    constexpr int      O_UFunction_FunctionFlags = 0xB8; // EFunctionFlags
    constexpr int      O_UFunction_ExecFunction  = 0xE0; // native impl ptr
    constexpr uint32_t FUNC_Native               = 0x00000400; // EFunctionFlags::FUNC_Native bit

    // ---- FField / FProperty reflection (UE4.25+; verified from Dumper-7) -----
    // The DATA members of a UStruct live in ChildProperties (a singly-linked
    // FField list), NOT Children (which holds UFunctions). Walking this list with
    // ONLY raw, guarded reads lets us dump EVERY named field of ANY class/struct --
    // the universal-discovery capability the diagnostics logger needs (e.g. to find
    // what drives Larisa's walk). All offsets are static_assert-verified in
    // dumped-sdk/CppSDK/Assertions.inl for this exact AtomicHeart build.
    namespace Reflect
    {
        // FField (size 0x38)
        constexpr int FField_ClassPrivate = 0x08; // FFieldClass*  (the property's type)
        constexpr int FField_Next         = 0x20; // FField*       (next field in the owner)
        constexpr int FField_Name         = 0x28; // FName

        // FFieldClass (size 0x28)
        constexpr int FFieldClass_Name       = 0x00; // FName (e.g. "ObjectProperty","FloatProperty")
        constexpr int FFieldClass_SuperClass = 0x20; // FFieldClass*

        // FProperty (size 0x78) -- base of every concrete property type
        constexpr int FProperty_ArrayDim     = 0x38; // int32 (C-array dim; usually 1)
        constexpr int FProperty_ElementSize  = 0x3C; // int32
        constexpr int FProperty_PropertyFlags= 0x40; // uint64
        constexpr int FProperty_Offset       = 0x4C; // int32  (value's byte offset in the container)

        // Concrete subclasses (all extend FProperty @0x78)
        constexpr int FByteProperty_Enum        = 0x78; // UEnum*
        constexpr int FBoolProperty_FieldSize   = 0x78; // uint8
        constexpr int FBoolProperty_ByteOffset  = 0x79; // uint8
        constexpr int FBoolProperty_ByteMask    = 0x7A; // uint8
        constexpr int FBoolProperty_FieldMask   = 0x7B; // uint8
        constexpr int FObjectProperty_PropClass = 0x78; // UClass*  (FObjectPropertyBase::PropertyClass)
        constexpr int FClassProperty_MetaClass  = 0x80; // UClass*
        constexpr int FStructProperty_Struct    = 0x78; // UScriptStruct*
        constexpr int FArrayProperty_Inner      = 0x78; // FProperty*
        constexpr int FEnumProperty_Underlaying = 0x78; // FProperty* (numeric)
        constexpr int FEnumProperty_Enum        = 0x80; // UEnum*
    }

    // ProcessEvent: index into the UObject vtable. UE4.27 is commonly ~0x44.
    // Confirm from the dump (Dumper-7 prints "ProcessEvent index").
    constexpr int VFUNC_PROCESSEVENT       = 0x44;

    // Dumper-7 TUObjectArray layout for this build:
    //   0x00 Objects(FUObjectItem**) 0x10 Max 0x14 Num 0x18 MaxChunks 0x1C NumChunks
    constexpr int O_ObjArray_Objects       = 0x00; // FUObjectItem** chunks
    constexpr int O_ObjArray_MaxElements   = 0x10;
    constexpr int O_ObjArray_NumElements   = 0x14;
    constexpr int O_ObjArray_NumChunks     = 0x1C;
    constexpr int NumElementsPerChunk      = 64 * 1024;
    constexpr int O_ObjectItem_Object      = 0x00; // UObject* inside FUObjectItem
    constexpr int SIZE_FUObjectItem        = 0x18;

    // FName pool block layout (UE4.23+ FNamePool)
    constexpr int O_NamePool_Stride        = 2;     // FNameEntry alignment shift
    constexpr int O_NamePool_BlocksOffset  = 0x10;  // start of Blocks[] in pool
    constexpr int O_NameEntry_Header       = 0x00;  // FNameEntryHeader (uint16)

    // ---- Local-player traversal chain (dumped, UE4.27.2 AtomicHeart) --------
    // UWorld -> OwningGameInstance -> LocalPlayers[0] -> PlayerController -> Pawn
    constexpr int O_World_GameInstance      = 0x1D8; // UWorld::OwningGameInstance
    constexpr int O_GameInst_LocalPlayers   = 0x40;  // UGameInstance::LocalPlayers (TArray<ULocalPlayer*>)
    constexpr int O_Player_PlayerController  = 0x38;  // UPlayer::PlayerController
    constexpr int O_Controller_Pawn         = 0x2C0; // APlayerController::AcknowledgedPawn (0x270 = AController::Pawn)
    constexpr int O_BaseController_Pawn     = 0x270; // AController::Pawn
    constexpr int O_Pawn_Controller         = 0x278; // APawn::Controller

    // ---- Camera (for world-to-screen / ESP) --------------------------------
    // APlayerController::PlayerCameraManager, then APlayerCameraManager's cached
    // POV. CameraCachePrivate (FCameraCacheEntry) @0x1B00, POV (FMinimalViewInfo)
    // @+0x10: Location@+0x00, Rotation@+0x0C, FOV@+0x18.
    constexpr int O_PC_CameraManager        = 0x2D8;
    constexpr int O_CamMgr_POV_Location     = 0x1B00 + 0x10 + 0x00; // 0x1B10 FVector
    constexpr int O_CamMgr_POV_Rotation     = 0x1B00 + 0x10 + 0x0C; // 0x1B1C FRotator
    constexpr int O_CamMgr_POV_FOV          = 0x1B00 + 0x10 + 0x18; // 0x1B28 float

    // Actor / scene component location cache. For movable pawns this root
    // component relative location is effectively world-space and avoids
    // reflection calls for ESP/scanner distance checks.
    constexpr int O_Actor_RootComponent      = 0x140; // AActor::RootComponent
    constexpr int O_Scene_RelativeLocation   = 0x144; // USceneComponent::RelativeLocation
    constexpr int O_Actor_CustomTimeDilation = 0xA4;  // AActor::CustomTimeDilation (float; ~0 freezes the actor)

    // ---- Fast actor enumeration (level walk, instead of a 360k GObjects sweep) --
    // UWorld -> PersistentLevel + Levels[] (loaded levels) -> ULevel::Actors.
    // This is a few thousand live actors, not every UObject, so AI discovery is
    // effectively instant and never hangs / delays commands.
    constexpr int O_World_PersistentLevel    = 0x38;  // UWorld::PersistentLevel (ULevel*)
    constexpr int O_World_Levels             = 0x190; // UWorld::Levels (TArray<ULevel*>)
    constexpr int O_Level_Actors             = 0xA0;  // ULevel::Actors (TArray<AActor*>)

    // ===================================================================
    //  ATOMIC HEART specific (dumped). Player pawn = AAHBaseCharacter.
    // ===================================================================
    namespace AH
    {
        // AAHBaseCharacter : ACharacter
        constexpr int Char_CharacterMovement = 0x2A8; // ACharacter::CharacterMovement
        constexpr int Char_AttributeSet      = 0x568; // UAHBaseCharacterAttributeSet*
        constexpr int Char_bIsDead           = 0x5E8;
        constexpr int Char_AbilitySystemComp = 0x738;

        // UCharacterMovementComponent
        constexpr int Move_GravityScale      = 0x170; // float (1.0 default; lower = floaty)
        constexpr int Move_JumpZVelocity     = 0x178; // float (jump impulse)
        constexpr int Move_MovementMode      = 0x188; // EMovementMode (5 = MOVE_Flying)
        constexpr int Move_MaxWalkSpeed      = 0x1AC; // float
        constexpr int Move_MaxFlySpeed       = 0x1B8; // float
        constexpr int Move_AirControl        = 0x1E4; // float (0..1 mid-air steering)
        constexpr uint8_t MOVE_Flying        = 5;
        constexpr uint8_t MOVE_Walking       = 1;

        // FGameplayAttributeData inner layout (size 0x10)
        constexpr int Attr_BaseValue         = 0x08;
        constexpr int Attr_CurrentValue      = 0x0C;

        // UAHBaseCharacterAttributeSet attribute offsets
        constexpr int Set_MaxHealth          = 0x38;
        constexpr int Set_Health             = 0x48;
        constexpr int Set_MaxStamina         = 0x98;
        constexpr int Set_Stamina            = 0xA8;
        constexpr int Set_MaxAir             = 0xC8;
        constexpr int Set_Air                = 0xD8;
        constexpr int Set_MaxEnergy          = 0xE8;
        constexpr int Set_Energy             = 0xF8;
        constexpr int Set_IncomingDamageMult = 0x178; // 0 => take no damage
        constexpr int Set_InstigatedDmgMult  = 0x188; // high => one-hit-kill

        // Camera FOV override. Writing the active camera component's FieldOfView
        // each frame persists into the next game tick (unlike the post-render POV
        // cache), so it actually changes the rendered FOV.
        constexpr int Char_FPCamera          = 0x5B0; // AAHBaseCharacter::FPCamera (UCameraComponent*)
        constexpr int Char_TPCamera          = 0x5B8; // AAHBaseCharacter::TPCamera (UCameraComponent*)
        constexpr int Camera_FieldOfView     = 0x288; // UCameraComponent::FieldOfView (float)

        // AAHPlayerCharacter / UAHInventoryPlayer
        constexpr int Char_InventoryPlayer   = 0x1788; // UAHInventoryPlayer*
        constexpr int Inventory_AmmoCount    = 0x0424; // ammo inventory capacity/count gate
        constexpr int Inventory_InfiniteAmmoCount = 0x04A8;

        // AEquipableItem / ABaseWeapon / ARangeWeapon
        constexpr int Weapon_Mesh             = 0x02B8; // AEquipableItem::Mesh (USkeletalMeshComponent*) -- weapon RGB / chams
        constexpr int Weapon_ItemDataAsset    = 0x0340; // inherited AEquipableItem::ItemDataAsset
        constexpr int Weapon_AmmoSize         = 0x05D8; // magazine capacity (int32)
        constexpr int Weapon_StartAmmoCount   = 0x05DC;
        constexpr int RangeWeapon_Bullet      = 0x09B8; // TSubclassOf<AAHBullet>
        constexpr int RangeWeapon_Barrel      = 0x09C0; // EasyBallistics.UEBBarrel*

        // EasyBallistics.UEBBarrel
        constexpr int EBBarrel_ShootingBlocked    = 0x0519;
        constexpr int EBBarrel_CycleAmmoUnlimited = 0x0531;
        constexpr int EBBarrel_Ammo               = 0x0538; // TArray<TSubclassOf<AEBBullet>>
        constexpr int EBBarrel_CycleAmmoCount     = 0x0548;
        constexpr int EBBarrel_CycleAmmoPos       = 0x054C;
        constexpr int EBBarrel_ChamberedBullet    = 0x0560;
        constexpr int EBBarrel_LoadNext           = 0x0570;

        // Engine.Actor / AHBaseCharacter functions for ProcessEvent
        constexpr const char* Fn_GetActorLocation = "Function /Script/Engine.Actor.K2_GetActorLocation";
        constexpr const char* Fn_SetActorLocation = "Function /Script/Engine.Actor.K2_SetActorLocation";
        constexpr const char* Fn_SetActorEnableCollision = "Function /Script/Engine.Actor.SetActorEnableCollision"; // noclip: pass through geometry
        constexpr const char* Fn_K2DestroyActor = "Function /Script/Engine.Actor.K2_DestroyActor";                 // delete an actor from the world outright
        constexpr const char* Fn_SetActorScale3D = "Function /Script/Engine.Actor.SetActorScale3D";                 // giant / tiny player
        constexpr const char* Fn_LaunchCharacter = "Function /Script/Engine.Character.LaunchCharacter";             // ragdoll-launch enemies skyward
        constexpr const char* Fn_BaseWeapon_FullUpgrade = "Function /Script/AtomicHeart.BaseWeapon.FullUpgrade";    // max weapon upgrades
        constexpr const char* Fn_GetControlRotation = "Function /Script/Engine.Controller.GetControlRotation";
        constexpr const char* Fn_InvalidateStreaming = "Function /Script/AtomicHeart.StreamingUtils.InvalidateStreaming";
        constexpr const char* Fn_GetAHWorldStreamingSubsystem = "Function /Script/AtomicHeart.SubsystemUtils.GetAHWorldStreamingSubsystem";
        constexpr const char* Fn_EnableLevelStreaming = "Function /Script/AtomicHeart.AHWorldStreamingSubsystem.EnableLevelStreaming";
        constexpr const char* Fn_GetCurrentWeapon = "Function /Script/AtomicHeart.AHBaseCharacter.GetCurrentWeapon";
        constexpr const char* Fn_GetWeaponInventoryAmmoCount = "Function /Script/AtomicHeart.AHBaseCharacter.GetWeaponInventoryAmmoCount";
        constexpr const char* Fn_InstantTakeWeapon = "Function /Script/AtomicHeart.AHPlayerCharacter.InstantTakeWeapon";
        constexpr const char* Fn_TakeWeapon = "Function /Script/AtomicHeart.AHPlayerCharacter.TakeWeapon";
        constexpr const char* Fn_EquipWeaponByDataAsset = "Function /Script/AtomicHeart.AHPlayerCharacter.EquipWeaponByDataAsset";
        constexpr const char* Fn_FindWeaponByDataAsset = "Function /Script/AtomicHeart.AHPlayerCharacter.FindWeaponByDataAsset";
        constexpr const char* Fn_GetCurrentWeaponInventoryAmmoCount = "Function /Script/AtomicHeart.AHPlayerCharacter.GetCurrentWeaponInventoryAmmoCount";
        constexpr const char* Fn_GetInventoryPlayer = "Function /Script/AtomicHeart.AHPlayerCharacter.GetInventoryPlayer";
        constexpr const char* Fn_AHInventory_AddItemsToInventory = "Function /Script/AtomicHeart.AHInventory.AddItemsToInventory";
        constexpr const char* Fn_AHInventory_GetItemsCount = "Function /Script/AtomicHeart.AHInventory.GetItemsCount";
        constexpr const char* Fn_AHInventory_SetIgnoreOverWeight = "Function /Script/AtomicHeart.AHInventory.SetIgnoreOverWeight";
        constexpr const char* Fn_BaseWeapon_GetAmmoInPossession = "Function /Script/AtomicHeart.BaseWeapon.GetAmmoInPossession";
        constexpr const char* Fn_BaseWeapon_GetAmmoCount = "Function /Script/AtomicHeart.BaseWeapon.GetAmmoCount";
        constexpr const char* Fn_BaseWeapon_GetAmmoSize = "Function /Script/AtomicHeart.BaseWeapon.GetAmmoSize";
        constexpr const char* Fn_EBBarrel_GetAmmoCount = "Function /Script/EasyBallistics.EBBarrel.GetAmmoCount";
        constexpr const char* Fn_EBBarrel_SetAmmo = "Function /Script/EasyBallistics.EBBarrel.SetAmmo";
        constexpr const char* Fn_SetIgnoreLookInput = "Function /Script/Engine.Controller.SetIgnoreLookInput";
        constexpr const char* Fn_SetIgnoreMoveInput = "Function /Script/Engine.Controller.SetIgnoreMoveInput";
        constexpr const char* Fn_Debug_InstantLockUnlock = "Function /Script/AtomicHeart.DebugSubsystem.InstantLockUnlock";
        constexpr const char* Fn_Debug_SetInstantPuzzleResolve = "Function /Script/AtomicHeart.DebugSubsystem.SetInstantPuzzleResolve";
        constexpr const char* Fn_Debug_WinQTE = "Function /Script/AtomicHeart.DebugSubsystem.WinQTE";
        // Quest progression debug drivers (UDebugSubsystem, no params). Promote =
        // advance every active quest ONE step (skip the current objective, e.g. a
        // "you need a ticket" gate); Complete = finish every active quest outright.
        constexpr const char* Fn_Debug_PromoteAllActiveQuests  = "Function /Script/AtomicHeart.DebugSubsystem.PromoteAllActiveQuests";
        constexpr const char* Fn_Debug_CompleteAllActiveQuests = "Function /Script/AtomicHeart.DebugSubsystem.CompleteAllActiveQuests";

        // BPC_MiniGameBase_C: the component that drives the interactive puzzle
        // minigames the DebugSubsystem trio does NOT cover (button grids, the
        // tri-way electric lockpick, radial/beam dials, etc.). Every puzzle
        // minigame subclass (e.g. MiniGame_TriWay_C) derives from this base, so
        // an IsA(base) sweep catches them all, and neither completion function
        // is overridden by subclasses -- calling the base versions is enough.
        //
        // Cls_MiniGameBase resolves the base UClass (full-name substring, run on
        // the worker thread). The two completion functions are resolved by SHORT
        // name via a walk of that class's Children list -- BP content packages
        // live under a "/Game/..." mount path at runtime, so a "Function Pkg..."
        // full-name needle never matches (the leading "Function " is not adjacent
        // to the package path); the children walk sidesteps the path entirely.
        constexpr const char* Cls_MiniGameBase        = "BPC_MiniGameBase.BPC_MiniGameBase_C";
        constexpr const char* Fn_MiniGame_SetComplete = "MiniGame_SetComplete"; // short name (children walk)
        constexpr const char* Fn_MiniGame_GameComplete = "SetGameComplete";     // short name (children walk)
        constexpr int MiniGame_IsGameComplete = 0x3D2; // bool; best-effort dedupe so we don't re-fire forever

        // BP_LockComponent_C (: ULockComponent): Atomic Heart's universal lock
        // component. It drives the door locks that are NOT BPC_MiniGameBase
        // minigames -- the CodeLock button grid (the circular CCCP plate from the
        // screenshot, LockButton x21), ColorsLockPick, CoinLock, UniversalLock.
        // Its parameterless Unlock() opens the lock and broadcasts OnUnlocked, so
        // it covers every lock variant at once. The DebugSubsystem.InstantLockUnlock
        // path does NOT touch these. Resolve Unlock via the class Children walk.
        constexpr const char* Cls_LockComponent = "BP_LockComponent.BP_LockComponent_C";
        constexpr const char* Fn_Lock_Unlock    = "Unlock"; // short name (children walk)
        constexpr int Lock_IsLocked = 0x188; // bool; dedupe -> skip in poll mode once already unlocked

        // AUniversalLock (: AObservableObject): the ACTOR behind multi-part door
        // locks -- the circular CodeLock button grid is a UniversalLock part.
        // OnCompleteAllParts() marks every part solved and runs the open path
        // (SendUsedToTargets). Native /Script class, so IsA(this) catches the
        // BP_UniversalLock_C instance. No simple "solved" bool to dedupe on
        // (SolvedParts is a TArray<bool>), so this target only fires on an
        // explicit Solve press / toggle enable-edge, never the steady poll.
        constexpr const char* Cls_UniversalLock            = "AtomicHeart.UniversalLock";
        constexpr const char* Fn_UniversalLock_CompleteAll      = "OnCompleteAllParts";      // short name (children walk)
        constexpr const char* Fn_UniversalLock_CompleteAllEvent = "OnCompleteAllPartsEvent"; // short name (children walk)

        // World / visuals helpers.
        // UGameplayStatics is a static-only class; call its functions on the CDO.
        constexpr const char* Fn_SetGlobalTimeDilation = "Function /Script/Engine.GameplayStatics.SetGlobalTimeDilation";
        constexpr const char* Obj_GameplayStatics      = "Engine.Default__GameplayStatics";
        // ESP target: enemy AI characters (AAHAICharacter : AAHBaseCharacter).
        constexpr const char* Cls_AICharacter          = "AtomicHeart.AHAICharacter";
        constexpr const char* Cls_AHAIController       = "AtomicHeart.AHAIController";
        constexpr const char* Cls_AIController         = "AIModule.AIController";
        constexpr const char* Cls_AIBlueprintHelper    = "AIModule.AIBlueprintHelperLibrary";
        constexpr const char* Cls_Pawn                 = "Engine.Pawn";
        constexpr const char* Cls_Character            = "Engine.Character";

        // Native AI controls.
        // Character-level team / attitude (defined on AAHBaseCharacter, inherited by
        // AHAICharacter). These route through the engine's IGenericTeamAgentInterface
        // so they refresh the perception/attitude solver -- the RELIABLE way to make
        // a unit hostile/friendly. Poking the raw controller team byte did NOT refresh
        // that cache, so released guards stayed friendly = unkillable ("godmode leak").
        constexpr const char* Fn_AHBaseCharacter_SetGenericTeamId = "Function /Script/AtomicHeart.AHBaseCharacter.SetGenericTeamId";
        constexpr const char* Fn_AHBaseCharacter_GetGenericTeamId = "Function /Script/AtomicHeart.AHBaseCharacter.GetGenericTeamId";
        constexpr const char* Fn_AHBaseCharacter_Revive           = "Function /Script/AtomicHeart.AHBaseCharacter.Revive";

        constexpr const char* Fn_AHAICharacter_SetIsPassive = "Function /Script/AtomicHeart.AHAICharacter.SetIsPassive";
        constexpr const char* Fn_AHAICharacter_SetTargetAlly = "Function /Script/AtomicHeart.AHAICharacter.SetTargetAlly";
        constexpr const char* Fn_AHAICharacter_SetTargetEnemy = "Function /Script/AtomicHeart.AHAICharacter.SetTargetEnemy";
        constexpr const char* Fn_AHAICharacter_Suicide = "Function /Script/AtomicHeart.AHAICharacter.Suicide";
        constexpr const char* Fn_AHAICharacter_SwitchTeamToMatchCharacterAttitude = "Function /Script/AtomicHeart.AHAICharacter.SwitchTeamToMatchCharacterAttitude";
        constexpr const char* Fn_AHAICharacter_GetAIController = "Function /Script/AtomicHeart.AHAICharacter.GetAIController";
        constexpr const char* Fn_AHAIController_PauseBehaviorTree = "Function /Script/AtomicHeart.AHAIController.PauseBehaviorTree";
        constexpr const char* Fn_AHAIController_ResumeBehaviorTree = "Function /Script/AtomicHeart.AHAIController.ResumeBehaviorTree";
        constexpr const char* Fn_AHAIController_StopBehaviorTree = "Function /Script/AtomicHeart.AHAIController.StopBehaviorTree";
        constexpr const char* Fn_AHAIController_StartBehaviorTree = "Function /Script/AtomicHeart.AHAIController.StartBehaviorTree";
        constexpr const char* Fn_AHAIController_SetBlackboardFollowLocation = "Function /Script/AtomicHeart.AHAIController.SetBlackboardFollowLocation";
        constexpr const char* Fn_AHAIController_SetBlackboardIsAggressive = "Function /Script/AtomicHeart.AHAIController.SetBlackboardIsAggressive";
        constexpr const char* Fn_AHAIController_SetBlackboardTargetAlly = "Function /Script/AtomicHeart.AHAIController.SetBlackboardTargetAlly";
        constexpr const char* Fn_AHAIController_SetBlackboardTargetEnemy = "Function /Script/AtomicHeart.AHAIController.SetBlackboardTargetEnemy";
        constexpr const char* Fn_AHAIController_SetFollowLocationSpeed = "Function /Script/AtomicHeart.AHAIController.SetFollowLocationSpeed";
        constexpr const char* Fn_AHAIController_SetSenseHearingEnabled = "Function /Script/AtomicHeart.AHAIController.SetSenseHearingEnabled";
        constexpr const char* Fn_AHAIController_SetSenseSightEnabled = "Function /Script/AtomicHeart.AHAIController.SetSenseSightEnabled";

        // Generic AIModule helpers.
        constexpr const char* Fn_AIController_MoveToActor = "Function /Script/AIModule.AIController.MoveToActor";
        constexpr const char* Fn_AIController_K2_SetFocus = "Function /Script/AIModule.AIController.K2_SetFocus";
        constexpr const char* Fn_AIBlueprintHelper_SpawnAIFromClass = "Function /Script/AIModule.AIBlueprintHelperLibrary.SpawnAIFromClass";
        constexpr const char* Fn_AIBlueprintHelper_SimpleMoveToActor = "Function /Script/AIModule.AIBlueprintHelperLibrary.SimpleMoveToActor"; // reliable nav walk toward an actor
        constexpr const char* Obj_AIBlueprintHelper = "AIModule.Default__AIBlueprintHelperLibrary";
        constexpr const char* Fn_Pawn_AddMovementInput = "Function /Script/Engine.Pawn.AddMovementInput"; // direct locomotion nudge (walk anim)

        // MERCUNA nav -- THE game's actual mover for ground AI (the BTs use
        // UBTTask_Mercuna_MoveTo). UMercunaNavigationComponent lives on the AI pawn;
        // MoveToActor(Actor, EndDistance, Speed, UsePartialPath) paths it to a target with
        // real locomotion + animation. This is why UE MoveToActor did nothing (Mercuna
        // replaces the nav). Resolve the component via GetComponentsByClass on the pawn.
        constexpr const char* Cls_MercunaNavComponent = "Mercuna.MercunaNavigationComponent";
        constexpr const char* Fn_Mercuna_MoveToActor  = "Function /Script/Mercuna.MercunaNavigationComponent.MoveToActor";
        constexpr const char* Fn_Mercuna_MoveToLocation = "Function /Script/Mercuna.MercunaNavigationComponent.MoveToLocation";
        constexpr const char* Fn_Mercuna_Stop         = "Function /Script/Mercuna.MercunaNavigationComponent.Stop";

        // On-demand asset loading (KismetSystemLibrary). MakeSoftClassPath builds a
        // soft path FROM A STRING (no FName construction needed), then
        // LoadClassAsset_Blocking synchronously loads + returns the UClass*. This is
        // how we spawn a saved NPC/boss WITHOUT being near it (no proximity needed).
        constexpr const char* Fn_MakeSoftClassPath        = "Function /Script/Engine.KismetSystemLibrary.MakeSoftClassPath";
        constexpr const char* Fn_LoadClassAsset_Blocking  = "Function /Script/Engine.KismetSystemLibrary.LoadClassAsset_Blocking";

        // ===================================================================
        //  DEEP AI AGGRO INJECTION (make enemies truly fight + bodyguards)
        // ---------------------------------------------------------------------
        //  Atomic Heart never has enemies attack each other, so the only way to
        //  force it is to write the target/aggro state directly and force the
        //  AI's behaviour tree into the attack branch -- NOT by routing damage.
        //  All of these are taken straight from the Dumper-7 SDK
        //  (AtomicHeart_classes.hpp: AAHAICharacter / AAHAIController).
        // ===================================================================

        // AAHAICharacter direct fields (raw writes -- no ProcessEvent, crash-safe
        // behind Mem::IsReadable). These ARE the aggro/target state the engine
        // reads, so writing them is the "bypass the damage system entirely" path.
        constexpr int AICh_bIsAlwaysAggressive   = 0x14E5; // bool: never go idle, always hunt
        constexpr int AICh_bIsPassive            = 0x15D8; // bool: passive => won't attack
        constexpr int AICh_bPassiveButWithSenses = 0x15D9; // bool
        constexpr int AICh_bActorTickEnabled     = 0x1C58; // bool (significance gate)
        constexpr int AICh_bMeshTickEnabled      = 0x1C59; // bool (significance gate -> visible)
        constexpr int AICh_CachedTargetEnemy     = 0x1C28; // AAHBaseCharacter* : the cached aggro target
        constexpr int AICh_LastSensedCharacter   = 0x1C30; // AAHBaseCharacter*
        constexpr int AICh_OwnerCharacter        = 0x1A48; // AAHBaseCharacter*
        // AAHAICharacter::OrdinaryBehaviorTree (verified from the Larisa dump): the
        // BT asset that defines the NPC's behaviour. Non-combat NPCs (Larisa, civilians)
        // run a "BT_Pedestrian"-style tree with NO combat state machine, so
        // SetCharacterAggressive AVs deep in game code on them -- we gate combat on
        // this BT's name to make "squad attacks" crash-safe for any character.
        constexpr int AICh_OrdinaryBehaviorTree  = 0x1438; // UBehaviorTree*
        // AAHAICharacter schedule system (verified from the Larisa walk capture): the
        // HIGH-LEVEL driver of NPC movement. The game makes Larisa walk by switching
        // Schedule from "..._Idle_NoOnteraction" to "..._MoveWholeCorridor" (a
        // DA_AISchedule asset); SavedSchedule stashes the previous one. NOTE 0x1788 is
        // Schedule on AHAICharacter (NOT the player's InventoryPlayer at the same
        // offset on AHPlayerCharacter -- different subclass). Use only on AHAICharacters.
        constexpr int AICh_Schedule              = 0x1788; // UDA_AISchedule* (current)
        constexpr int AICh_SavedSchedule         = 0x2078; // UDA_AISchedule* (previous)
        constexpr int AICh_WaitingSchedule       = 0x2080; // UDA_AISchedule*

        // AAHAIController / inherited AAIController direct fields.
        constexpr int AICtrl_PathFollowing = 0x02F8; // inherited AAIController::PathFollowingComponent
        constexpr int AICtrl_BrainComp     = 0x0300; // inherited AAIController::BrainComponent
        constexpr int AICtrl_Blackboard    = 0x0318; // inherited AAIController::Blackboard (UBlackboardComponent*)
        constexpr int AICtrl_TeamID        = 0x0401; // uint8 generic team id (different id => hostile => real damage)
        // AtomicHeart_classes.hpp: AAHAIController key-name fields.
        constexpr int AICtrl_Key_SelfActor         = 0x0348;
        constexpr int AICtrl_Key_TargetEnemy       = 0x0350;
        constexpr int AICtrl_Key_TargetAlly        = 0x0358;
        constexpr int AICtrl_Key_TargetObject      = 0x0360;
        constexpr int AICtrl_Key_CurrentWaypoint   = 0x0368;
        constexpr int AICtrl_Key_Duration          = 0x0370;
        constexpr int AICtrl_Key_FollowLocation    = 0x0378;
        constexpr int AICtrl_Key_ForceFollowLoc    = 0x0380;
        constexpr int AICtrl_Key_IsAggressive      = 0x0388;
        constexpr int AICtrl_Key_CanReachFollowLoc = 0x03A8;
        constexpr int AICtrl_Key_BehaviorState     = 0x03B0;
        constexpr int AICtrl_Key_AcceptableRadius  = 0x03F8;

        // AIModule_classes.hpp: UBlackboardComponent / UBlackboardData.
        constexpr int BBComp_BrainComp             = 0x00D0;
        constexpr int BBComp_DefaultAsset          = 0x00D8;
        constexpr int BBComp_Asset                 = 0x00E0;
        constexpr int BBComp_KeyInstances          = 0x0108;
        constexpr int BBData_Parent                = 0x0038;
        constexpr int BBData_Keys                  = 0x0040;
        constexpr int BBEntry_Stride               = 0x0018;
        constexpr int BBEntry_Name                 = 0x0000;
        constexpr int BBEntry_KeyType              = 0x0008;

        // AIModule_classes.hpp: UPathFollowingComponent.
        constexpr int PathFollow_MovementComp      = 0x0108;
        constexpr int PathFollow_NavData           = 0x0118;

        // ACharacter::Mesh (USkeletalMeshComponent*) -- used to force a freshly
        // spawned ally's mesh visible (fixes "invisible body, ESP-only" spawns).
        constexpr int Char_Mesh = 0x2A0;

        // AtomicHeart.AIUtils: a UBlueprintFunctionLibrary of native AI drivers.
        // SetCharacterAggressive is THE attack-forcing primitive: it flips the AI
        // into its aggressive state machine with a chosen target loaded. Called on
        // the AIUtils CDO with the AI character + target as parameters.
        constexpr const char* Cls_AIUtils = "AtomicHeart.AIUtils";
        constexpr const char* Obj_AIUtils = "AtomicHeart.Default__AIUtils";
        constexpr const char* Fn_AIUtils_SetCharacterAggressive = "Function /Script/AtomicHeart.AIUtils.SetCharacterAggressive";
        constexpr const char* Fn_AIUtils_SetCharacterPassive    = "Function /Script/AtomicHeart.AIUtils.SetCharacterPassive";
        constexpr const char* Fn_AIUtils_RestartLogic           = "Function /Script/AtomicHeart.AIUtils.RestartLogic";
        constexpr const char* Fn_AIUtils_AreFriendlyCharacters  = "Function /Script/AtomicHeart.AIUtils.AreFriendlyCharacters";

        // AHAICharacter helper for placement (short name; children walk).
        constexpr const char* Fn_AHAICharacter_SnapCapsuleToGround = "Function /Script/AtomicHeart.AHAICharacter.SnapCapsuleToGround";

        // Engine.Actor / SceneComponent visibility (spawn fixup, full names).
        constexpr const char* Fn_SetActorHiddenInGame = "Function /Script/Engine.Actor.SetActorHiddenInGame";
        constexpr const char* Fn_SetActorTickEnabled  = "Function /Script/Engine.Actor.SetActorTickEnabled";
        constexpr const char* Fn_K2_TeleportTo        = "Function /Script/Engine.Actor.K2_TeleportTo";
        constexpr const char* Fn_Comp_SetVisibility   = "Function /Script/Engine.SceneComponent.SetVisibility";
        constexpr const char* Fn_Comp_SetHiddenInGame = "Function /Script/Engine.SceneComponent.SetHiddenInGame";
        constexpr const char* Fn_ActorGetComponentsByClass = "Function /Script/Engine.Actor.K2_GetComponentsByClass";

        // ===================================================================
        //  RENDER HIJACK (chams / world tint / console)
        // ---------------------------------------------------------------------
        //  We change the game's appearance through the engine's OWN render
        //  objects (materials, lights, post-process, cvars) instead of hooking
        //  D3D12 draw calls / swapping PSOs per-draw -- the latter is the
        //  crash-prone path on a DLSS title with immutable PSOs. All of these
        //  are /Script engine functions (resolve via CachedFn / FindObjectFast).
        // ===================================================================

        // Console: UKismetSystemLibrary.ExecuteConsoleCommand(world, FString, pc).
        // Single most powerful "change the look" lever -- viewmodes (wireframe/
        // unlit), show flags, r.* cvars, slomo, screenshots, etc.
        constexpr const char* Obj_KismetSystemLibrary  = "Engine.Default__KismetSystemLibrary";

        // Asset Registry: enumerate EVERY character blueprint class in the game + DLC
        // (even ones never loaded this session -- bosses, DLC enemies) without loading
        // them, then load-on-demand by path at spawn time (LoadClassByPath).
        constexpr const char* Obj_AssetRegistryHelpers = "AssetRegistry.Default__AssetRegistryHelpers";
        constexpr const char* Fn_GetAssetRegistry      = "Function /Script/AssetRegistry.AssetRegistryHelpers.GetAssetRegistry";
        constexpr const char* Fn_GetAssetsByClass      = "Function /Script/AssetRegistry.AssetRegistry.GetAssetsByClass";
        constexpr const char* Fn_ExecuteConsoleCommand = "Function /Script/Engine.KismetSystemLibrary.ExecuteConsoleCommand";

        // Lights: ALight.SetLightColor(FLinearColor) -- no FName needed, so it is
        // fully reliable. Tinting the directional (sun) + point/spot lights recolours
        // the whole scene / sky.
        constexpr const char* Cls_Light             = "Engine.Light";
        constexpr const char* Fn_Light_SetLightColor = "Function /Script/Engine.Light.SetLightColor";

        // Chams: drive the enemy SkeletalMesh component's materials. The "...OnMaterials"
        // helpers create dynamic instances internally and set a named parameter across
        // every slot, so we never have to manage MIDs. Custom depth = best-effort
        // see-through (needs a custom-depth post-process to actually show through walls).
        constexpr const char* Fn_MeshSetVectorParamOnMaterials = "Function /Script/Engine.MeshComponent.SetVectorParameterValueOnMaterials";
        constexpr const char* Fn_MeshSetScalarParamOnMaterials = "Function /Script/Engine.MeshComponent.SetScalarParameterValueOnMaterials";
        // Read the material's OWN parameter names (live FNames) so recolor confidently
        // hits whatever param the material actually uses, instead of guessing names.
        constexpr const char* Fn_PrimGetMaterial        = "Function /Script/Engine.PrimitiveComponent.GetMaterial";
        constexpr const char* Fn_PrimGetNumMaterials    = "Function /Script/Engine.PrimitiveComponent.GetNumMaterials";
        constexpr const char* Fn_PrimSetMaterial        = "Function /Script/Engine.PrimitiveComponent.SetMaterial";
        constexpr const char* Fn_PrimCreateMID          = "Function /Script/Engine.PrimitiveComponent.CreateAndSetMaterialInstanceDynamic";
        constexpr const char* Fn_PrimCreateMIDFromMaterial = "Function /Script/Engine.PrimitiveComponent.CreateAndSetMaterialInstanceDynamicFromMaterial";
        constexpr const char* Fn_MIDSetVectorParam      = "Function /Script/Engine.MaterialInstanceDynamic.SetVectorParameterValue";
        constexpr const char* Fn_MIDSetScalarParam      = "Function /Script/Engine.MaterialInstanceDynamic.SetScalarParameterValue";
        constexpr const char* Obj_WeaponRgbDebugMaterial    = "Material /Engine/EngineDebugMaterials/DebugMeshMaterial.DebugMeshMaterial";
        constexpr const char* Obj_WeaponRgbBasicShapeMaterial = "Material /Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial";
        constexpr const char* Obj_WeaponRgbDefaultMaterial = "Material /Engine/EngineMaterials/DefaultMaterial.DefaultMaterial";
        constexpr const char* Obj_WeaponRgbWorldGridMaterial = "Material /Engine/EngineMaterials/WorldGridMaterial.WorldGridMaterial";
        constexpr const char* Obj_WeaponRgbGameEmissiveMaterial = "Material /Game/Development/MaterialLibrary/MasterMaterials/M_Emissive.M_Emissive";
        constexpr const char* Obj_WeaponRgbTurretEmissiveMaterial = "Material /Game/Development/FX/TurretFx/Materials/_MasterMaterials/Master_EmissiveSimple.Master_EmissiveSimple";
        constexpr const char* Obj_WeaponRgbScannerSelectMaterial = "Material /Game/Development/FX/Scanner/Materials/MPP_ScannerTranslucentSelect.MPP_ScannerTranslucentSelect";
        constexpr const char* Cls_MeshComponent      = "Engine.MeshComponent";
        constexpr const char* Cls_MaterialInstance      = "Engine.MaterialInstance";
        constexpr int Mat_ScalarParameterValues = 0x00E8; // UMaterialInstance::ScalarParameterValues (TArray<FScalarParameterValue>)
        constexpr int Mat_VectorParameterValues = 0x00F8; // UMaterialInstance::VectorParameterValues (TArray<FVectorParameterValue>)
        constexpr int ScalarParamValue_Stride   = 0x24;   // sizeof(FScalarParameterValue)
        constexpr int ScalarParamValue_NameOff  = 0x00;   // ParameterInfo.Name
        constexpr int ScalarParamValue_ValueOff = 0x10;   // float ParameterValue
        constexpr int VectorParamValue_Stride   = 0x30;   // sizeof(FVectorParameterValue): info 0x10 + color 0x10 + guid 0x10
        constexpr int VectorParamValue_NameOff  = 0x00;   // ParameterInfo.Name (FName) at the start
        constexpr int VectorParamValue_ValueOff = 0x10;   // FLinearColor ParameterValue
        constexpr const char* Fn_PrimSetRenderCustomDepth      = "Function /Script/Engine.PrimitiveComponent.SetRenderCustomDepth";
        constexpr const char* Fn_PrimSetCustomDepthStencil     = "Function /Script/Engine.PrimitiveComponent.SetCustomDepthStencilValue";
    }
}
