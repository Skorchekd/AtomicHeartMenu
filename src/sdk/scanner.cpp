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
#include "scanner.h"
#include "../core/globals.h"
#include <Windows.h>
#include <cstdlib>
#include <vector>

// Standard IDA-style AOB scanner. With USE_STATIC_OFFSETS this is the fallback
// path (ue4.cpp resolves GObjects/GNames/GWorld from a dump), but it must still
// link, and it works if you ever flip back to scanning.

namespace
{
    // Scan range [base, base+size). Prefer the game-exe module info captured at
    // injection; fall back to the main module's PE headers.
    bool GetMainModuleRange(uint8_t*& base, size_t& size)
    {
        if (G::moduleBase && G::moduleSize)
        {
            base = G::moduleBase;
            size = G::moduleSize;
            return true;
        }
        HMODULE h = GetModuleHandleA(nullptr);
        if (!h)
            return false;
        base = reinterpret_cast<uint8_t*>(h);
        auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE)
            return false;
        auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE)
            return false;
        size = nt->OptionalHeader.SizeOfImage;
        return true;
    }

    // One contiguous scan range.
    struct ScanRange { uint8_t* base; size_t size; };

    // Executable sections of the main module only. Code signatures should never
    // match inside .rdata/.data, and on a 400 MB image those data sections cause
    // false "ambiguous" duplicates. Restricting to executable sections makes a
    // genuinely-unique function prologue resolve uniquely. PE parse failure is a
    // hard failure: silently scanning the whole image would mix data matches into
    // code signature counts and violate the hook fail-closed policy.
    bool GetExecRanges(std::vector<ScanRange>& out)
    {
        uint8_t* base = nullptr;
        size_t   size = 0;
        if (!GetMainModuleRange(base, size))
            return false;

        auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic == IMAGE_DOS_SIGNATURE)
        {
            auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
            if (nt->Signature == IMAGE_NT_SIGNATURE)
            {
                auto* sec = IMAGE_FIRST_SECTION(nt);
                for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i)
                {
                    if (!(sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE))
                        continue;
                    size_t vsz = sec[i].Misc.VirtualSize ? sec[i].Misc.VirtualSize
                                                         : sec[i].SizeOfRawData;
                    if (vsz)
                        out.push_back({ base + sec[i].VirtualAddress, vsz });
                }
            }
        }
        return !out.empty();
    }

    bool GetRanges(Scanner::Scope scope, std::vector<ScanRange>& out)
    {
        if (scope == Scanner::Scope::ExecutableSections)
            return GetExecRanges(out);
        uint8_t* base = nullptr;
        size_t size = 0;
        if (!GetMainModuleRange(base, size))
            return false;
        out.push_back({ base, size });
        return true;
    }

    // Parse "48 8B ?? C0" (or "??" wildcards) into a byte array + match mask.
    void ParsePattern(const char* pattern, std::vector<uint8_t>& bytes, std::vector<bool>& mask)
    {
        for (const char* p = pattern; *p; )
        {
            if (*p == ' ') { ++p; continue; }
            if (*p == '?')
            {
                bytes.push_back(0);
                mask.push_back(false); // wildcard byte
                ++p;
                if (*p == '?') ++p; // accept "??"
            }
            else
            {
                bytes.push_back(static_cast<uint8_t>(strtoul(p, nullptr, 16)));
                mask.push_back(true);
                ++p;
                if (*p && *p != ' ') ++p; // step past the 2nd hex digit
            }
        }
    }
}

namespace Scanner
{
    uint8_t* Find(const char* pattern, Scope scope)
    {
        std::vector<ScanRange> ranges;
        if (!GetRanges(scope, ranges))
            return nullptr;

        std::vector<uint8_t> bytes;
        std::vector<bool>    mask;
        ParsePattern(pattern, bytes, mask);
        const size_t n = bytes.size();
        if (n == 0)
            return nullptr;

        for (const ScanRange& r : ranges)
        {
            if (r.size < n) continue;
            for (size_t i = 0; i + n <= r.size; ++i)
            {
                bool ok = true;
                for (size_t j = 0; j < n; ++j)
                    if (mask[j] && r.base[i + j] != bytes[j]) { ok = false; break; }
                if (ok)
                    return r.base + i;
            }
        }
        return nullptr;
    }

