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
#include "native_hooks.h"
#include "../core/globals.h"
#include "../core/log.h"
#include "../core/memory.h"
#include "../sdk/scanner.h"
#include <Windows.h>
#include <MinHook.h>
#include <mutex>
#include <cstdio>
#include <cstring>
#include <intrin.h>
#include <initializer_list>

namespace NativeHooks
{
namespace
{
    // ---- fixed entry indices ----------------------------------------------
    enum
    {
        H_PRIMARY = 0,  // FUN_141bc8ff0  null-this guard ([this+0x738])  <-- the crash
        H_SIBLING,      // FUN_141d097b0  null-this guard ([this+0x318])  (sibling cast)
        H_ISA,          // FUN_141bcbc70  IsA_AHAICharacter               (call target)
        H_GETALLY,      // FUN_141cf8720  GetContextTargetAlly            (call target)
        H_CALLSITE,     // FUN_141b988e0  hot callsite that makes the bad call (locator)
        H_COUNT
    };

    // The null-page ceiling. No real UObject lives in the first 64 KiB (it is the
    // reserved null region), so `this < kNullPage` is an exact, branch-cheap test
    // for the exact fault we proved (RCX==0 reading 0x738) plus any tiny garbage.
    constexpr uintptr_t kNullPage = 0x10000;

    Entry g_entries[H_COUNT];

    std::mutex        g_mtx;
    std::atomic<bool> g_built{ false };

    // ---- detour trampolines + call targets --------------------------------
    using GuardFn = void* (__fastcall*)(void* self);
    using IsAFn   = bool  (__fastcall*)(void* obj);
    using AllyFn  = void* (__fastcall*)(void* ctrl);

    GuardFn g_oPrimary  = nullptr;  // original FUN_141bc8ff0
    GuardFn g_oSibling  = nullptr;  // original FUN_141d097b0
    IsAFn   g_fnIsA     = nullptr;  // resolved FUN_141bcbc70 (called, not patched)
    AllyFn  g_fnGetAlly = nullptr;  // resolved FUN_141cf8720 (called, not patched)

    // ---- the detours -------------------------------------------------------
    // Both engine helpers share the same shape: read `[this + off]` and validate
    // the result in GObjects -- but neither null-checks `this` first. We add the
    // missing check. Returning 0 is exactly the helper's own "no result" path,
    // which every caller already branches on (`if (result != 0) ...`), so this is
    // behaviour-preserving for every valid object and merely refuses to fault on a
    // null one. The required primary firewall is permanently gated on while live.
    void* __fastcall hkPrimary(void* self)
    {
        Entry& e = g_entries[H_PRIMARY];
        e.calls.fetch_add(1, std::memory_order_relaxed);
        e.lastCaller.store(reinterpret_cast<uintptr_t>(_ReturnAddress()), std::memory_order_relaxed);
        if (e.enabled && reinterpret_cast<uintptr_t>(self) < kNullPage)
        {
            e.guarded.fetch_add(1, std::memory_order_relaxed);
            e.lastGuardMs.store(GetTickCount64(), std::memory_order_relaxed);
            return nullptr;
        }
        return g_oPrimary(self);
    }

    void* __fastcall hkSibling(void* self)
    {
        Entry& e = g_entries[H_SIBLING];
        e.calls.fetch_add(1, std::memory_order_relaxed);
        e.lastCaller.store(reinterpret_cast<uintptr_t>(_ReturnAddress()), std::memory_order_relaxed);
        if (e.enabled && reinterpret_cast<uintptr_t>(self) < kNullPage)
        {
            e.guarded.fetch_add(1, std::memory_order_relaxed);
            e.lastGuardMs.store(GetTickCount64(), std::memory_order_relaxed);
            return nullptr;
        }
        return g_oSibling(self);
    }

    // ---- descriptor table --------------------------------------------------
    struct Desc
    {
        const char* name;
        const char* sig;
        const char* purpose;
        Kind kind;
        bool required;
    };

