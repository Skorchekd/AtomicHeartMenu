// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Skorchekd. Preserve attribution; see LICENSE/NOTICE.
#pragma once
#include "../sdk/ue4.h"
#include <string>
#include <vector>

namespace BodyguardEngine
{
    bool Usable(UE::UObject* actor);
    bool CombatCapable(UE::UObject* actor);
    bool Friendly(UE::UObject* actor, UE::UObject* player);
    bool Protected(UE::UObject* actor);
    bool LiveEnemy(UE::UObject* actor);
    UE::UObject* Target(UE::UObject* actor);
    bool Location(UE::UObject* actor, UE::FVector& out);
    void ClearCombat(UE::UObject* actor);
    bool Attack(UE::UObject* actor, UE::UObject* enemy, UE::UObject* player);
    void StopMovement(UE::UObject* actor);
    void PrepareFollow(UE::UObject* actor, UE::UObject* player, const UE::FVector& location);
}

namespace Bodyguards
{
    enum class Order { FollowAndDefend, FollowOnly, Hold };
    struct Summary
    {
        unsigned long long id = 0;
        std::string name;
        std::string activity;
        Order order = Order::FollowAndDefend;
        bool friendly = false;
        bool combatCapable = false;
        float distanceM = -1;
        unsigned protectedTargetsCleared = 0;
    };
    // Engine mutations run only on the game thread. Snapshot/AllowsFollow are
    // read-only and copy state without holding a lock across engine calls.
    bool Adopt(UE::UObject* actor, UE::UObject* player);
    void Forget(UE::UObject* actor);
    bool Contains(UE::UObject* actor);
    void SetOrder(UE::UObject* actor, Order order);
    bool AttackTarget(UE::UObject* actor, UE::UObject* target);
    void Update(UE::UObject* actor, UE::UObject* player, const UE::FVector& playerLocation,
                UE::UObject* candidate);
    bool AllowsFollow(UE::UObject* actor);
    std::vector<Summary> Snapshot();
    std::string SnapshotJson();
}

