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
#include "reflect.h"
#include "offsets.h"
#include "../core/memory.h"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <string>

using namespace UE;
namespace R = Offsets::Reflect;

namespace
{
    std::string JsonEscape(const std::string& s)
    {
        std::string o;
        o.reserve(s.size() + 8);
        for (char c : s)
        {
            switch (c)
            {
                case '\"': o += "\\\""; break;
                case '\\': o += "\\\\"; break;
                case '\n': o += "\\n";  break;
                case '\r': o += "\\r";  break;
                case '\t': o += "\\t";  break;
                default:
                    if ((unsigned char)c < 0x20) o += ' ';
                    else o += c;
                    break;
            }
        }
        return o;
    }

    template <typename T> T Rd(const void* p) { return *reinterpret_cast<const T*>(p); }

    std::string SafeName(UObject* o)
    {
        if (!Mem::IsReadable(o, 0x30)) return {};
        try { return o->GetName(); } catch (...) { return {}; }
    }

    std::string SafeFName(const FName& n)
    {
        try { return n.ToString(); } catch (...) { return {}; }
    }

    // FFieldClass::Name -> property type string (e.g. "ObjectProperty").
    std::string PropTypeName(uint8_t* prop)
    {
        if (!Mem::IsReadable(prop + R::FField_ClassPrivate, sizeof(void*))) return {};
        uint8_t* fc = Rd<uint8_t*>(prop + R::FField_ClassPrivate);
        if (!Mem::IsReadable(fc + R::FFieldClass_Name, sizeof(FName))) return {};
        FName fn = Rd<FName>(fc + R::FFieldClass_Name);
        return SafeFName(fn);
    }

    bool TypeIs(const std::string& t, const char* name) { return t == name; }

    // The type STRING says "EnumProperty"; this says which enum. Same idea for
    // object/class/struct/array properties, whose FFieldClass name alone is not
    // enough to build a params block from.
    std::string PropTypeDetail(uint8_t* prop, const std::string& type, int depth = 0)
    {
        auto pointee = [&](int off) -> std::string
        {
            if (!Mem::IsReadable(prop + off, sizeof(void*))) return {};
            return SafeName(Rd<UObject*>(prop + off));
        };

        if (type == "ObjectProperty"     || type == "ClassProperty"     ||
            type == "WeakObjectProperty" || type == "SoftObjectProperty" ||
            type == "LazyObjectProperty" || type == "InterfaceProperty")
            return pointee(R::FObjectProperty_PropClass);
        if (type == "StructProperty") return pointee(R::FStructProperty_Struct);
        if (type == "EnumProperty")   return pointee(R::FEnumProperty_Enum);
        if (type == "ByteProperty")   return pointee(R::FByteProperty_Enum);
        if (type == "ArrayProperty" && depth < 2)
        {
            if (!Mem::IsReadable(prop + R::FArrayProperty_Inner, sizeof(void*))) return {};
            uint8_t* inner = Rd<uint8_t*>(prop + R::FArrayProperty_Inner);
            if (!Mem::IsReadable(inner, R::FField_Name + sizeof(FName))) return {};
            std::string innerType   = PropTypeName(inner);
            std::string innerDetail = PropTypeDetail(inner, innerType, depth + 1);
            if (innerType.empty()) return {};
            return innerDetail.empty() ? innerType : innerType + "<" + innerDetail + ">";
        }
        return {};
    }

    void WriteFloat(std::ostream& os, float v)
    {
        if (std::isfinite(v)) os << v; else os << "\"nan\"";
    }

    // Forward decls (mutual recursion: struct value -> properties).
    void DumpStructValue(std::ostream& os, UObject* strct, uint8_t* base,
                         const Reflect::Options& opts, int depth, int& budget);
    void DumpPropValue(std::ostream& os, uint8_t* prop, const std::string& type,
                       uint8_t* valueAddr, const Reflect::Options& opts, int depth, int& budget);