    const Desc kDesc[H_COUNT] =
    {
        { "CrashGuard.ContextAllyComp (FUN_141bc8ff0)",
          // sub rsp,28; mov r9,[rcx+0x738]; mov r10,rcx; test r9,r9; jz ...
          "48 83 EC 28 4C 8B 89 38 07 00 00 4C 8B D1 4D 85 C9 0F 84",
          "Null-this guard for the helper that reads [this+0x738]; THE faulting "
          "function in the captured crash. Returns 0 on null this.",
          Kind::Detour, true },

        { "CrashGuard.AllyCastBase (FUN_141d097b0)",
          // identical shape, [rcx+0x318]
          "48 83 EC 28 4C 8B 89 18 03 00 00 4C 8B D1 4D 85 C9 0F 84",
          "Null-this guard for the sibling cast helper that reads [this+0x318]; same "
          "un-checked pattern, hooked for defence in depth.",
          Kind::Detour, false },

        { "IsA_AHAICharacter (FUN_141bcbc70)",
          // push rbx; sub rsp,20; mov rbx,rcx; call StaticClass; mov rdx,[rbx+10]; lea r8,[rax+38]; movsxd
          "40 53 48 83 EC 20 48 8B D9 E8 ?? ?? ?? ?? 48 8B 53 10 4C 8D 40 38 48 63",
          "Engine IsA() against AHAICharacter::StaticClass. Called to pre-screen a "
          "target-ally exactly the way the AI tick does (a failing check is what "
          "poisons the blackboard and leads to the crash).",
          Kind::Resolve, false },

        { "GetContextTargetAlly (FUN_141cf8720)",
          // sub rsp,28; lea r9,[rcx+0x14a0]; mov rcx,r9; call IsValid; test al,al; jz
          "48 83 EC 28 4C 8D 89 A0 14 00 00 49 8B C9 E8 ?? ?? ?? ?? 84 C0 74 14",
          "Reads controller+0x14a0 (the context target-ally weak ptr), resolves + "
          "casts it. Used read-only to show the live ally in the tab.",
          Kind::Resolve, false },

        { "AIContextTick.callsite (in FUN_141b988e0)",
          // lea rcx,[rsi+0x800]; call; lea rcx,[rsi+0x800]; call; mov rcx,rax; call PRIMARY; mov r8,rax; test rax,rax; jz
          "48 8D 8E 00 08 00 00 E8 ?? ?? ?? ?? 48 8D 8E 00 08 00 00 E8 ?? ?? ?? ?? "
          "48 8B C8 E8 ?? ?? ?? ?? 4C 8B C0 48 85 C0 74 55",
          "The exact callsite that makes the bad call into the crash helper. Resolved "
          "only (not patched) to locate the crash origin and self-verify the chain.",
          Kind::Resolve, true },
    };

    void BuildTableOnce()
    {
        if (g_built.exchange(true))
            return;
        for (int i = 0; i < H_COUNT; ++i)
        {
            g_entries[i].name    = kDesc[i].name;
            g_entries[i].sig     = kDesc[i].sig;
            g_entries[i].purpose = kDesc[i].purpose;
            g_entries[i].kind    = kDesc[i].kind;
            g_entries[i].required = kDesc[i].required;
            g_entries[i].enabled = true;
            g_entries[i].state   = State::Pending;
            g_entries[i].resolver = Resolver::None;
            g_entries[i].executableOnly = true;
            g_entries[i].sanityPassed = false;
            g_entries[i].detail = "not scanned";
        }
    }

    bool BytesEqual(const uint8_t* p, const uint8_t* expected, size_t n)
    {
        return Scanner::IsExecutableAddress(p, n) && std::memcmp(p, expected, n) == 0;
    }

