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
#include <string>
#include <vector>
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

    // Same, but from a UStruct -- a UClass or UScriptStruct -- rather than an
    // instance. The class carries the property list, so the offset is readable the
    // moment the class loads, with no instance in existence.
    int FindPropertyOffsetInStruct(UE::UObject* structOrClass, const char* propName);

    // The UClass an object-typed property points at (FObjectPropertyBase::PropertyClass).
    // Passed a UFunction and a parameter name, this is the game stating what that
    // parameter will be cast to. Returns nullptr if the property is absent, is not
    // object-typed, or unreadable.
    UE::UObject* ObjectPropertyClassInStruct(UE::UObject* structOrClass, const char* propName);

    // ---- Signature description (declarations, not values) -------------------
    // The above answer "where is this field"; the two below answer "what fields
    // exist at all", which is what you need before you can call a UFunction you
    // have not seen: a params block is only safe to build once every parameter's
    // type, offset and size came from the game itself.

    // One reflected field: a class member, or one parameter of a UFunction.
    struct FieldInfo
    {
        std::string name;
        std::string type;    // FFieldClass name, e.g. "EnumProperty", "ObjectProperty"
        std::string detail;  // resolved enum/struct/class the type points at ("" if none)
        int32_t     offset = -1;   // byte offset within the container / params block
        int32_t     size   = 0;    // ElementSize * ArrayDim
        uint64_t    flags  = 0;    // EPropertyFlags

        bool IsParm()   const { return (flags & Offsets::Reflect::CPF_Parm) != 0; }
        bool IsReturn() const { return (flags & Offsets::Reflect::CPF_ReturnParm) != 0; }
        bool IsOut()    const { return (flags & Offsets::Reflect::CPF_OutParm) != 0; }
    };

    // Describe a UStruct's OWN ChildProperties in declaration order. No SuperStruct
    // walk: a UFunction's parameter list is exactly its own fields, and mixing in
    // inherited members would misdescribe the params block. Pass a UClass to list
    // the members that class itself declares.
    std::vector<FieldInfo> DescribeStructFields(UE::UObject* structOrClass, int maxFields = 128);

    // One entry of a UEnum.
    struct EnumValue
    {
        std::string name;   // as the engine stores it, fully qualified ("EAchievements::KillTwins")
        int64_t     value = 0;
    };

    // ---- TMap reading -------------------------------------------------------
    // A TMap is a TSet of key/value pairs and a TSet is a sparse array plus a hash,
    // so reading one means walking the sparse array and skipping the holes its bit
    // array marks. A view never copies: it hands out pointers INTO the live map, so
    // it is only valid while the owning object is alive and untouched -- read it and
    // be done, do not store it across frames.
    struct MapView
    {
        bool     valid       = false;
        uint8_t* elements    = nullptr; // first stored element
        int32_t  count       = 0;       // slots INCLUDING holes -- iterate with IsValidIndex
        int32_t  stride      = 0;
        int32_t  keyOffset   = 0;
        int32_t  valueOffset = 0;
        int32_t  keySize     = 0;
        int32_t  valueSize   = 0;
        std::string keyType;            // FFieldClass name, e.g. "EnumProperty"
        std::string valueType;
        const uint32_t* allocFlags = nullptr;
        int32_t  allocBits   = 0;
        std::string error;              // why valid == false

        bool     IsValidIndex(int index) const;
        uint8_t* KeyAt(int index) const;
        uint8_t* ValueAt(int index) const;
    };

    // Open a named TMap member of `container` for reading. Fails closed: the map's
    // own FScriptMapLayout is read rather than derived from type sizes, then every
    // part of it is range-checked before a byte of map data is trusted, so a layout
    // a patch has moved reports an error instead of handing back plausible garbage.
    MapView OpenMapProperty(UE::UObject* container, const char* propName);

    // Read a UEnum's name->value table by probing for UEnum::Names and validating
    // the hit against the enum's own name (see Offsets::Reflect::UEnum_NamesProbe*).
    // Returns empty if no candidate validated. `outNamesOffset` receives the offset
    // that won, so a caller can log it for promotion into offsets.h.
    std::vector<EnumValue> DescribeEnumValues(UE::UObject* uenum, int* outNamesOffset = nullptr,
                                              int maxValues = 512);
}