    // Decode a known math struct by name; returns false if not a recognised one.
    bool DumpMathStruct(std::ostream& os, const std::string& sname, uint8_t* v)
    {
        if (sname == "Vector" && Mem::IsReadable(v, 12))
        {
            FVector f = Rd<FVector>(v);
            os << "{\"x\":"; WriteFloat(os, f.X); os << ",\"y\":"; WriteFloat(os, f.Y);
            os << ",\"z\":"; WriteFloat(os, f.Z); os << "}";
            return true;
        }
        if (sname == "Rotator" && Mem::IsReadable(v, 12))
        {
            FRotator r = Rd<FRotator>(v);
            os << "{\"pitch\":"; WriteFloat(os, r.Pitch); os << ",\"yaw\":"; WriteFloat(os, r.Yaw);
            os << ",\"roll\":"; WriteFloat(os, r.Roll); os << "}";
            return true;
        }
        if (sname == "Vector2D" && Mem::IsReadable(v, 8))
        {
            os << "{\"x\":"; WriteFloat(os, Rd<float>(v)); os << ",\"y\":"; WriteFloat(os, Rd<float>(v + 4)); os << "}";
            return true;
        }
        if ((sname == "LinearColor") && Mem::IsReadable(v, 16))
        {
            os << "{\"r\":"; WriteFloat(os, Rd<float>(v)); os << ",\"g\":"; WriteFloat(os, Rd<float>(v + 4));
            os << ",\"b\":"; WriteFloat(os, Rd<float>(v + 8)); os << ",\"a\":"; WriteFloat(os, Rd<float>(v + 12)); os << "}";
            return true;
        }
        if (sname == "Color" && Mem::IsReadable(v, 4))
        {
            os << "{\"b\":" << (int)v[0] << ",\"g\":" << (int)v[1] << ",\"r\":" << (int)v[2] << ",\"a\":" << (int)v[3] << "}";
            return true;
        }
        if ((sname == "Quat") && Mem::IsReadable(v, 16))
        {
            os << "{\"x\":"; WriteFloat(os, Rd<float>(v)); os << ",\"y\":"; WriteFloat(os, Rd<float>(v + 4));
            os << ",\"z\":"; WriteFloat(os, Rd<float>(v + 8)); os << ",\"w\":"; WriteFloat(os, Rd<float>(v + 12)); os << "}";
            return true;
        }
        return false;
    }

