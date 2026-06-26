// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
namespace UE { struct UObject; struct FVector; }
namespace AiMovementHooks
{
    struct Status
    {
        bool moveHelperResolved = false, mixedTransitionResolved = false, mercunaDetoursLive = false;
        bool controllerDetoursLive = false;
        std::uintptr_t moveHelper = 0, mixedTransition = 0, moveRequestTarget = 0, stopTarget = 0, cancelTarget = 0;
        std::uintptr_t controllerMoveTarget = 0, controllerStopTarget = 0;
        std::uintptr_t controllerPathCommitTarget = 0, controllerPathBuildTarget = 0;
        std::uintptr_t pathRequestTarget = 0, pathAbortTarget = 0;
        std::uintptr_t movementCanStartTarget = 0;
        std::uint64_t nativeMovesIssued = 0, nativeStopsIssued = 0;
        std::uint64_t externalMovesBlocked = 0, externalStopsBlocked = 0, externalCancelsBlocked = 0;
        std::uint64_t controllerMovesBlocked = 0, controllerStopsBlocked = 0;
        std::uint64_t controllerMovesIssued = 0, controllerStopsIssued = 0;
        std::uint64_t controllerMoveCalls = 0, controllerPathCommits = 0, controllerPathBuilds = 0;
        std::uint64_t pathRequests = 0, pathAborts = 0;
        std::uint64_t twinGenericBypasses = 0, twinOriginalMoveAttempts = 0, twinOriginalMoveAccepted = 0;
        std::uint64_t twinGenericFallbacks = 0, twinGenericFallbackAccepted = 0;
        std::uint64_t movementCanStartCalls = 0, movementCanStartForced = 0;
        std::uint64_t mixedTransitions = 0;
        int registeredComponents = 0, ownedComponents = 0, registeredPathFollowers = 0;
        int registeredMovementComponents = 0;
    };
    bool ResolveHelpers(UE::UObject*, UE::UObject*);
    bool RegisterMercunaComponent(UE::UObject*, UE::UObject*, bool);
    bool RegisterController(UE::UObject*, UE::UObject*);
    void UnregisterGuard(UE::UObject*);
    void SetMercunaOwned(UE::UObject*, UE::UObject*, bool);
    void SetControllerOwned(UE::UObject*, UE::UObject*, bool);
    void SetControllerCallInternal(bool);
    bool ControllerMoveToActor(UE::UObject*, UE::UObject*, UE::UObject*, float);
    bool ControllerStop(UE::UObject*, UE::UObject*);
    bool OwnsGuard(UE::UObject*);
    UE::UObject* RegisteredNavigation(UE::UObject*);
    bool MoveToLocation(UE::UObject*, const UE::FVector&, float, float, bool);
    bool Stop(UE::UObject*);
    bool ForceMixedNavigation(UE::UObject*, std::uint8_t);
    bool SetMixedAutomatic(UE::UObject*, bool);
    std::uint8_t CurrentMixedNavigation(UE::UObject*);
    Status GetStatus();
    void DumpStatus();
    void Shutdown();
}

