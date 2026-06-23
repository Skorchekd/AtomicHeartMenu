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
    uint8_t* Find(const char* pattern)
    {
        uint8_t* base = nullptr;
        size_t   size = 0;
        if (!GetMainModuleRange(base, size))
            return nullptr;

        std::vector<uint8_t> bytes;
        std::vector<bool>    mask;
        ParsePattern(pattern, bytes, mask);
        const size_t n = bytes.size();
        if (n == 0 || size < n)
            return nullptr;

        for (size_t i = 0; i + n <= size; ++i)
        {
            bool ok = true;
            for (size_t j = 0; j < n; ++j)
            {
                if (mask[j] && base[i + j] != bytes[j]) { ok = false; break; }
            }
            if (ok)
                return base + i;
        }
        return nullptr;
    }

    uint8_t* ResolveRipRel(uint8_t* match, int offsetToRel32, int instructionLength)
    {
        if (!match)
            return nullptr;
        int32_t rel = *reinterpret_cast<int32_t*>(match + offsetToRel32);
        return match + instructionLength + rel;
    }

    uint8_t* FindRipRel(const char* pattern, int offsetToRel32, int instructionLength)
    {
        uint8_t* match = Find(pattern);
        return match ? ResolveRipRel(match, offsetToRel32, instructionLength) : nullptr;
    }
}
