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

    // Why a companion is fighting, strongest first in Score().
    enum class Reason { None, Hunt, Intercept, AttacksCompanion, Continue, AttacksPlayer, Explicit };

    struct State
    {
        Ref actor, enemy, player;
        Bodyguards::Summary summary;
        bool explicitAttack = false;
        bool engaged = false;
        Reason reason = Reason::None;
        ULONGLONG nextAllegianceCheck = 0;
        ULONGLONG lastProgressMs = 0;
        FVector lastPosition{};
    };
    std::mutex stateMutex;
    std::unordered_map<UObject*, State> states;

    std::mutex settingsMutex;
    Bodyguards::Settings settings;

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
    bool ProtectedTarget(UObject* target, UObject* player)
    {
        return target && (target == player || BodyguardEngine::Protected(target));
    }

    // Everything about one threat that does not depend on which companion asks.
    struct ThreatView
    {
        UObject* actor = nullptr;
        FVector location{};
        bool attacksPlayer = false;
        bool attacksCompanion = false;
        bool aware = false;   // has sensed you or a companion, or is attacking one
        bool passive = false;
        int hostile = -1;     // costs an engine call, so evaluated on first need
        int assigned = 0;     // companions already sent at it during this update
    };

    ThreatView View(UObject* threat, const FVector& location, UObject* player)
    {
        ThreatView view;
        view.actor = threat;
        view.location = location;
        auto companion = [player](UObject* target)
        { return target && target != player && BodyguardEngine::Protected(target); };
        UObject* target = BodyguardEngine::Target(threat);
        UObject* blackboard = BodyguardEngine::BlackboardTarget(threat);
        view.attacksPlayer = player && (target == player || blackboard == player);
        view.attacksCompanion = companion(target) || companion(blackboard);
        UObject* sensed = BodyguardEngine::Sensed(threat);
        view.aware = view.attacksPlayer || view.attacksCompanion || ProtectedTarget(sensed, player);
        view.passive = BodyguardEngine::Passive(threat);
        return view;
    }

    bool Hostile(ThreatView& view, UObject* player)
    {
        if (view.hostile < 0)
            view.hostile = BodyguardEngine::HostileTo(view.actor, player) ? 1 : 0;
        return view.hostile == 1;
    }

    Reason Qualify(const State& state, ThreatView& view, UObject* player, const FVector& playerLocation,
                   const FVector& selfLocation, const Bodyguards::Settings& s)
    {
        const float fromPlayer = Distance(view.location, playerLocation);
        const float fromSelf = Distance(view.location, selfLocation);
        const bool current = state.enemy.Get() == view.actor;
        // A direct order holds until the enemy dies or leaves the leash.
        if (current && state.explicitAttack)
            return fromPlayer <= s.leashRadiusM ? Reason::Explicit : Reason::None;
        const float nearest = (std::min)(fromPlayer, fromSelf);
        if (view.attacksPlayer && nearest <= s.defendRadiusM)
            return Reason::AttacksPlayer;
        if (view.attacksCompanion && nearest <= s.defendRadiusM)
            return Reason::AttacksCompanion;
        // Keep an ongoing fight through a stagger, a dodge or a lost line of sight,
        // instead of walking away and letting the enemy recover.
        if (current && state.engaged && fromPlayer <= s.leashRadiusM)
            return Reason::Continue;
        // Nothing below starts a fight the game did not: passive robots are left alone.
        if (view.passive)
            return Reason::None;
        if (state.summary.order == Bodyguards::Order::FollowAndAttack &&
            fromPlayer <= s.defendRadiusM && Hostile(view, player))
            return Reason::Hunt;
        // Defend before the hit lands: a hostile enemy that has spotted you (or a
        // companion) and is closing in is engaged first.
        if (view.aware && fromPlayer <= s.interceptRadiusM && Hostile(view, player))
            return Reason::Intercept;
        return Reason::None;
    }

    float Score(Reason reason, float fromPlayerM, int assigned, bool current)
    {
        float base = 0.0f;
        switch (reason)
        {
        case Reason::Explicit:         base = -6000.0f; break;
        case Reason::AttacksPlayer:    base = -5000.0f; break;
        case Reason::Continue:         base = -4000.0f; break;
        case Reason::AttacksCompanion: base = -3000.0f; break;
        case Reason::Intercept:        base = -2000.0f; break;
        case Reason::Hunt:             base = -1000.0f; break;
        default: return 3.4e38f;
        }
        // Distance to the player decides within a tier. A threat that already has a
        // companion on it costs 12 m, which spreads the squad across simultaneous
        // attackers, and a small bonus for the current target stops flip-flopping
        // between two enemies at almost the same range.
        return base + fromPlayerM + (float)assigned * 12.0f - (current ? 15.0f : 0.0f);
    }

    const char* ActivityFor(Reason reason)
    {
        switch (reason)
        {
        case Reason::Explicit:         return "Attacking (ordered)";
        case Reason::AttacksPlayer:    return "Defending you";
        case Reason::AttacksCompanion: return "Defending the squad";
        case Reason::Continue:         return "Fighting";
        case Reason::Intercept:        return "Intercepting";
        case Reason::Hunt:             return "Hunting";
        default:                       return "Following";
        }
    }

    std::string NameOf(UObject* actor)
    {
        try { return actor ? actor->GetName() : std::string(); }
        catch (...) { return std::string(); }
    }

    void UpdateOne(UObject* actor, UObject* player, const FVector& playerLocation,
                   std::vector<ThreatView>& views, const Bodyguards::Settings& s)
    {
        State state;
        if (!Load(actor, state)) return;
        if (!state.actor.Get()) { Bodyguards::Forget(actor); return; }
        if (!UE::IsLiveObject(player) || !BodyguardEngine::Usable(actor)) return;
        ULONGLONG now = GetTickCount64();

        // The hard guarantee comes first: whatever the companion's own AI picked --
        // retaliation for a stray shot, a scripted re-target -- it never keeps you or
        // another companion as its target. The blackboard key is checked as well as
        // the cached field, because native perception writes it without ProcessEvent.
        UObject* currentTarget = BodyguardEngine::Target(actor);
        if (ProtectedTarget(currentTarget, player) || ProtectedTarget(BodyguardEngine::BlackboardTarget(actor), player))
        {
            BodyguardEngine::ClearCombat(actor);
            ++state.summary.protectedTargetsCleared;
            state.engaged = false; state.enemy = {}; state.explicitAttack = false;
            state.reason = Reason::None;
            state.nextAllegianceCheck = 0;
            currentTarget = nullptr;
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
            state.reason = Reason::None;
            state.summary.target.clear();
            state.summary.activity = "Waiting for friendly team";
            Save(actor, state);
            return;
        }

        FVector location{};
        if (!BodyguardEngine::Location(actor, location)) { Save(actor, state); return; }
        state.summary.distanceM = Distance(location, playerLocation);
        float health = -1.0f;
        state.summary.healthFrac = BodyguardEngine::Health(actor, health) ? health : -1.0f;
        if (!state.lastProgressMs || Distance(location, state.lastPosition) >= 0.2f)
        { state.lastPosition = location; state.lastProgressMs = now; }

        // Choose what to fight, if anything.
        ThreatView* best = nullptr;
        Reason bestReason = Reason::None;
        float bestScore = 3.4e38f;
        ThreatView unlisted;
        const bool mayFight = state.summary.combatCapable &&
            (state.summary.order == Bodyguards::Order::FollowAndDefend ||
             state.summary.order == Bodyguards::Order::FollowAndAttack);
        if (mayFight)
        {
            auto consider = [&](ThreatView& view)
            {
                if (!Enemy(view.actor, actor, player)) return;
                Reason reason = Qualify(state, view, player, playerLocation, location, s);
                if (reason == Reason::None) return;
                float score = Score(reason, Distance(view.location, playerLocation), view.assigned,
                                    state.enemy.Get() == view.actor);
                if (score < bestScore) { bestScore = score; best = &view; bestReason = reason; }
            };
            for (ThreatView& view : views)
                consider(view);
            // The ordered or ongoing enemy may be missing from this update's list
            // (out of the scan radius, not cached yet); judge it anyway.
            if (UObject* enemy = state.enemy.Get())
            {
                bool listed = false;
                for (const ThreatView& view : views)
                    if (view.actor == enemy) { listed = true; break; }
                FVector enemyLocation{};
                if (!listed && BodyguardEngine::Location(enemy, enemyLocation))
                {
                    unlisted = View(enemy, enemyLocation, player);
                    consider(unlisted);
                }
            }
        }

        if (best)
        {
            UObject* target = best->actor;
            state.engaged = BodyguardEngine::Attack(actor, target, player);
            if (state.engaged)
            {
                ++best->assigned;
                if (bestReason != Reason::Explicit) state.explicitAttack = false;
                state.enemy = Ref(target);
                state.reason = bestReason;
                state.summary.target = NameOf(target);
            }
            // Attacking re-asserts the team; confirm the companion is still ours.
            state.summary.friendly = BodyguardEngine::Friendly(actor, player);
            if (!state.summary.friendly) { BodyguardEngine::ClearCombat(actor); state.engaged = false; }
            state.summary.activity = state.engaged && state.summary.friendly ? ActivityFor(bestReason) : "Attack unavailable";
        }
        else
        {
            if (state.engaged || state.enemy.Get() || currentTarget) BodyguardEngine::ClearCombat(actor);
            state.engaged = false; state.enemy = {}; state.explicitAttack = false;
            state.reason = Reason::None;
            state.summary.target.clear();
            if (state.summary.order == Bodyguards::Order::Hold)
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

    bool Engaged(UObject* actor)
    {
        std::lock_guard<std::mutex> lock(stateMutex);
        auto found = states.find(actor);
        return found != states.end() && found->second.engaged;
    }
}

void Bodyguards::Configure(const Settings& value)
{
    Settings clean = value;
    auto clamp = [](float v, float lo, float hi) { return std::isfinite(v) ? (std::min)(hi, (std::max)(lo, v)) : lo; };
    clean.defendRadiusM = clamp(clean.defendRadiusM, 5.0f, 100.0f);
    clean.interceptRadiusM = clamp(clean.interceptRadiusM, 0.0f, clean.defendRadiusM);
    clean.leashRadiusM = clamp(clean.leashRadiusM, clean.defendRadiusM, 150.0f);
    std::lock_guard<std::mutex> lock(settingsMutex);
    settings = clean;
}
Bodyguards::Settings Bodyguards::CurrentSettings()
{
    std::lock_guard<std::mutex> lock(settingsMutex);
    return settings;
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
    state.reason = Reason::None;
    state.summary.target.clear();
    BodyguardEngine::ClearCombat(actor);
    BodyguardEngine::StopMovement(actor);
    state.summary.activity = order == Order::Hold ? "Holding position" : "Following";
    Save(actor, state);
}
bool Bodyguards::GetOrder(UObject* actor, Order& out)
{
    State state;
    if (!Load(actor, state) || !state.actor.Get()) return false;
    out = state.summary.order;
    return true;
}
bool Bodyguards::AttackTarget(UObject* actor, UObject* target)
{
    State state;
    if (!Load(actor, state) || !state.actor.Get() || !state.summary.combatCapable ||
        !state.summary.friendly || !Enemy(target, actor, UE::GetLocalPawn())) return false;
    state.enemy = Ref(target); state.explicitAttack = true;
    // An order to attack takes a passive companion out of Follow only / Hold, but
    // leaves an aggressive one aggressive.
    if (state.summary.order != Order::FollowAndAttack)
        state.summary.order = Order::FollowAndDefend;
    Save(actor, state);
    return true;
}
void Bodyguards::Update(UObject* actor, UObject* player, const UE::FVector& playerLocation,
                        UObject* candidate)
{
    std::vector<ThreatView> views;
    FVector location{};
    if (candidate && candidate != player && BodyguardEngine::LiveEnemy(candidate) &&
        !BodyguardEngine::Protected(candidate) && BodyguardEngine::Location(candidate, location))
        views.push_back(View(candidate, location, player));
    UpdateOne(actor, player, playerLocation, views, CurrentSettings());
}
void Bodyguards::UpdateAll(const std::vector<UObject*>& members, UObject* player,
                           const UE::FVector& playerLocation, const std::vector<Threat>& threats)
{
    const Settings s = CurrentSettings();
    const float reach = (std::max)((std::max)(s.defendRadiusM, s.interceptRadiusM), s.leashRadiusM) + 10.0f;
    std::vector<ThreatView> views;
    views.reserve(threats.size());
    for (const Threat& threat : threats)
    {
        if (!threat.actor || threat.actor == player || Distance(threat.location, playerLocation) > reach ||
            !BodyguardEngine::LiveEnemy(threat.actor) || BodyguardEngine::Protected(threat.actor))
            continue;
        views.push_back(View(threat.actor, threat.location, player));
    }
    // Companions already in a fight keep their claim before the rest are spread
    // over whatever is left.
    std::vector<UObject*> ordered;
    ordered.reserve(members.size());
    for (UObject* member : members) if (Engaged(member)) ordered.push_back(member);
    for (UObject* member : members) if (!Engaged(member)) ordered.push_back(member);
    for (UObject* member : ordered)
        UpdateOne(member, player, playerLocation, views, s);
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
    std::sort(result.begin(), result.end(), [](const Summary& a, const Summary& b) { return a.id < b.id; });
    return result;
}


std::string Bodyguards::SnapshotJson()
{
    nlohmann::json result = nlohmann::json::array();
    for (const auto& row : Snapshot())
        result.push_back({ { "id", row.id }, { "name", row.name }, { "activity", row.activity },
            { "order", static_cast<int>(row.order) }, { "friendly", row.friendly },
            { "combat_capable", row.combatCapable }, { "distance_m", row.distanceM },
            { "health", row.healthFrac }, { "target", row.target },
            { "protected_targets_cleared", row.protectedTargetsCleared } });
    return result.dump();
}