    int CountMatches(const char* pattern, int cap, Scope scope)
    {
        std::vector<ScanRange> ranges;
        if (!GetRanges(scope, ranges))
            return 0;

        std::vector<uint8_t> bytes;
        std::vector<bool>    mask;
        ParsePattern(pattern, bytes, mask);
        const size_t n = bytes.size();
        if (n == 0)
            return 0;
        if (cap < 1) cap = 1;

        int found = 0;
        for (const ScanRange& r : ranges)
        {
            if (r.size < n) continue;
            for (size_t i = 0; i + n <= r.size; ++i)
            {
                bool ok = true;
                for (size_t j = 0; j < n; ++j)
                    if (mask[j] && r.base[i + j] != bytes[j]) { ok = false; break; }
                if (ok && ++found >= cap)
                    return found; // bounded: only distinguish 0 / 1 / >=cap
            }
        }
        return found;
    }

    uint8_t* FindUnique(const char* pattern, int* outCount, Scope scope)
    {
        std::vector<ScanRange> ranges;
        if (!GetRanges(scope, ranges))
        {
            if (outCount) *outCount = 0;
            return nullptr;
        }

        std::vector<uint8_t> bytes;
        std::vector<bool>    mask;
        ParsePattern(pattern, bytes, mask);
        const size_t n = bytes.size();
        if (n == 0)
        {
            if (outCount) *outCount = 0;
            return nullptr;
        }

        uint8_t* first = nullptr;
        int      count = 0;
        for (const ScanRange& r : ranges)
        {
            if (r.size < n) continue;
            for (size_t i = 0; i + n <= r.size; ++i)
            {
                bool ok = true;
                for (size_t j = 0; j < n; ++j)
                    if (mask[j] && r.base[i + j] != bytes[j]) { ok = false; break; }
                if (ok)
                {
                    if (!first) first = r.base + i;
                    if (++count >= 2) { if (outCount) *outCount = count; return nullptr; }
                }
            }
        }
        if (outCount) *outCount = count;
        return (count == 1) ? first : nullptr;
    }

    uint8_t* ResolveRipRel(uint8_t* match, int offsetToRel32, int instructionLength)
    {
        if (!match)
            return nullptr;
        int32_t rel = *reinterpret_cast<int32_t*>(match + offsetToRel32);
        return match + instructionLength + rel;
    }

    bool IsExecutableAddress(const void* address, size_t size)
    {
        if (!address || size == 0)
            return false;
        const uintptr_t start = reinterpret_cast<uintptr_t>(address);
        if (start + size < start)
            return false;
        std::vector<ScanRange> ranges;
        if (!GetExecRanges(ranges))
            return false;
        for (const ScanRange& r : ranges)
        {
            const uintptr_t rb = reinterpret_cast<uintptr_t>(r.base);
            const uintptr_t re = rb + r.size;
            if (start >= rb && start + size <= re)
                return true;
        }
        return false;
    }

    uint8_t* DecodeRel32CallTarget(uint8_t* call)
    {
        if (!IsExecutableAddress(call, 5) || *call != 0xE8)
            return nullptr;
        int32_t rel = *reinterpret_cast<int32_t*>(call + 1);
        uint8_t* target = call + 5 + rel;
        return IsExecutableAddress(target, 1) ? target : nullptr;
    }

    uint8_t* DecodeRel32JumpTarget(uint8_t* jump)
    {
        if (!IsExecutableAddress(jump, 5) || *jump != 0xE9)
            return nullptr;
        int32_t rel = *reinterpret_cast<int32_t*>(jump + 1);
        uint8_t* target = jump + 5 + rel;
        return IsExecutableAddress(target, 1) ? target : nullptr;
    }

    uint8_t* FindRipRel(const char* pattern, int offsetToRel32, int instructionLength,
                        Scope scope)
    {
        uint8_t* match = Find(pattern, scope);
        return match ? ResolveRipRel(match, offsetToRel32, instructionLength) : nullptr;
    }
}
