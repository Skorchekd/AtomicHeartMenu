#include "../../deps/nlohmann/json.hpp"
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Skorchekd. Preserve attribution; see LICENSE/NOTICE.
#include "bodyguards.h"
#include "../core/log.h"
#include <Windows.h>
#include <algorithm>
#include <cmath>
#include <mutex>
#include <unordered_map>

namespace
{
    using namespace UE;
    struct Ref
    {
        UObject* actor = nullptr;
        int index = -1;
        FName name{};
        Ref() = default;
        explicit Ref(UObject* value)
        {
            if (IsLiveObject(value)) { actor = value; index = value->Index(); name = *value->NamePtr(); }
        }
        UObject* Get() const
        {
            if (!IsLiveObject(actor) || actor->Index() != index) return nullptr;
            FName current = *actor->NamePtr();
            return current.ComparisonIndex == name.ComparisonIndex && current.Number == name.Number ? actor : nullptr;
        }
    };
    struct State
    {
        Ref actor, enemy, player;
        Bodyguards::Summary summary;
        bool explicitAttack = false;
        bool engaged = false;
        ULONGLONG nextAllegianceCheck = 0;
        ULONGLONG lastProgressMs = 0;
        FVector lastPosition{};
    };
    std::mutex stateMutex;
    std::unordered_map<UObject*, State> states;

    bool Load(UObject* actor, State& state)
    {
        std::lock_guard<std::mutex> lock(stateMutex);
        auto found = states.find(actor);
        if (found == states.end()) return false;
        state = found->second;
        return true;
    }
    void Save(UObject* actor, const State& state)
    {
        std::lock_guard<std::mutex> lock(stateMutex);
        auto found = states.find(actor);
        if (found != states.end() && found->second.actor.index == state.actor.index)
            found->second = state;
    }
    float Distance(const FVector& a, const FVector& b)
    {
        float x = a.X-b.X, y = a.Y-b.Y, z = a.Z-b.Z;
        return std::sqrt(x*x+y*y+z*z) / 100.0f;
    }
    bool Enemy(UObject* target, UObject* actor, UObject* player)
    {
        return target && target != actor && target != player && BodyguardEngine::LiveEnemy(target) &&
            !BodyguardEngine::Protected(target);
    }
}