    void DumpPropValue(std::ostream& os, uint8_t* prop, const std::string& type,
                       uint8_t* valueAddr, const Reflect::Options& opts, int depth, int& budget)
    {
        if (!Mem::IsReadable(valueAddr, 1)) { os << "null"; return; }

        if (TypeIs(type, "ObjectProperty") || TypeIs(type, "ClassProperty") ||
            TypeIs(type, "InterfaceProperty"))
        {
            if (!Mem::IsReadable(valueAddr, sizeof(void*))) { os << "null"; return; }
            Reflect::WriteObjectRefJson(os, Rd<UObject*>(valueAddr));
            return;
        }
        if (TypeIs(type, "WeakObjectProperty"))
        {
            if (!Mem::IsReadable(valueAddr, 8)) { os << "null"; return; }
            int32_t idx = Rd<int32_t>(valueAddr);
            UObject* o = (idx >= 0) ? GetObjectByIndex(idx) : nullptr;
            Reflect::WriteObjectRefJson(os, o);
            return;
        }
        if (TypeIs(type, "FloatProperty")) { if (Mem::IsReadable(valueAddr, 4)) WriteFloat(os, Rd<float>(valueAddr)); else os << "null"; return; }
        if (TypeIs(type, "DoubleProperty")) { if (Mem::IsReadable(valueAddr, 8)) { double d = Rd<double>(valueAddr); if (std::isfinite(d)) os << d; else os << "\"nan\""; } else os << "null"; return; }
        if (TypeIs(type, "IntProperty")) { if (Mem::IsReadable(valueAddr, 4)) os << Rd<int32_t>(valueAddr); else os << "null"; return; }
        if (TypeIs(type, "Int64Property")) { if (Mem::IsReadable(valueAddr, 8)) os << Rd<int64_t>(valueAddr); else os << "null"; return; }
        if (TypeIs(type, "Int16Property")) { if (Mem::IsReadable(valueAddr, 2)) os << Rd<int16_t>(valueAddr); else os << "null"; return; }
        if (TypeIs(type, "Int8Property")) { if (Mem::IsReadable(valueAddr, 1)) os << (int)Rd<int8_t>(valueAddr); else os << "null"; return; }
        if (TypeIs(type, "UInt32Property")) { if (Mem::IsReadable(valueAddr, 4)) os << Rd<uint32_t>(valueAddr); else os << "null"; return; }
        if (TypeIs(type, "UInt16Property")) { if (Mem::IsReadable(valueAddr, 2)) os << Rd<uint16_t>(valueAddr); else os << "null"; return; }
        if (TypeIs(type, "UInt64Property")) { if (Mem::IsReadable(valueAddr, 8)) os << Rd<uint64_t>(valueAddr); else os << "null"; return; }
        if (TypeIs(type, "BoolProperty"))
        {
            uint8_t byteOff = Mem::IsReadable(prop + R::FBoolProperty_ByteOffset, 1) ? prop[R::FBoolProperty_ByteOffset] : 0;
            uint8_t mask    = Mem::IsReadable(prop + R::FBoolProperty_ByteMask, 1) ? prop[R::FBoolProperty_ByteMask] : 0xFF;
            if (!Mem::IsReadable(valueAddr + byteOff, 1)) { os << "null"; return; }
            os << (((valueAddr[byteOff] & mask) != 0) ? "true" : "false");
            return;
        }
        if (TypeIs(type, "ByteProperty") || TypeIs(type, "EnumProperty"))
        {
            // EnumProperty stores its value in an underlying numeric property; most
            // enums are 1 byte. Read one byte and (best-effort) the enum name.
            if (!Mem::IsReadable(valueAddr, 1)) { os << "null"; return; }
            int val = (int)Rd<uint8_t>(valueAddr);
            uint8_t* enumPtr = nullptr;
            int enumOff = TypeIs(type, "EnumProperty") ? R::FEnumProperty_Enum : R::FByteProperty_Enum;
            if (Mem::IsReadable(prop + enumOff, sizeof(void*)))
                enumPtr = Rd<uint8_t*>(prop + enumOff);
            std::string ename = SafeName(reinterpret_cast<UObject*>(enumPtr));
            if (!ename.empty()) os << "{\"value\":" << val << ",\"enum\":\"" << JsonEscape(ename) << "\"}";
            else os << val;
            return;
        }
        if (TypeIs(type, "NameProperty")) { if (Mem::IsReadable(valueAddr, sizeof(FName))) os << "\"" << JsonEscape(SafeFName(Rd<FName>(valueAddr))) << "\""; else os << "null"; return; }
        if (TypeIs(type, "StrProperty"))
        {
            if (!Mem::IsReadable(valueAddr, sizeof(FString))) { os << "null"; return; }
            FString s = Rd<FString>(valueAddr);
            std::string str; try { str = s.ToString(); } catch (...) {}
            os << "\"" << JsonEscape(str) << "\"";
            return;
        }
        if (TypeIs(type, "StructProperty"))
        {
            uint8_t* strct = nullptr;
            if (Mem::IsReadable(prop + R::FStructProperty_Struct, sizeof(void*)))
                strct = Rd<uint8_t*>(prop + R::FStructProperty_Struct);
            std::string sname = SafeName(reinterpret_cast<UObject*>(strct));
            if (DumpMathStruct(os, sname, valueAddr)) return;
            if (depth > 0 && Mem::IsReadable(strct, 0x60))
            {
                DumpStructValue(os, reinterpret_cast<UObject*>(strct), valueAddr, opts, depth - 1, budget);
                return;
            }
            os << "{\"struct\":\"" << JsonEscape(sname) << "\"}";
            return;
        }
        if (TypeIs(type, "ArrayProperty"))
        {
            if (!Mem::IsReadable(valueAddr, 0x10)) { os << "null"; return; }
            uint8_t* data = Rd<uint8_t*>(valueAddr);
            int32_t count = Rd<int32_t>(valueAddr + 8);
            uint8_t* inner = Mem::IsReadable(prop + R::FArrayProperty_Inner, sizeof(void*)) ? Rd<uint8_t*>(prop + R::FArrayProperty_Inner) : nullptr;
            std::string innerType = inner ? PropTypeName(inner) : std::string{};
            int32_t innerSize = (inner && Mem::IsReadable(inner + R::FProperty_ElementSize, 4)) ? Rd<int32_t>(inner + R::FProperty_ElementSize) : 0;
            os << "{\"count\":" << count << ",\"inner\":\"" << JsonEscape(innerType) << "\"";
            int sample = (count < opts.arraySample) ? count : opts.arraySample;
            if (data && innerSize > 0 && sample > 0 && Mem::IsReadable(data, (size_t)sample * innerSize))
            {
                os << ",\"items\":[";
                for (int i = 0; i < sample; ++i)
                {
                    if (i) os << ",";
                    DumpPropValue(os, inner, innerType, data + (size_t)i * innerSize, opts, 0, budget);
                }
                os << "]";
            }
            os << "}";
            return;
        }
        // Unhandled (Text/Map/Set/Delegate/SoftObject/FieldPath/...): note type only.
        os << "{\"unhandled\":\"" << JsonEscape(type) << "\"}";
    }

