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
    bool alive = true, combat = true, friendly = true;
    UObject* target = nullptr;
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
    bool Location(UObject* a,FVector& out) { if (!Usable(a))return false;out=objects.at(a)->position;return true; }
    void ClearCombat(UObject* a) { ++clears;objects.at(a)->target=nullptr; }
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
    std::printf("Passed %d bodyguard policy checks. Native engine behavior still requires live validation.\n",checks);
}
