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
#include <cstddef>
#include <cstdint>

// Pointer-safety helpers. The whole point: with wrong offsets we must DEGRADE
// (show "SDK not resolved") instead of dereferencing garbage and crashing the
// game. Every read of game memory should be gated on these.
namespace Mem
{
    // True if [p, p+size) is committed and readable in this process.
    bool IsReadable(const void* p, size_t size = sizeof(void*));

    // Heuristic: looks like a usable heap/image pointer (aligned, readable,
    // not in the null page or obviously bogus high range).
    bool LooksLikePtr(const void* p);
}