    // Walk one UStruct's (and its SuperStruct chain's) ChildProperties over the
    // value at `base`, emitting "name":{type,offset,value}. Writes the {} braces.
    void DumpStructValue(std::ostream& os, UObject* strct, uint8_t* base,
                         const Reflect::Options& opts, int depth, int& budget)
    {
        os << "{";
        bool first = true;
        UObject* cur = strct;
        for (int classDepth = 0; cur && classDepth < 64 && budget > 0; ++classDepth)
        {
            if (!Mem::IsReadable(reinterpret_cast<uint8_t*>(cur) + Offsets::O_UStruct_ChildProps, sizeof(void*)))
                break;
            uint8_t* prop = *reinterpret_cast<uint8_t**>(reinterpret_cast<uint8_t*>(cur) + Offsets::O_UStruct_ChildProps);
            for (int n = 0; prop && n < 4096 && budget > 0; ++n)
            {
                if (!Mem::IsReadable(prop, R::FField_Name + sizeof(FName)))
                    break;
                std::string pname = SafeFName(Rd<FName>(prop + R::FField_Name));
                std::string ptype = PropTypeName(prop);
                int32_t off = Mem::IsReadable(prop + R::FProperty_Offset, 4) ? Rd<int32_t>(prop + R::FProperty_Offset) : -1;
                if (!pname.empty() && !ptype.empty() && off >= 0 && off < 0x40000)
                {
                    if (!first) os << ",";
                    first = false;
                    --budget;
                    os << "\"" << JsonEscape(pname) << "\":{\"type\":\"" << JsonEscape(ptype)
                       << "\",\"offset\":\"0x" << std::hex << off << std::dec << "\",\"value\":";
                    try { DumpPropValue(os, prop, ptype, base + off, opts, depth, budget); }
                    catch (...) { os << "\"<fault>\""; }
                    os << "}";
                }
                prop = Mem::IsReadable(prop + R::FField_Next, sizeof(void*)) ? Rd<uint8_t*>(prop + R::FField_Next) : nullptr;
            }
            // ascend to the super struct/class
            cur = Mem::IsReadable(reinterpret_cast<uint8_t*>(cur) + Offsets::O_UStruct_SuperStruct, sizeof(void*))
                ? *reinterpret_cast<UObject**>(reinterpret_cast<uint8_t*>(cur) + Offsets::O_UStruct_SuperStruct)
                : nullptr;
        }
        os << "}";
    }
}

