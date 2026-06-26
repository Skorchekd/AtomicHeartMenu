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
#include <atomic>

namespace UE { struct UObject; }

// ===========================================================================
//  Native hook layer  --  signature-scanned engine functions + MinHook detours.
// ---------------------------------------------------------------------------
//  This is the "scan raw code bytes and patch the engine" half of the menu,
//  deliberately kept SEPARATE from the SDK reflection layer (sdk/ue4.cpp, which
//  walks GObjects). Everything here is resolved by AOB signature over the live
//  module and logged on the [HOOK] channel; the SDK layer logs on [SDK].
//
//  PRIME DIRECTIVE: the random AI crash.
//  We verified (Ghidra + the captured minidump) that the game thread faults at
//  AtomicHeart+0x1bc8ff4 -> `MOV R9,[RCX+0x738]` with RCX == 0, i.e. a helper
//  (FUN_141bc8ff0) dereferences a null `this`. It is reached from the engine's
//  AI context-target-ally tick (FUN_141b988e0): the menu's bodyguard feature sets
//  the AI's "target ally" to the PLAYER, the player is an AHBaseCharacter (NOT an
//  AHAICharacter), so the engine's IsA_AHAICharacter check zeroes it, stores null,
//  resolves null, and the un-null-checked helper crashes on it.
//
//  The fix is a MinHook detour on that helper that returns 0 when `this` is null
//  -- exactly the value every caller already handles ("no result"). One tiny,
//  always-safe compare neutralises the engine bug for ALL call paths at once.
//
//  Every resolve fails SAFE: a signature that is missing OR ambiguous (>=2 hits)
//  is never hooked directly. Derived targets must come from the unique callsite
//  chain, remain executable, and pass semantic byte checks. The required primary
//  firewall cannot be disabled from Hook Diagnostics.
// ===========================================================================
namespace NativeHooks
{
    enum class Kind : uint8_t
    {
        Detour,   // installed as a MinHook trampoline detour
        Resolve,  // address only -- a native function we may CALL (not patch)
    };

    enum class State : uint8_t
    {
        Pending,      // not yet scanned
        Unresolved,   // signature/callsite not found
        Ambiguous,    // signature matched >=2 sites -> refused (unsafe to hook)
        Resolved,     // address found, or a detour created+enabled
        Skipped,      // dependency unavailable; intentionally not attempted
        SanityFailed, // derived/direct target did not match required semantics
        InstallFailed,// found, but MinHook create/enable failed
    };

    enum class Resolver : uint8_t
    {
        None,
        DirectSignature,
        CallsiteDerived,
        ReVaConfirmed,
    };

    struct Entry
    {
        // static descriptor (filled at Init from the internal table)
        const char* name      = "";
        const char* sig       = "";
        const char* purpose   = "";
        Kind        kind      = Kind::Resolve;
        bool        required  = false;

        // runtime state
        uint8_t*    addr      = nullptr;   // resolved function address (null = none)
        int         matches   = 0;         // signature hit count (0,1, or 2==">=2")
        int         directMatches = 0;     // original standalone signature count
        State       state     = State::Pending;
        Resolver    resolver  = Resolver::None;
        bool        executableOnly = true;
        bool        sanityPassed = false;
        const char* detail    = "not scanned";
        bool        enabled   = true;      // optional detour gate; required guard stays on

        std::atomic<uint64_t> calls{ 0 };  // times the detour body ran
        std::atomic<uint64_t> guarded{ 0 };// times a null-`this` was caught & neutralised
        std::atomic<uint64_t> lastGuardMs{ 0 };
        std::atomic<uintptr_t> lastCaller{ 0 };
    };

    // Scan all signatures + create/enable the detours. Idempotent and safe to call
    // before the SDK resolves (pure module byte scan; MinHook is self-initialised).
    // Logs every step on the [HOOK] channel.
    void Init();

    // Re-run resolution + (re)install any detours that are not yet active. Used by
    // the "Rescan signatures" button. Never tears down a working detour.
    void Rescan();

    // Disable + remove every detour. Called on eject.
    void Shutdown();

    // True once the primary crash-guard detour is live (the menu can trust that the
    // null-target AI crash is neutralised).
    bool CrashGuardActive();
    const char* StateName(State state);
    const char* ResolverName(Resolver resolver);

    // ---- inspection (for the Hook Diagnostics tab) ------------------------
    int          Count();
    const Entry& Get(int i);
    void         SetEnabled(int i, bool on);   // toggle a detour's guard logic

    // ---- native validation helpers (safe-fail) ---------------------------
    // These CALL resolved engine functions when available, else return a safe
    // default. They let features validate exactly the way the engine does.

    // True if `obj` IS an AHAICharacter (the engine's own IsA used by the AI tick).
    // This is the test that, when it FAILS for the assigned target-ally, leads to
    // the crash -- so callers can pre-screen an ally before assigning it.
    bool IsAHAICharacter(UE::UObject* obj);

    // Read a controller's current context target-ally (controller+0x14a0 weak ptr,
    // resolved + cast) the same way the engine does. Returns null if unresolved /
    // invalid. Pure read; never assigns.
    UE::UObject* ContextTargetAlly(UE::UObject* controller);

    // Headline counter for the tab: total null-`this` hits the crash guard caught.
    uint64_t TotalGuarded();

    // Write a complete status snapshot to the [HOOK] log.
    void DumpStatus();
}
