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
#include <ostream>
#include "ue4.h"

// ===========================================================================
//  Generic UE4 reflection dumper (FProperty walker).
// ---------------------------------------------------------------------------
//  Walks an object's class chain + ChildProperties (FField list) and writes a
//  JSON object of EVERY named field with its decoded value -- the universal
//  "discover what we don't know" capability for the diagnostics logger. It is
//  the right tool for reverse-engineering an arbitrary actor (e.g. LarisaCharacter,
//  her controller, her movement/anim components, BP_ReasignTargetOnFollow_C):
//  no hard-coded offsets, just the engine's own reflection data.
//
//  SAFETY: every read is Mem::IsReadable-guarded and bounded (prop count + struct
//  recursion depth + array sample). It performs ONLY raw reads -- never calls
//  ProcessEvent -- so it is thread-safe to run on a background/worker thread
//  (exactly like the GObjects name-index builder), and therefore CANNOT freeze
//  the game thread.
// ===========================================================================
namespace Reflect
{
    struct Options
    {
        int maxProps    = 600; // hard cap on fields emitted (runaway guard)
        int structDepth = 2;   // how deep to recurse into nested UScriptStructs
        int arraySample = 8;   // max elements sampled per TArray
    };

    // Emit {"Prop":{"type":..,"offset":"0x..","value":..}, ...} for obj's whole
    // class chain. Writes the braces. `obj` may be null (emits {}).
    void DumpObjectJson(std::ostream& os, UE::UObject* obj, const Options& opts = {});

    // Emit a compact {"ptr","index","name","class","full"} reference for an object
    // (null -> "null"). Shared with the diagnostics writer.
    void WriteObjectRefJson(std::ostream& os, UE::UObject* obj);

    // Resolve an Object/Class-typed property BY NAME via the engine's reflection
    // (walks the class chain's ChildProperties), returning the pointed-to object.
    // Offset-independent -- the principled way to read e.g. "CurrentWeapon" without
    // hard-coding a build-specific offset. Returns nullptr if absent/unreadable.
    UE::UObject* ReadNamedObjectProperty(UE::UObject* obj, const char* propName);

    // Resolve the byte offset of a named property within its container (or -1).
    int FindPropertyOffset(UE::UObject* obj, const char* propName);
}