void Reflect::WriteObjectRefJson(std::ostream& os, UObject* o)
{
    if (!Mem::IsReadable(o, 0x30)) { os << "null"; return; }
    int idx = -1;
    try { idx = o->Index(); } catch (...) {}
    std::string name, cls, full;
    try { name = o->GetName(); } catch (...) {}
    try { UObject* c = o->Class(); if (Mem::IsReadable(c, 0x30)) cls = c->GetName(); } catch (...) {}
    try { full = o->GetFullName(); } catch (...) {}
    char ptr[32];
    std::snprintf(ptr, sizeof(ptr), "0x%llX", (unsigned long long)(uintptr_t)o);
    os << "{\"ptr\":\"" << ptr << "\",\"index\":" << idx
       << ",\"name\":\"" << JsonEscape(name) << "\""
       << ",\"class\":\"" << JsonEscape(cls) << "\""
       << ",\"full\":\"" << JsonEscape(full) << "\"}";
}

namespace
{
    // Walk a UStruct's own ChildProperties, then its SuperStruct chain, for a
    // property named `propName`. Returns the FField* (as bytes) and writes its
    // value offset to *outOffset.
    uint8_t* FindPropertyFieldInStruct(UObject* structOrClass, const char* propName, int32_t* outOffset)
    {
        if (outOffset) *outOffset = -1;
        if (!propName || !*propName) return nullptr;
        UObject* cur = structOrClass;
        for (int classDepth = 0; Mem::IsReadable(cur, 0x60) && classDepth < 64; ++classDepth)
        {
            uint8_t* prop = nullptr;
            if (Mem::IsReadable(reinterpret_cast<uint8_t*>(cur) + Offsets::O_UStruct_ChildProps, sizeof(void*)))
                prop = *reinterpret_cast<uint8_t**>(reinterpret_cast<uint8_t*>(cur) + Offsets::O_UStruct_ChildProps);
            for (int n = 0; prop && n < 4096; ++n)
            {
                if (Mem::IsReadable(prop + R::FField_Name, sizeof(FName)))
                {
                    std::string pname = SafeFName(Rd<FName>(prop + R::FField_Name));
                    if (pname == propName)
                    {
                        if (outOffset && Mem::IsReadable(prop + R::FProperty_Offset, 4))
                            *outOffset = Rd<int32_t>(prop + R::FProperty_Offset);
                        return prop;
                    }
                }
                prop = Mem::IsReadable(prop + R::FField_Next, sizeof(void*)) ? Rd<uint8_t*>(prop + R::FField_Next) : nullptr;
            }
            cur = Mem::IsReadable(reinterpret_cast<uint8_t*>(cur) + Offsets::O_UStruct_SuperStruct, sizeof(void*))
                ? *reinterpret_cast<UObject**>(reinterpret_cast<uint8_t*>(cur) + Offsets::O_UStruct_SuperStruct)
                : nullptr;
        }
        return nullptr;
    }

    // Same lookup, entered from an instance instead of from its class.
    uint8_t* FindPropertyField(UObject* obj, const char* propName, int32_t* outOffset)
    {
        if (outOffset) *outOffset = -1;
        if (!Mem::IsReadable(obj, 0x30)) return nullptr;
        UObject* cls = nullptr;
        try { cls = obj->Class(); } catch (...) { return nullptr; }
        return FindPropertyFieldInStruct(cls, propName, outOffset);
    }
}

int Reflect::FindPropertyOffset(UObject* obj, const char* propName)
{
    int32_t off = -1;
    try { FindPropertyField(obj, propName, &off); } catch (...) { off = -1; }
    return off;
}

int Reflect::FindPropertyOffsetInStruct(UObject* structOrClass, const char* propName)
{
    int32_t off = -1;
    try { FindPropertyFieldInStruct(structOrClass, propName, &off); } catch (...) { off = -1; }
    return off;
}

UObject* Reflect::ObjectPropertyClassInStruct(UObject* structOrClass, const char* propName)
{
    try
    {
        uint8_t* prop = FindPropertyFieldInStruct(structOrClass, propName, nullptr);
        if (!prop)
            return nullptr;

        // PropertyClass sits at the same offset across the FObjectPropertyBase
        // family but not outside it, so the type name gates the read.
        std::string type = PropTypeName(prop);
        if (type != "ObjectProperty" && type != "ClassProperty" &&
            type != "SoftObjectProperty" && type != "SoftClassProperty" &&
            type != "WeakObjectProperty" && type != "LazyObjectProperty")
            return nullptr;

        if (!Mem::IsReadable(prop + R::FObjectProperty_PropClass, sizeof(void*)))
            return nullptr;
        UObject* cls = Rd<UObject*>(prop + R::FObjectProperty_PropClass);
        return Mem::IsReadable(cls, 0x30) ? cls : nullptr;
    }
    catch (...) { return nullptr; }
}

