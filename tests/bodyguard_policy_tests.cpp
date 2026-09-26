// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Skorchekd. See LICENSE/NOTICE.
// Policy tests use a fake engine; live movement and native damage require in-game tests.
#include "../src/features/bodyguards.h"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <unordered_map>
using UE::UObject;
using UE::FVector;
struct Fake;
static std::unordered_map<UObject*, Fake*> objects;
static UObject* player = nullptr;
static int attacks = 0, clears = 0, stops = 0, follows = 0, checks = 0;
struct Fake
{
    alignas(16) std::array<unsigned char,64> memory{};
    bool alive = true, combat = true, friendly = true, hostile = true, passive = false;
    UObject* target = nullptr;
    UObject* blackboard = nullptr;
    UObject* sensed = nullptr;
    float health = 1.0f;
    FVector position{};
    UObject* ptr() { return reinterpret_cast<UObject*>(memory.data()); }
    Fake(int id, float x = 0)
    {
        auto* p = ptr(); objects[p] = this;
        *reinterpret_cast<int*>(memory.data()+Offsets::O_UObject_InternalIndex)=id;
        *reinterpret_cast<UObject**>(memory.data()+Offsets::O_UObject_Class)=p;
        *p->NamePtr() = {id,0}; position.X=x;
    }
    ~Fake() { Bodyguards::Forget(ptr()); objects.erase(ptr()); }
};
static void Check(bool ok, const char* what)
{
    ++checks;
    if (!ok) { std::fprintf(stderr,"FAILED: %s\n",what); std::exit(1); }
}
namespace UE
{
    bool IsLiveObject(UObject* p) { auto i=objects.find(p);return i!=objects.end()&&i->second->alive; }
    UObject* GetLocalPawn() { return player; }
    std::string UObject::GetName() { return "TestRobot"; }
}
namespace Log { void Write(const char*, ...) {} }
namespace BodyguardEngine
{
    bool Usable(UObject* a) { return UE::IsLiveObject(a); }
    bool CombatCapable(UObject* a) { return Usable(a)&&objects.at(a)->combat; }
    bool Friendly(UObject* a,UObject*) { return Usable(a)&&objects.at(a)->friendly; }
    bool Protected(UObject* a) { return a==player||Bodyguards::Contains(a); }
    bool LiveEnemy(UObject* a) { return Usable(a); }
    UObject* Target(UObject* a) { return Usable(a)?objects.at(a)->target:nullptr; }
    UObject* BlackboardTarget(UObject* a) { return Usable(a)?objects.at(a)->blackboard:nullptr; }
    UObject* Sensed(UObject* a) { return Usable(a)?objects.at(a)->sensed:nullptr; }
    bool HostileTo(UObject* a,UObject*) { return Usable(a)&&objects.at(a)->hostile; }
    bool Passive(UObject* a) { return Usable(a)&&objects.at(a)->passive; }
    bool Health(UObject* a,float& out) { if (!Usable(a))return false;out=objects.at(a)->health;return true; }
    bool Location(UObject* a,FVector& out) { if (!Usable(a))return false;out=objects.at(a)->position;return true; }
    void ClearCombat(UObject* a) { ++clears;objects.at(a)->target=nullptr;objects.at(a)->blackboard=nullptr; }
    bool Attack(UObject* a,UObject* enemy,UObject*) { ++attacks;objects.at(a)->target=enemy;return true; }
    void StopMovement(UObject*) { ++stops; }
    void PrepareFollow(UObject*,UObject*,const FVector&) { ++follows; }
}
int main()
{
    Fake human(1), guard(2,1000), other(3,500), enemy(4,700);
    player=human.ptr();
    Check(!Bodyguards::Adopt(player,player),"player cannot become own guard");
    Check(Bodyguards::Adopt(guard.ptr(),player),"guard admitted");
    Check(Bodyguards::Contains(guard.ptr()),"registered immediately");
    Check(Bodyguards::AllowsFollow(guard.ptr()),"new companion follows");
    Check(Bodyguards::Adopt(other.ptr(),player),"second guard admitted");
    Check(!Bodyguards::AttackTarget(guard.ptr(),player),"explicit player attack rejected");
    Check(!Bodyguards::AttackTarget(guard.ptr(),other.ptr()),"explicit ally attack rejected");
    int before=attacks;
    Bodyguards::Update(guard.ptr(),player,human.position,enemy.ptr());
    Check(attacks==before,"idle nearby robot is not attacked");
    enemy.target=player;
    Bodyguards::Update(guard.ptr(),player,human.position,enemy.ptr());
    Check(attacks==before+1,"attacker is engaged");
    Check(!Bodyguards::AllowsFollow(guard.ptr()),"follow does not interrupt combat");
    enemy.alive=false;
    Bodyguards::Update(guard.ptr(),player,human.position,enemy.ptr());
    Check(Bodyguards::AllowsFollow(guard.ptr()),"dead target releases follow");
    Check(guard.target==nullptr,"dead target cleared");
    enemy.alive=true;
    guard.target=player; int clearBefore=clears;
    Bodyguards::Update(guard.ptr(),player,human.position,nullptr);
    Check(clears>clearBefore&&guard.target==nullptr,"retaliation against player is cleared");
    guard.target=other.ptr();clearBefore=clears;
    Bodyguards::Update(guard.ptr(),player,human.position,nullptr);
    Check(clears>clearBefore&&guard.target==nullptr,"retaliation against ally is cleared");
    Bodyguards::SetOrder(guard.ptr(),Bodyguards::Order::Hold);before=attacks;
    Bodyguards::Update(guard.ptr(),player,human.position,enemy.ptr());
    Check(!Bodyguards::AllowsFollow(guard.ptr())&&attacks==before,"hold does not follow or initiate combat");
    guard.target=enemy.ptr();int stopBefore=stops;
    Bodyguards::Update(guard.ptr(),player,human.position,enemy.ptr());
    Check(guard.target==nullptr&&stops>stopBefore,"hold cancels a target reacquired by native AI");
    Bodyguards::SetOrder(guard.ptr(),Bodyguards::Order::FollowOnly);
    Bodyguards::Update(guard.ptr(),player,human.position,enemy.ptr());
    Check(attacks==before&&Bodyguards::AllowsFollow(guard.ptr()),"follow-only suppresses automatic defence");
    guard.target=enemy.ptr();
    Bodyguards::Update(guard.ptr(),player,human.position,enemy.ptr());
    Check(guard.target==nullptr,"follow-only clears autonomous native aggression");
    Check(Bodyguards::AttackTarget(guard.ptr(),enemy.ptr()),"explicit valid attack accepted");
    enemy.target=nullptr;
    Bodyguards::Update(guard.ptr(),player,human.position,nullptr);
    Check(attacks==before+1,"explicit attack is retained through automatic selection");
    enemy.position.X=10000;before=attacks;
    Bodyguards::Update(guard.ptr(),player,human.position,enemy.ptr());
    Check(attacks==before&&Bodyguards::AllowsFollow(guard.ptr()),"distant target cannot drag guard away");
    Bodyguards::SetOrder(guard.ptr(),Bodyguards::Order::Hold);
    ++guard.ptr()->NamePtr()->Number;
    Check(!Bodyguards::Contains(guard.ptr()),"recycled object identity rejected");
    Check(Bodyguards::Adopt(guard.ptr(),player)&&Bodyguards::AllowsFollow(guard.ptr()),"reused slot receives fresh defaults");
    {
        Fake unknown(5);unknown.friendly=false;
        Check(Bodyguards::Adopt(unknown.ptr(),player),"unconfirmed guard remains tracked");
        unknown.target=enemy.ptr();
        before=attacks;Bodyguards::Update(unknown.ptr(),player,human.position,enemy.ptr());
        Check(unknown.target==nullptr&&!Bodyguards::AttackTarget(unknown.ptr(),enemy.ptr()),"unconfirmed allegiance clears targets and rejects explicit attack");
        Check(!Bodyguards::AllowsFollow(unknown.ptr())&&attacks==before,"failed allegiance does not enable movement or combat");
    }
    {
        Fake civilian(6);civilian.combat=false;
        Bodyguards::Adopt(civilian.ptr(),player);enemy.target=player;enemy.position.X=700;
        before=attacks;Bodyguards::Update(civilian.ptr(),player,human.position,enemy.ptr());
        Check(attacks==before&&Bodyguards::AllowsFollow(civilian.ptr()),"noncombat companion follows without combat calls");
    }
    Bodyguards::Forget(guard.ptr());
    Check(!Bodyguards::Contains(guard.ptr()),"release removes ownership");

    // ---- proactive defence ------------------------------------------------
    {
        Fake defender(10,200), scout(11,900);
        Check(Bodyguards::Adopt(defender.ptr(),player),"defender admitted");
        before=attacks;
        Bodyguards::Update(defender.ptr(),player,human.position,scout.ptr());
        Check(attacks==before,"unaware robot nearby is left alone");
        scout.sensed=player;
        Bodyguards::Update(defender.ptr(),player,human.position,scout.ptr());
        Check(attacks==before+1&&defender.target==scout.ptr(),"hostile robot that spotted you is intercepted before it attacks");
        Bodyguards::Update(defender.ptr(),player,human.position,nullptr);
        Check(attacks==before+2,"an ongoing fight continues while the enemy stays in range");
        scout.alive=false;
        Bodyguards::Update(defender.ptr(),player,human.position,scout.ptr());
        Check(Bodyguards::AllowsFollow(defender.ptr())&&defender.target==nullptr,"fight ends when the enemy dies");
    }
    {
        Fake defender(12,200), worker(13,900);
        Bodyguards::Adopt(defender.ptr(),player);
        worker.sensed=player; worker.passive=true; before=attacks;
        Bodyguards::Update(defender.ptr(),player,human.position,worker.ptr());
        Check(attacks==before,"passive robots are never engaged automatically");
        worker.passive=false; worker.hostile=false;
        Bodyguards::Update(defender.ptr(),player,human.position,worker.ptr());
        Check(attacks==before,"robots that are not hostile to you are never engaged automatically");
        worker.hostile=true; worker.sensed=nullptr; worker.position.X=2500;
        Bodyguards::Update(defender.ptr(),player,human.position,worker.ptr());
        Check(attacks==before,"defend order does not hunt idle robots");
        Bodyguards::SetOrder(defender.ptr(),Bodyguards::Order::FollowAndAttack);
        Bodyguards::Update(defender.ptr(),player,human.position,worker.ptr());
        Check(attacks==before+1,"attack order hunts hostile robots within the defend radius");
        Bodyguards::Order order{};
        Check(Bodyguards::GetOrder(defender.ptr(),order)&&order==Bodyguards::Order::FollowAndAttack,"order is reported back");
        Check(Bodyguards::AttackTarget(defender.ptr(),worker.ptr())&&Bodyguards::GetOrder(defender.ptr(),order)&&
              order==Bodyguards::Order::FollowAndAttack,"explicit attack keeps an aggressive companion aggressive");
    }
    {
        Fake loyal(14,300);
        Bodyguards::Adopt(loyal.ptr(),player);
        loyal.blackboard=player; int clearBefore=clears;
        Bodyguards::Update(loyal.ptr(),player,human.position,nullptr);
        Check(clears>clearBefore&&loyal.blackboard==nullptr,"blackboard retaliation against the player is cleared");
        Fake mate(15,-300);
        Bodyguards::Adopt(mate.ptr(),player);
        loyal.blackboard=mate.ptr(); clearBefore=clears;
        Bodyguards::Update(loyal.ptr(),player,human.position,nullptr);
        Check(clears>clearBefore&&loyal.blackboard==nullptr,"blackboard retaliation against a companion is cleared");
    }
    {
        Fake a(20,100), b(21,-100), r1(22,500), r2(23,-800);
        Bodyguards::Adopt(a.ptr(),player); Bodyguards::Adopt(b.ptr(),player);
        r1.target=player; r2.target=player;
        std::vector<UObject*> squad{ a.ptr(), b.ptr() };
        std::vector<Bodyguards::Threat> threats{ { r1.ptr(), r1.position }, { r2.ptr(), r2.position } };
        Bodyguards::UpdateAll(squad,player,human.position,threats);
        Check(a.target&&b.target&&a.target!=b.target,"two attackers are split between two companions");
        Bodyguards::UpdateAll(squad,player,human.position,threats);
        Check(a.target&&b.target&&a.target!=b.target,"the split is stable on the next update");
    }
    {
        Fake c(30,0), onCompanion(31,600), onYou(32,1200);
        Bodyguards::Adopt(c.ptr(),player);
        onCompanion.target=c.ptr();
        Bodyguards::Update(c.ptr(),player,human.position,onCompanion.ptr());
        Check(c.target==onCompanion.ptr(),"an enemy attacking a companion is engaged");
        onYou.target=player;
        std::vector<UObject*> squad{ c.ptr() };
        std::vector<Bodyguards::Threat> threats{ { onCompanion.ptr(), onCompanion.position }, { onYou.ptr(), onYou.position } };
        Bodyguards::UpdateAll(squad,player,human.position,threats);
        Check(c.target==onYou.ptr(),"an enemy attacking you outranks the current fight");
        onYou.target=nullptr; onYou.blackboard=player; onYou.position.X=2800;
        c.target=nullptr;
        threats={ { onCompanion.ptr(), onCompanion.position }, { onYou.ptr(), onYou.position } };
        Bodyguards::UpdateAll(squad,player,human.position,threats);
        Check(c.target==onYou.ptr(),"an attacker known only to its blackboard is still answered");
        onYou.position.X=6000;
        threats={ { onCompanion.ptr(), onCompanion.position }, { onYou.ptr(), onYou.position } };
        Bodyguards::UpdateAll(squad,player,human.position,threats);
        Check(c.target==onCompanion.ptr(),"attackers beyond the defend radius do not pull the squad away");
    }
    std::printf("Passed %d bodyguard policy checks. Native engine behavior still requires live validation.\n",checks);
}