bool Bodyguards::Contains(UObject* actor)
{
    State state;
    return Load(actor, state) && state.actor.Get() == actor;
}
bool Bodyguards::Adopt(UObject* actor, UObject* player)
{
    if (!BodyguardEngine::Usable(actor) || !UE::IsLiveObject(player) || actor == player || !UE::IsLiveObject(actor->Class())) return false;
    if (Contains(actor)) return true;
    State state;
    state.actor = Ref(actor);
    state.player = Ref(player);
    state.summary.id = reinterpret_cast<unsigned long long>(actor);
    state.summary.name = actor->Class()->GetName();
    state.summary.activity = "Initializing";
    state.summary.combatCapable = BodyguardEngine::CombatCapable(actor);
    {
        std::lock_guard<std::mutex> lock(stateMutex);
        for (auto it = states.begin(); it != states.end(); )
            if (!it->second.actor.Get()) it = states.erase(it); else ++it;
        if (states.size() >= 24) return false;
        states[actor] = state; // protected before any initialization dispatch
    }
    BodyguardEngine::ClearCombat(actor);
    state.summary.friendly = BodyguardEngine::Friendly(actor, player);
    state.summary.activity = state.summary.friendly ? "Following" : "Waiting for friendly team";
    if (!state.summary.friendly) BodyguardEngine::StopMovement(actor);
    Save(actor, state);
    LOG("Bodyguard registered: actor=%p class=%s friendly=%d combat=%d",
        actor, state.summary.name.c_str(), state.summary.friendly, state.summary.combatCapable);
    return true; // registration is distinct from verified allegiance
}
void Bodyguards::Forget(UObject* actor)
{
    std::lock_guard<std::mutex> lock(stateMutex);
    states.erase(actor);
}
void Bodyguards::SetOrder(UObject* actor, Order order)
{
    State state;
    if (!Load(actor, state) || !state.actor.Get()) return;
    state.summary.order = order;
    state.enemy = {}; state.explicitAttack = state.engaged = false;
    BodyguardEngine::ClearCombat(actor);
    BodyguardEngine::StopMovement(actor);
    state.summary.activity = order == Order::Hold ? "Holding position" : "Following";
    Save(actor, state);
}
bool Bodyguards::AttackTarget(UObject* actor, UObject* target)
{
    State state;
    if (!Load(actor, state) || !state.actor.Get() || !state.summary.combatCapable ||
        !state.summary.friendly || !Enemy(target, actor, UE::GetLocalPawn())) return false;
    state.enemy = Ref(target); state.explicitAttack = true;
    state.summary.order = Order::FollowAndDefend;
    Save(actor, state);
    return true;
}
void Bodyguards::Update(UObject* actor, UObject* player, const UE::FVector& playerLocation,
                        UObject* candidate)
{
    State state;
    if (!Load(actor, state)) return;
    if (!state.actor.Get()) { Forget(actor); return; }
    if (!UE::IsLiveObject(player) || !BodyguardEngine::Usable(actor)) return;
    ULONGLONG now = GetTickCount64();
    UObject* currentTarget = BodyguardEngine::Target(actor);
    const bool unsafe = currentTarget && (currentTarget == player || BodyguardEngine::Protected(currentTarget));
    if (unsafe)
    {
        BodyguardEngine::ClearCombat(actor);
        ++state.summary.protectedTargetsCleared;
        state.engaged = false; state.enemy = {}; state.explicitAttack = false;
        state.nextAllegianceCheck = 0;
    }
    if (state.player.Get() != player || now >= state.nextAllegianceCheck)
    {
        state.player = Ref(player);
        state.summary.combatCapable = BodyguardEngine::CombatCapable(actor);
        state.summary.friendly = BodyguardEngine::Friendly(actor, player);
        state.nextAllegianceCheck = now + 750;
    }
    if (!state.summary.friendly)
    {
        BodyguardEngine::ClearCombat(actor);
        BodyguardEngine::StopMovement(actor);
        state.engaged = false;
        state.summary.activity = "Waiting for friendly team";
        Save(actor, state);
        return;
    }

    FVector location{};
    if (!BodyguardEngine::Location(actor, location)) return;
    state.summary.distanceM = Distance(location, playerLocation);
    if (!state.lastProgressMs || Distance(location, state.lastPosition) >= 0.2f)
    { state.lastPosition = location; state.lastProgressMs = now; }
    UObject* target = state.explicitAttack ? state.enemy.Get() : candidate;
    bool engage = state.summary.combatCapable && state.summary.order == Order::FollowAndDefend &&
        Enemy(target, actor, player);
    if (engage)
    {
        FVector targetLocation{};
        UObject* attacks = BodyguardEngine::Target(target);
        bool attackingProtected = attacks == player || (attacks && BodyguardEngine::Protected(attacks));
        engage = BodyguardEngine::Location(target, targetLocation) &&
            std::min(Distance(location, targetLocation), Distance(playerLocation, targetLocation)) <= 35.0f &&
            (state.explicitAttack || attackingProtected);
    }
    if (engage)
    {
        state.engaged = BodyguardEngine::Attack(actor, target, player);
        state.enemy = Ref(target);
        state.summary.friendly = BodyguardEngine::Friendly(actor, player);
        state.summary.activity = state.engaged && state.summary.friendly ? "Defending" : "Attack unavailable";
        if (!state.summary.friendly) BodyguardEngine::ClearCombat(actor);
    }
    else
    {
        if (state.engaged || state.enemy.Get() || currentTarget) BodyguardEngine::ClearCombat(actor);
        state.engaged = false; state.enemy = {}; state.explicitAttack = false;
        if (state.summary.order == Order::Hold)
        {
            BodyguardEngine::StopMovement(actor);
            state.summary.activity = "Holding position";
        }
        else
        {
            BodyguardEngine::PrepareFollow(actor, player, playerLocation);
            state.summary.activity = state.summary.distanceM > 5.0f && now - state.lastProgressMs > 4000
                ? "Follow stalled" : "Following";
        }
    }
    Save(actor, state);
}
bool Bodyguards::AllowsFollow(UObject* actor)
{
    State state;
    if (!Load(actor, state)) return true;
    return state.actor.Get() && state.summary.friendly && !state.engaged && state.summary.order != Order::Hold;
}
std::vector<Bodyguards::Summary> Bodyguards::Snapshot()
{
    std::vector<Summary> result;
    std::lock_guard<std::mutex> lock(stateMutex);
    for (const auto& entry : states) result.push_back(entry.second.summary);
    return result;
}


std::string Bodyguards::SnapshotJson()
{
    nlohmann::json result = nlohmann::json::array();
    for (const auto& row : Snapshot())
        result.push_back({ { "id", row.id }, { "name", row.name }, { "activity", row.activity },
            { "order", static_cast<int>(row.order) }, { "friendly", row.friendly },
            { "combat_capable", row.combatCapable }, { "distance_m", row.distanceM },
            { "protected_targets_cleared", row.protectedTargetsCleared } });
    return result.dump();
}