UObject* Reflect::ReadNamedObjectProperty(UObject* obj, const char* propName)
{
    try
    {
        int32_t off = -1;
        if (!FindPropertyField(obj, propName, &off) || off < 0 || off >= 0x40000)
            return nullptr;
        uint8_t* addr = reinterpret_cast<uint8_t*>(obj) + off;
        if (!Mem::IsReadable(addr, sizeof(void*)))
            return nullptr;
        UObject* o = Rd<UObject*>(addr);
        return Mem::IsReadable(o, 0x30) ? o : nullptr;
    }
    catch (...) { return nullptr; }
}

void Reflect::DumpObjectJson(std::ostream& os, UObject* obj, const Options& opts)
{
    if (!Mem::IsReadable(obj, 0x30)) { os << "{}"; return; }
    UObject* cls = nullptr;
    try { cls = obj->Class(); } catch (...) {}
    if (!Mem::IsReadable(cls, 0x30)) { os << "{}"; return; }
    int budget = opts.maxProps;
    DumpStructValue(os, cls, reinterpret_cast<uint8_t*>(obj), opts, opts.structDepth, budget);
}

std::vector<Reflect::FieldInfo> Reflect::DescribeStructFields(UObject* structOrClass, int maxFields)
{
    std::vector<FieldInfo> out;
    try
    {
        if (!Mem::IsReadable(structOrClass, Offsets::O_UStruct_ChildProps + sizeof(void*)))
            return out;
        if (maxFields < 0) maxFields = 0;
        out.reserve((size_t)(maxFields < 32 ? maxFields : 32));

        uint8_t* prop = *reinterpret_cast<uint8_t**>(
            reinterpret_cast<uint8_t*>(structOrClass) + Offsets::O_UStruct_ChildProps);

        for (int n = 0; prop && n < maxFields; ++n)
        {
            // A short read here is a broken FField list, not an empty one -- stop
            // rather than describe fields we cannot fully see.
            if (!Mem::IsReadable(prop, R::FProperty_Offset + 4))
                break;

            FieldInfo f;
            f.name = SafeFName(Rd<FName>(prop + R::FField_Name));
            f.type = PropTypeName(prop);
            f.detail = PropTypeDetail(prop, f.type);
            f.offset = Rd<int32_t>(prop + R::FProperty_Offset);
            f.flags  = Rd<uint64_t>(prop + R::FProperty_PropertyFlags);
            const int32_t elementSize = Rd<int32_t>(prop + R::FProperty_ElementSize);
            const int32_t arrayDim    = Rd<int32_t>(prop + R::FProperty_ArrayDim);
            f.size = (elementSize > 0 && arrayDim > 0 && arrayDim < 0x10000)
                       ? elementSize * arrayDim : elementSize;
            out.push_back(std::move(f));

            prop = Mem::IsReadable(prop + R::FField_Next, sizeof(void*))
                     ? Rd<uint8_t*>(prop + R::FField_Next) : nullptr;
        }
    }
    catch (...) { /* partial list is still useful; report what we read */ }
    return out;
}

