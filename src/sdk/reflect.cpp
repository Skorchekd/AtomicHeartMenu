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
    // Walk obj's class chain ChildProperties for a property named `propName`.
    // Returns the FField* (as bytes) and writes its value offset to *outOffset.
    uint8_t* FindPropertyField(UObject* obj, const char* propName, int32_t* outOffset)
    {
        if (outOffset) *outOffset = -1;
        if (!Mem::IsReadable(obj, 0x30) || !propName || !*propName) return nullptr;
        UObject* cur = nullptr;
        try { cur = obj->Class(); } catch (...) {}
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
}

int Reflect::FindPropertyOffset(UObject* obj, const char* propName)
{
    int32_t off = -1;
    try { FindPropertyField(obj, propName, &off); } catch (...) { off = -1; }
    return off;
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