    bool SanityCheckTarget(int idx, uint8_t* target)
    {
        if (!Scanner::IsExecutableAddress(target, 16))
            return false;
        static const uint8_t kPrimary[] =
            { 0x48,0x83,0xEC,0x28,0x4C,0x8B,0x89,0x38,0x07,0x00,0x00,0x4C,0x8B,0xD1,0x4D,0x85,0xC9 };
        static const uint8_t kSibling[] =
            { 0x48,0x83,0xEC,0x28,0x4C,0x8B,0x89,0x18,0x03,0x00,0x00,0x4C,0x8B,0xD1,0x4D,0x85,0xC9 };
        static const uint8_t kIsA[] =
            { 0x40,0x53,0x48,0x83,0xEC,0x20,0x48,0x8B,0xD9,0xE8 };
        static const uint8_t kGetAlly[] =
            { 0x48,0x83,0xEC,0x28,0x4C,0x8D,0x89,0xA0,0x14,0x00,0x00,0x49,0x8B,0xC9,0xE8 };
        static const uint8_t kCallsite[] =
            { 0x48,0x8D,0x8E,0x00,0x08,0x00,0x00,0xE8 };
        switch (idx)
        {
            case H_PRIMARY:  return BytesEqual(target, kPrimary, sizeof(kPrimary));
            case H_SIBLING:  return BytesEqual(target, kSibling, sizeof(kSibling));
            case H_ISA:      return BytesEqual(target, kIsA, sizeof(kIsA));
            case H_GETALLY:  return BytesEqual(target, kGetAlly, sizeof(kGetAlly)) &&
                                    Scanner::IsExecutableAddress(target + 0x26, 5) && target[0x26] == 0xE9;
            case H_CALLSITE:
                return Scanner::IsExecutableAddress(target - 0x1F, 1) &&
                       Scanner::IsExecutableAddress(target - 0x0F, 1) &&
                       Scanner::IsExecutableAddress(target + 27, 1) &&
                       BytesEqual(target, kCallsite, sizeof(kCallsite)) &&
                       target[-0x1F] == 0xE8 && target[-0x0F] == 0xE8 && target[27] == 0xE8;
        }
        return false;
    }

    uintptr_t Rva(void* p)
    {
        return (p && G::moduleBase) ? (uintptr_t)p - (uintptr_t)G::moduleBase : 0;
    }

    // Create + enable the guard detour for H_PRIMARY/H_SIBLING at a known-good
    // address (shared by the sig path and the callsite-derived path). On failure
    // marks the entry InstallFailed and returns false (fail-safe).
    bool InstallGuardDetour(int idx, uint8_t* target)
    {
        Entry& e = g_entries[idx];
        if (!e.sanityPassed || !Scanner::IsExecutableAddress(target, 16))
        {
            e.state = State::SanityFailed;
            e.detail = "target is outside executable sections or failed semantic byte checks";
            LOG_HOOK("hook refused '%s' -> failed sanity check.", e.name);
            return false;
        }
        void*  detour = (idx == H_PRIMARY) ? (void*)&hkPrimary : (void*)&hkSibling;
        void** tramp  = (idx == H_PRIMARY) ? reinterpret_cast<void**>(&g_oPrimary)
                                           : reinterpret_cast<void**>(&g_oSibling);
        MH_STATUS c = MH_CreateHook(target, detour, tramp);
        if (c != MH_OK && c != MH_ERROR_ALREADY_CREATED)
        {
            e.state = State::InstallFailed;
            LOG_HOOK("install '%s' -> MH_CreateHook FAILED (%d). Failing safe (no detour).", e.name, c);
            return false;
        }
        MH_STATUS en = MH_EnableHook(target);
        if (en != MH_OK && en != MH_ERROR_ENABLED)
        {
            e.state = State::InstallFailed;
            LOG_HOOK("install '%s' -> MH_EnableHook FAILED (%d). Failing safe (no detour).", e.name, en);
            return false;
        }
        e.addr  = target;
        e.state = State::Resolved;
        return true;
    }