std::vector<Reflect::EnumValue> Reflect::DescribeEnumValues(UObject* uenum, int* outNamesOffset,
                                                            int maxValues)
{
    std::vector<EnumValue> out;
    if (outNamesOffset) *outNamesOffset = -1;
    try
    {
        if (!Mem::IsReadable(uenum, 0x30) || maxValues <= 0)
            return out;

        // UE4 stores enum entries fully qualified ("EAchievements::KillTwins"), so
        // the enum's own name is a signature only the real Names array carries.
        const std::string self = SafeName(uenum);
        if (self.empty()) return out;
        const std::string prefix = self + "::";

        for (int off = R::UEnum_NamesProbeFirst; off <= R::UEnum_NamesProbeLast; off += 8)
        {
            uint8_t* at = reinterpret_cast<uint8_t*>(uenum) + off;
            if (!Mem::IsReadable(at, 16)) continue;

            uint8_t*      data  = Rd<uint8_t*>(at);
            const int32_t count = Rd<int32_t>(at + 8);
            const int32_t max   = Rd<int32_t>(at + 12);
            if (!data || count <= 0 || count > maxValues || max < count) continue;
            if (!Mem::IsReadable(data, (size_t)count * R::UEnum_NamesEntrySize)) continue;

            // Validation: a wrong offset can produce a plausible-looking TArray, but
            // not one whose first entry names this very enum.
            const std::string first = SafeFName(Rd<FName>(data));
            if (first.compare(0, prefix.size(), prefix) != 0) continue;

            out.reserve((size_t)count);
            for (int i = 0; i < count; ++i)
            {
                uint8_t* entry = data + (size_t)i * R::UEnum_NamesEntrySize;
                EnumValue v;
                v.name  = SafeFName(Rd<FName>(entry));
                v.value = Rd<int64_t>(entry + 8);
                if (v.name.empty()) continue;
                out.push_back(std::move(v));
            }
            if (outNamesOffset) *outNamesOffset = off;
            return out;
        }
    }
    catch (...) { out.clear(); }
    return out;
}

// ---- TMap reading -------------------------------------------------------------
bool Reflect::MapView::IsValidIndex(int index) const
{
    if (!valid || index < 0 || index >= count) return false;
    if (!allocFlags || index >= allocBits) return false;
    // TBitArray packs 32 slots per uint32; a clear bit is a hole left by a removal.
    const uint32_t word = allocFlags[index >> 5];
    return (word & (1u << (index & 31))) != 0;
}

uint8_t* Reflect::MapView::KeyAt(int index) const
{
    if (!IsValidIndex(index)) return nullptr;
    return elements + (size_t)index * stride + keyOffset;
}

uint8_t* Reflect::MapView::ValueAt(int index) const
{
    if (!IsValidIndex(index)) return nullptr;
    return elements + (size_t)index * stride + valueOffset;
}

