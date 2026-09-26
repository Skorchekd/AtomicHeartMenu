// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Skorchekd. Preserve attribution; see LICENSE/NOTICE.
#pragma once
#include "../sdk/ue4.h"
#include <string>
#include <vector>

// Engine-facing primitives the companion policy is written against. Implemented in
// features.cpp over the real game; the policy tests substitute a fake engine.
namespace BodyguardEngine
{
    bool Usable(UE::UObject* actor);
    bool CombatCapable(UE::UObject* actor);
    bool Friendly(UE::UObject* actor, UE::UObject* player);
    bool Protected(UE::UObject* actor);
    bool LiveEnemy(UE::UObject* actor);
    UE::UObject* Target(UE::UObject* actor);
    // The TargetEnemy key of the AI's own blackboard. Native perception writes it
    // without going through ProcessEvent, so it can name the player even when the
    // cached target field does not.
    UE::UObject* BlackboardTarget(UE::UObject* actor);
    // The character this AI most recently sensed (saw or heard), or null.
    UE::UObject* Sensed(UE::UObject* actor);
    // The engine's team attitude says this AI is hostile toward the player.
    bool HostileTo(UE::UObject* actor, UE::UObject* player);
    // Flagged passive (peaceful worker robots, scripted NPCs). Never chosen
    // automatically, so companions do not start fights the game did not.
    bool Passive(UE::UObject* actor);
    bool Health(UE::UObject* actor, float& fraction);
    bool Location(UE::UObject* actor, UE::FVector& out);
    void ClearCombat(UE::UObject* actor);
    bool Attack(UE::UObject* actor, UE::UObject* enemy, UE::UObject* player);
    void StopMovement(UE::UObject* actor);
    void PrepareFollow(UE::UObject* actor, UE::UObject* player, const UE::FVector& location);
}

namespace Bodyguards
{
    // Values are part of the agent-harness protocol (squad_order) and the menu.
    enum class Order { FollowAndDefend = 0, FollowOnly = 1, Hold = 2, FollowAndAttack = 3 };

    struct Settings
    {
        // Enemies attacking you or a companion are engaged inside this range.
        float defendRadiusM = 30.0f;
        // Hostile enemies that have already spotted you are engaged inside this
        // range, before they get a hit in.
        float interceptRadiusM = 15.0f;
        // A fight is dropped once the enemy is this far from you, so companions do
        // not chase across the level and leave you alone.
        float leashRadiusM = 45.0f;
    };
    void Configure(const Settings& settings);
    Settings CurrentSettings();

    struct Threat
    {
        UE::UObject* actor = nullptr;
        UE::FVector  location{};
    };

    struct Summary
    {
        unsigned long long id = 0;
        std::string name;
        std::string activity;
        std::string target;
        Order order = Order::FollowAndDefend;
        bool friendly = false;
        bool combatCapable = false;
        float distanceM = -1;
        float healthFrac = -1;
        unsigned protectedTargetsCleared = 0;
    };
    // Engine mutations run only on the game thread. Snapshot/AllowsFollow are
    // read-only and copy state without holding a lock across engine calls.
    bool Adopt(UE::UObject* actor, UE::UObject* player);
    void Forget(UE::UObject* actor);
    bool Contains(UE::UObject* actor);
    void SetOrder(UE::UObject* actor, Order order);
    bool GetOrder(UE::UObject* actor, Order& out);
    bool AttackTarget(UE::UObject* actor, UE::UObject* target);
    // One companion against one candidate threat (kept for the policy tests).
    void Update(UE::UObject* actor, UE::UObject* player, const UE::FVector& playerLocation,
                UE::UObject* candidate);
    // The whole squad against the current threat list. Whoever is attacking you is
    // answered first, and simultaneous attackers are shared out instead of every
    // companion piling onto the nearest one while another enemy hits you.
    void UpdateAll(const std::vector<UE::UObject*>& members, UE::UObject* player,
                   const UE::FVector& playerLocation, const std::vector<Threat>& threats);
    bool AllowsFollow(UE::UObject* actor);
    std::vector<Summary> Snapshot();
    std::string SnapshotJson();
}

#include "../../deps/nlohmann/json.hpp"