    // Resolve one entry's signature; for Detour kind, also create+enable the hook.
    // Pure: only ever moves an entry FORWARD (never tears a live detour down).
    void ResolveEntry(int i)
    {
        Entry& e = g_entries[i];
        if (e.state == State::Resolved && e.kind == Kind::Resolve)
            return; // already located a call target
        if (e.state == State::Resolved && e.kind == Kind::Detour)
            return; // detour already live

        int count = 0;
        uint8_t* hit = Scanner::FindUnique(e.sig, &count, Scanner::Scope::ExecutableSections);
        e.matches = e.directMatches = count;
        e.resolver = Resolver::DirectSignature;

        if (count == 0)
        {
            e.state = State::Unresolved;
            e.detail = "direct executable-section signature not found";
            LOG_HOOK("resolve '%s' -> NOT FOUND in executable sections. Failing safe.", e.name);
            return;
        }
        if (count >= 2)
        {
            e.state = State::Ambiguous;
            e.addr  = nullptr;
            e.detail = "standalone prologue ambiguous in executable sections; awaiting derived resolver";
            LOG_HOOK("resolve '%s' -> AMBIGUOUS (>=2 executable matches). REFUSED direct hook.", e.name);
            return;
        }

        e.sanityPassed = SanityCheckTarget(i, hit);
        if (!e.sanityPassed)
        {
            e.addr = nullptr;
            e.state = State::SanityFailed;
            e.detail = "unique direct match failed semantic byte checks";
            LOG_HOOK("resolve '%s' -> unique direct match FAILED sanity check; refused.", e.name);
            return;
        }
        e.addr = hit;
        e.detail = "unique executable-section signature; sanity check passed";
        LOG_HOOK("resolve '%s' via direct signature -> %p (RVA 0x%llX), sanity PASS.", e.name, (void*)hit, (unsigned long long)Rva(hit));

        if (e.kind == Kind::Resolve)
        {
            // Wire the matching call target.
            if (i == H_ISA)     g_fnIsA     = reinterpret_cast<IsAFn>(hit);
            if (i == H_GETALLY) g_fnGetAlly = reinterpret_cast<AllyFn>(hit);
            e.state = State::Resolved;
            return;
        }

        // Detour kind: create + enable at the unique sig hit.
        if (InstallGuardDetour(i, hit))
            LOG_HOOK("install '%s' -> DETOUR LIVE @ %p (guard %s).",
                     e.name, (void*)hit, e.enabled ? "ON" : "OFF");
    }

    void ProbeStandaloneSignature(int i)
    {
        Entry& e = g_entries[i];
        if (e.state == State::Resolved)
            return;
        int count = Scanner::CountMatches(e.sig, 2, Scanner::Scope::ExecutableSections);
        e.matches = e.directMatches = count;
        e.resolver = Resolver::None;
        e.sanityPassed = false;
        if (count == 0)
        {
            e.state = State::Unresolved;
            e.detail = "standalone executable-section signature missing; derived resolver required";
        }
        else if (count >= 2)
        {
            e.state = State::Ambiguous;
            e.detail = "standalone executable-section signature ambiguous; direct hook refused";
            LOG_HOOK("resolve '%s' -> standalone signature AMBIGUOUS; direct hook refused, trying verified chain.", e.name);
        }
        else
        {
            // Even a currently unique prologue is less specific than the confirmed
            // AIContextTick relationship. Record it for diagnostics, but do not hook
            // until the callsite-derived target independently passes sanity checks.
            e.state = State::Pending;
            e.detail = "standalone signature unique but intentionally not trusted; awaiting derived resolver";
        }
    }

