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
#include <cstddef>

// Signature/AOB scanning over the main module. Patterns use IDA-style strings,
// e.g. "48 8B 05 ?? ?? ?? ?? 48 85 C0" where ?? is a wildcard byte.
namespace Scanner
{
    enum class Scope : uint8_t
    {
        ExecutableSections,
        WholeModule,
    };

    // Find the first match of `pattern`. Code signatures use the default
    // executable-only scope. WholeModule is reserved for explicitly data-aware
    // searches and must never be used for a detour target.
    uint8_t* Find(const char* pattern, Scope scope = Scope::ExecutableSections);

    // Find a UNIQUE match of `pattern`. Counts matches across the module (capped
    // at 2 for cost) and writes the count to *outCount when non-null. Returns the
    // address ONLY when there is exactly one match; returns nullptr for zero
    // matches OR for an ambiguous (>=2) pattern. This is the safe primitive for
    // native hooking: never detour a non-unique signature. The cap means
    // *outCount is 0, 1, or 2 (where 2 means "2 or more").
    uint8_t* FindUnique(const char* pattern, int* outCount = nullptr,
                        Scope scope = Scope::ExecutableSections);

    // Count matches of `pattern` across the module, capped at `cap` (so a unique
    // pattern returns 1, an ambiguous one returns `cap`). Cheap early-out scan.
    int CountMatches(const char* pattern, int cap = 2,
                     Scope scope = Scope::ExecutableSections);

    bool IsExecutableAddress(const void* address, size_t size = 1);
    uint8_t* DecodeRel32CallTarget(uint8_t* call);
    uint8_t* DecodeRel32JumpTarget(uint8_t* jump);

    // Find a match, then resolve a 32-bit RIP-relative reference located at
    // (match + offset). `instructionLength` is the full length of the
    // instruction that contains the rel32 (so we can compute the next-IP base).
    // Typical UE4 usage for `mov rax, [rip+disp]` (48 8B 05 xx xx xx xx) is
    // ResolveRipRel(match, 3, 7).
    uint8_t* ResolveRipRel(uint8_t* match, int offsetToRel32, int instructionLength);

    // Convenience: find pattern then resolve the rel32 in one call.
    uint8_t* FindRipRel(const char* pattern, int offsetToRel32, int instructionLength,
                        Scope scope = Scope::ExecutableSections);
}
