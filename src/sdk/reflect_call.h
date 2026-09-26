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
#include "ue4.h"
#include "offsets.h"
#include "reflect.h"
#include "../core/memory.h"
#include "../core/log.h"
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

// Calling into game code by name. Every parameter is placed at the offset and
// with the size the game's own reflection data gives for it, so a function whose
// signature differs on this build refuses the call instead of receiving a frame
// laid out for a guessed struct. Shared by the Workbench, sandbox and play-as
// features. Game thread only, like every ProcessEvent.
namespace RefCall
{
    // Index alone cannot distinguish an object recycled into the same slot.
    // These references also verify the object's engine-assigned name.
    struct ObjectRef
    {
        UE::UObject* ptr = nullptr;
        int index = -1;
        UE::FName name{};
        ObjectRef() = default;
        explicit ObjectRef(UE::UObject* object)
        {
            if (UE::IsLiveObject(object))
            {
                ptr = object;
                index = object->Index();
                name = *object->NamePtr();
            }
        }
        UE::UObject* Get() const
        {
            if (!UE::IsLiveObject(ptr) || ptr->Index() != index) return nullptr;
            UE::FName current = *ptr->NamePtr();
            return current.ComparisonIndex == name.ComparisonIndex && current.Number == name.Number ? ptr : nullptr;
        }
    };

    template<typename T> bool ReadAt(const void* object, int offset, T& out)
    {
        if (!object || offset < 0) return false;
        const auto* field = static_cast<const uint8_t*>(object) + offset;
        if (!Mem::IsReadable(field, sizeof(T))) return false;
        std::memcpy(&out, field, sizeof(T));
        return true;
    }

    // A UFunction by short name on the receiver's class or any class above it.
    inline UE::UFunction* Function(UE::UObject* receiver, const char* name)
    {
        if (!UE::IsLiveObject(receiver)) return nullptr;
        UE::UObject* cls = receiver->Class();
        for (int depth = 0; depth < 64 && UE::IsLiveObject(cls); ++depth)
        {
            if (auto* fn = UE::FindFunctionInClass(cls, name))
                return UE::IsLiveObject(fn) ? fn : nullptr;
            UE::UObject* parent = nullptr;
            if (!ReadAt(cls, Offsets::O_UStruct_SuperStruct, parent)) break;
            cls = parent;
        }
        return nullptr;
    }

    // Allocate the runtime reflected frame, including Blueprint local storage.
    // Parameters are resolved by name, so a shifted member is not a guessed write.
    struct Call
    {
        UE::UObject* receiver;
        UE::UFunction* fn;
        std::vector<uint8_t> data;
        bool valid = false;
        Call(UE::UObject* object, const char* name) : receiver(object), fn(Function(object, name))
        {
            int size = 0;
            if (!fn || !ReadAt(fn, Offsets::O_UStruct_PropertiesSize, size) || size < 0 || size > 65536) return;
            data.resize((std::max)(size, 1));
            valid = true;
        }
        template<typename T> bool Set(const char* name, const T& value)
        {
            int off = -1, size = 0, dim = 0;
            if (!valid || !Reflect::PropertyLayoutInStruct(fn, name, off, size, dim) ||
                dim != 1 || size != sizeof(T) || size_t(off) + sizeof(T) > data.size())
            { valid = false; return false; }
            std::memcpy(data.data() + off, &value, sizeof(T));
            return true;
        }
        template<typename T> bool Get(const char* name, T& value) const
        {
            int off = -1, size = 0, dim = 0;
            if (!valid || !Reflect::PropertyLayoutInStruct(fn, name, off, size, dim) ||
                dim != 1 || size != sizeof(T) || size_t(off) + sizeof(T) > data.size()) return false;
            std::memcpy(&value, data.data() + off, sizeof(T));
            return true;
        }
        bool Run()
        {
            if (!valid) LOG("Reflected call unavailable: receiver=%p function=%p", (void*)receiver, (void*)fn);
            return valid && UE::IsLiveObject(receiver) && UE::IsLiveObject(fn) && receiver->ProcessEvent(fn, data.data());
        }
    };

    // Call a no-argument function, or read a single return value.
    inline bool CallNoArgs(UE::UObject* object, const char* name)
    {
        Call call(object, name);
        return call.Run();
    }
    template<typename T> bool CallReturning(UE::UObject* object, const char* name, T& out)
    {
        Call call(object, name);
        return call.Run() && call.Get("ReturnValue", out);
    }
}