    bool AcceptDerived(int idx, uint8_t* target, Resolver resolver, const char* detail)
    {
        Entry& e = g_entries[idx];
        if (e.state == State::Resolved)
            return true;
        e.matches = 1;
        e.resolver = resolver;
        e.sanityPassed = SanityCheckTarget(idx, target);
        if (!e.sanityPassed)
        {
            e.addr = nullptr;
            e.state = State::SanityFailed;
            e.detail = "derived executable target failed semantic byte checks";
            LOG_HOOK("resolve '%s' -> derived target FAILED sanity check; refused.", e.name);
            return false;
        }
        e.addr = target;
        e.detail = detail;
        LOG_HOOK("resolve '%s' via %s -> %p (RVA 0x%llX), sanity PASS.",
                 e.name, resolver == Resolver::ReVaConfirmed ? "ReVa-confirmed target" : "callsite-derived target",
                 (void*)target, (unsigned long long)Rva(target));
        if (e.kind == Kind::Resolve)
        {
            if (idx == H_ISA) g_fnIsA = reinterpret_cast<IsAFn>(target);
            if (idx == H_GETALLY) g_fnGetAlly = reinterpret_cast<AllyFn>(target);
            e.state = State::Resolved;
            return true;
        }
        if (!InstallGuardDetour(idx, target))
            return false;
        LOG_HOOK("install '%s' (derived) -> DETOUR LIVE @ %p (guard ON).", e.name, (void*)target);
        return true;
    }

    // The unique AI-context block is the trust anchor. ReVa confirms its nearby
    // calls target GetContextTargetAlly (-0x1F), IsA_AHAICharacter (-0x0F), and
    // the primary crash helper (+27). GetContextTargetAlly then tail-jumps at +0x26
    // to AllyCastBase. Each edge is decoded and each destination sanity checked.
    void ResolveDerivedChain()
    {
        Entry& cs = g_entries[H_CALLSITE];
        if (cs.state != State::Resolved || !cs.addr)
        {
            for (int idx : { H_PRIMARY, H_SIBLING, H_ISA, H_GETALLY })
            {
                Entry& e = g_entries[idx];
                if (e.state != State::Resolved)
                {
                    e.state = State::Skipped;
                    e.detail = "unique AIContextTick callsite unavailable; derived resolver skipped";
                }
            }
            LOG_HOOK("derived resolver skipped: unique AIContextTick callsite unavailable.");
            return;
        }

        uint8_t* getAlly = Scanner::DecodeRel32CallTarget(cs.addr - 0x1F);
        uint8_t* isA = Scanner::DecodeRel32CallTarget(cs.addr - 0x0F);
        uint8_t* primary = Scanner::DecodeRel32CallTarget(cs.addr + 27);
        AcceptDerived(H_GETALLY, getAlly, Resolver::CallsiteDerived,
                      "unique AIContextTick call at RVA 0x1B9906B; ReVa semantics confirmed");
        AcceptDerived(H_ISA, isA, Resolver::CallsiteDerived,
                      "unique AIContextTick call at RVA 0x1B9907B; ReVa semantics confirmed");
        AcceptDerived(H_PRIMARY, primary, Resolver::CallsiteDerived,
                      "unique AIContextTick crash call; required firewall active");

        if (g_entries[H_GETALLY].state == State::Resolved && g_entries[H_GETALLY].addr)
        {
            uint8_t* sibling = Scanner::DecodeRel32JumpTarget(g_entries[H_GETALLY].addr + 0x26);
            AcceptDerived(H_SIBLING, sibling, Resolver::ReVaConfirmed,
                          "ReVa-confirmed tail jump from GetContextTargetAlly +0x26");
        }
    }

