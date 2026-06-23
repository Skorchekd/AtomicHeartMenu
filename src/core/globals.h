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

// Process-wide state shared between the injector thread, the render hook and the
// feature loop.
namespace G
{
    inline std::atomic<bool> running{ true };   // cleared on eject to unwind hooks
    inline std::atomic<bool> menuOpen{ false }; // toggled with INSERT
    inline std::atomic<bool> sdkReady{ false }; // engine globals resolved
    inline std::atomic<uint64_t> overlayLastDrawMs{ 0 }; // menu input block only while recently visible

    inline void*    hModule   = nullptr;        // our injected DLL base
    inline uint8_t* moduleBase = nullptr;       // game exe base (0x140000000)
    inline size_t   moduleSize = 0;
    inline void*    hGameWindow = nullptr;       // HWND of the game
}