namespace
{
    // Shared by both entry points: everything past "where is the map and what
    // describes it" is identical whether the TMap sits on a class or inside a
    // nested struct.
    Reflect::MapView OpenMapFromField(uint8_t* prop, uint8_t* mapStorage)
    {
        Reflect::MapView view;

        if (!prop || !mapStorage)        { view.error = "property not reflected"; return view; }
        if (PropTypeName(prop) != "MapProperty") { view.error = "property is not a TMap"; return view; }
        if (!Mem::IsReadable(prop, R::FMapProperty_SparseElemSize + 4))
        {                                  view.error = "map property truncated"; return view; }

        // The layout comes from the property itself -- no guessing at alignment.
        // The key offset is the one part NOT stored: UE omits it because a key is
        // always at zero within its pair.
        const int32_t keyOffset   = R::FMapProperty_KeyOffsetConst;
        const int32_t valueOffset = Rd<int32_t>(prop + R::FMapProperty_ValueOffset);
        const int32_t stride      = Rd<int32_t>(prop + R::FMapProperty_SparseElemSize);

        uint8_t* keyProp   = Mem::IsReadable(prop + R::FMapProperty_KeyProp, sizeof(void*))
                               ? Rd<uint8_t*>(prop + R::FMapProperty_KeyProp) : nullptr;
        uint8_t* valueProp = Mem::IsReadable(prop + R::FMapProperty_ValueProp, sizeof(void*))
                               ? Rd<uint8_t*>(prop + R::FMapProperty_ValueProp) : nullptr;
        if (!Mem::IsReadable(keyProp, R::FProperty_Offset + 4) ||
            !Mem::IsReadable(valueProp, R::FProperty_Offset + 4))
        {                                  view.error = "key/value property unreadable"; return view; }

        view.keyType   = PropTypeName(keyProp);
        view.valueType = PropTypeName(valueProp);
        view.keySize   = Rd<int32_t>(keyProp   + R::FProperty_ElementSize);
        view.valueSize = Rd<int32_t>(valueProp + R::FProperty_ElementSize);

        // A layout a patch has moved shows up here as nonsense, not as bad data.
        // The type names go into the message too, so a rejected map can be told
        // apart from a map whose value is simply not the type the caller expected.
        if (stride <= 0 || stride > 4096 ||
            valueOffset < view.keySize || valueOffset >= stride ||
            view.keySize <= 0 || view.valueSize <= 0 ||
            valueOffset + view.valueSize > stride)
        {
            char why[256];
            std::snprintf(why, sizeof(why),
                          "layout failed validation: TMap<%s(%d), %s(%d)> val@%d stride=%d",
                          view.keyType.c_str(), view.keySize,
                          view.valueType.c_str(), view.valueSize, valueOffset, stride);
            view.error = why;
            return view;
        }

        if (!Mem::IsReadable(mapStorage, 0x30)) { view.error = "map storage unreadable"; return view; }

        uint8_t*      elements = Rd<uint8_t*>(mapStorage + R::FScriptMap_ElementsData);
        const int32_t num      = Rd<int32_t>(mapStorage + R::FScriptMap_ElementsNum);
        const int32_t maxNum   = Rd<int32_t>(mapStorage + R::FScriptMap_ElementsMax);
        const int32_t numBits  = Rd<int32_t>(mapStorage + R::FScriptMap_AllocFlagsNumBits);

        if (num == 0) { view.valid = true; return view; }  // empty is not an error
        if (num < 0 || num > R::FScriptMap_MaxEntries || maxNum < num || numBits < num)
        {                                  view.error = "element count implausible"; return view; }
        if (!Mem::IsReadable(elements, (size_t)num * stride))
        {                                  view.error = "element storage unreadable"; return view; }

        // TInlineAllocator: the bits live inline until they outgrow it, at which
        // point the secondary pointer takes over. Non-null means "use the heap one".
        const uint32_t* flags = nullptr;
        uint8_t* heap = Mem::IsReadable(mapStorage + R::FScriptMap_AllocFlagsHeap, sizeof(void*))
                          ? Rd<uint8_t*>(mapStorage + R::FScriptMap_AllocFlagsHeap) : nullptr;
        const size_t flagBytes = ((size_t)numBits + 31) / 32 * 4;
        if (heap)
        {
            // A non-null secondary pointer means the heap copy is the live one; if
            // it cannot be read, the inline bits are stale and must not stand in.
            if (!Mem::IsReadable(heap, flagBytes))
            {                                  view.error = "allocation flags unreadable"; return view; }
            flags = reinterpret_cast<const uint32_t*>(heap);
        }
        else if (numBits <= R::FScriptMap_InlineFlagBits &&
                 Mem::IsReadable(mapStorage + R::FScriptMap_AllocFlagsInline, flagBytes))
            flags = reinterpret_cast<const uint32_t*>(mapStorage + R::FScriptMap_AllocFlagsInline);
        if (!flags)                      { view.error = "allocation flags unreadable"; return view; }

        view.elements    = elements;
        view.count       = num;
        view.stride      = stride;
        view.keyOffset   = keyOffset;
        view.valueOffset = valueOffset;
        view.allocFlags  = flags;
        view.allocBits   = numBits;
        view.valid       = true;
        return view;
    }
}

Reflect::MapView Reflect::OpenMapProperty(UObject* container, const char* propName)
{
    MapView view;
    try
    {
        if (!Mem::IsReadable(container, 0x30)) { view.error = "container unreadable"; return view; }
        int32_t  mapOffset = -1;
        uint8_t* prop      = FindPropertyField(container, propName, &mapOffset);
        if (!prop || mapOffset < 0) { view.error = "property not reflected"; return view; }
        return OpenMapFromField(prop, reinterpret_cast<uint8_t*>(container) + mapOffset);
    }
    catch (...) { view.valid = false; view.error = "exception while opening map"; }
    return view;
}