    // After resolution, prove the chain: the callsite's third CALL must target the
    // primary crash helper. This is a free integrity check that the sigs are
    // internally consistent (and that we hooked the right function).
    void CrossCheckChain()
    {
        Entry& cs = g_entries[H_CALLSITE];
        Entry& pr = g_entries[H_PRIMARY];
        if (cs.state != State::Resolved || !cs.addr)
            return;
        // callsite layout: the `E8 rel32` that calls PRIMARY starts at +27.
        uint8_t* call = cs.addr + 27;
        if (*call != 0xE8)
        {
            LOG_HOOK("cross-check: callsite+27 is not a CALL (0x%02X); chain layout differs from the capture.", *call);
            return;
        }
        uint8_t* target = Scanner::DecodeRel32CallTarget(call);
        LOG_HOOK("cross-check: callsite call-target = %p (RVA 0x%llX); primary helper = %p.",
                 (void*)target, (unsigned long long)Rva(target), (void*)pr.addr);
        if (pr.addr && target == pr.addr)
            LOG_HOOK("cross-check: PASS -- the callsite calls exactly the hooked crash helper.");
        else if (pr.addr)
            LOG_HOOK("cross-check: MISMATCH -- callsite target != primary sig hit. Investigate sigs.");
    }

    bool EnsureMinHook()
    {
        MH_STATUS s = MH_Initialize();
        if (s != MH_OK && s != MH_ERROR_ALREADY_INITIALIZED)
        {
            LOG_HOOK("MH_Initialize failed (%d) -- detours unavailable, falling back to SDK only.", s);
            return false;
        }
        return true;
    }
}

void Init()
{
    std::lock_guard<std::mutex> lk(g_mtx);
    BuildTableOnce();

    LOG_HOOK("==== native hook scan begin (separate from [SDK] reflection) ====");
    if (!EnsureMinHook())
    {
        // Still resolve addresses for the read-only call targets / locators.
        ResolveEntry(H_CALLSITE);
        ResolveEntry(H_ISA);
        ResolveEntry(H_GETALLY);
        CrossCheckChain();
        return;
    }

    // Resolve the unique trust-anchor first, then collect direct-signature results
    // for every target. Ambiguous/missing entries are subsequently replaced only
    // by the verified callsite/tail-jump chain.
    ResolveEntry(H_CALLSITE);
    for (int i = H_PRIMARY; i <= H_GETALLY; ++i)
        ProbeStandaloneSignature(i);
    ResolveDerivedChain();
    CrossCheckChain();

    int live = 0, resolved = 0;
    for (int i = 0; i < H_COUNT; ++i)
    {
        if (g_entries[i].state == State::Resolved)
        {
            ++resolved;
            if (g_entries[i].kind == Kind::Detour) ++live;
        }
    }
    LOG_HOOK("==== native hook scan done: %d/%d resolved, %d detour(s) live. Crash guard %s. ====",
             resolved, (int)H_COUNT, live,
             CrashGuardActive() ? "ACTIVE" : "NOT ACTIVE");
    if (CrashGuardActive())
        LOG_HOOK("crash guard active: required ContextAllyComp firewall is live; optional helper availability does not affect it.");
}

void Rescan()
{
    std::lock_guard<std::mutex> lk(g_mtx);
    BuildTableOnce();
    LOG_HOOK("rescan requested.");
    if (!EnsureMinHook())
        return;
    if (g_entries[H_CALLSITE].state != State::Resolved)
        ResolveEntry(H_CALLSITE);
    for (int i = H_PRIMARY; i <= H_GETALLY; ++i)
        ProbeStandaloneSignature(i);
    ResolveDerivedChain();
    CrossCheckChain();
    LOG_HOOK("rescan complete; crash guard %s.", CrashGuardActive() ? "ACTIVE" : "INACTIVE");
}

void Shutdown()
{
    std::lock_guard<std::mutex> lk(g_mtx);
    for (int i = 0; i < H_COUNT; ++i)
    {
        Entry& e = g_entries[i];
        if (e.kind == Kind::Detour && e.addr && e.state == State::Resolved)
        {
            MH_DisableHook(e.addr);
            MH_RemoveHook(e.addr);
        }
    }
    LOG_HOOK("native detours removed.");
}

bool CrashGuardActive()
{
    const Entry& e = g_entries[H_PRIMARY];
    return e.state == State::Resolved && e.kind == Kind::Detour && e.enabled;
}

const char* StateName(State state)
{
    switch (state)
    {
        case State::Pending:       return "pending";
        case State::Unresolved:    return "missing";
        case State::Ambiguous:     return "ambiguous";
        case State::Resolved:      return "resolved";
        case State::Skipped:       return "skipped";
        case State::SanityFailed:  return "failed sanity check";
        case State::InstallFailed: return "install failed";
    }
    return "unknown";
}

const char* ResolverName(Resolver resolver)
{
    switch (resolver)
    {
        case Resolver::None:             return "none";
        case Resolver::DirectSignature:  return "direct signature";
        case Resolver::CallsiteDerived:  return "callsite-derived";
        case Resolver::ReVaConfirmed:    return "ReVa-confirmed";
    }
    return "unknown";
}

int Count() { return H_COUNT; }

const Entry& Get(int i)
{
    if (i < 0) i = 0;
    if (i >= H_COUNT) i = H_COUNT - 1;
    return g_entries[i];
}

void SetEnabled(int i, bool on)
{
    if (i < 0 || i >= H_COUNT) return;
    if (g_entries[i].kind != Kind::Detour) return;
    if (g_entries[i].required && !on)
    {
        LOG_HOOK("guard '%s' -> disable REFUSED (required crash firewall).", g_entries[i].name);
        return;
    }
    g_entries[i].enabled = on;
    LOG_HOOK("guard '%s' -> %s", g_entries[i].name, on ? "ON (null-safe)" : "OFF (passthrough/repro)");
}

bool IsAHAICharacter(UE::UObject* obj)
{
    IsAFn fn = g_fnIsA;
    if (!fn || !Mem::IsReadable(obj, 0x18))
        return false;
    // The native fn dereferences obj+0x10 (ClassPrivate); make sure it is there.
    if (!Mem::IsReadable(reinterpret_cast<uint8_t*>(obj) + 0x10, sizeof(void*)))
        return false;
    try { return fn(reinterpret_cast<void*>(obj)); }
    catch (...) { return false; }
}

UE::UObject* ContextTargetAlly(UE::UObject* controller)
{
    AllyFn fn = g_fnGetAlly;
    // It reads controller+0x14a0; require that span to be present before calling.
    if (!fn || !Mem::IsReadable(controller, 0x14a8))
        return nullptr;
    try { return reinterpret_cast<UE::UObject*>(fn(reinterpret_cast<void*>(controller))); }
    catch (...) { return nullptr; }
}

uint64_t TotalGuarded()
{
    return g_entries[H_PRIMARY].guarded.load(std::memory_order_relaxed)
         + g_entries[H_SIBLING].guarded.load(std::memory_order_relaxed);
}

void DumpStatus()
{
    LOG_HOOK("==== hook diagnostic dump ====");
    for (int i = 0; i < H_COUNT; ++i)
    {
        const Entry& e = g_entries[i];
        LOG_HOOK("%s | %s | state=%s | resolver=%s | RVA=0x%llX | matches=%d direct=%d | exec-only=%d sanity=%d enabled=%d calls=%llu guarded=%llu lastGuardMs=%llu caller=%p | %s",
                 e.name, e.required ? "required" : "optional", StateName(e.state), ResolverName(e.resolver),
                 (unsigned long long)Rva(e.addr), e.matches, e.directMatches,
                 e.executableOnly ? 1 : 0, e.sanityPassed ? 1 : 0, e.enabled ? 1 : 0,
                 (unsigned long long)e.calls.load(std::memory_order_relaxed),
                 (unsigned long long)e.guarded.load(std::memory_order_relaxed),
                 (unsigned long long)e.lastGuardMs.load(std::memory_order_relaxed),
                 (void*)e.lastCaller.load(std::memory_order_relaxed), e.detail ? e.detail : "");
    }
    LOG_HOOK("required crash firewall: %s", CrashGuardActive() ? "ACTIVE" : "INACTIVE");
}

} // namespace NativeHooks
