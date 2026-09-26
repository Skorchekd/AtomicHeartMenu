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
#include <Windows.h>
#include <mutex>

// Subclasses the game window so ImGui sees raw input and the INSERT key toggles
// the menu. Installed once we know the swapchain's HWND.
namespace WndProcHook
{
    void Install(HWND hwnd);
    bool IsInstalled();
    void Tick();
    // False when our procedure had to stay installed (see IsTopLevel); the module
    // must then stay loaded.
    bool Remove();

    // True when our procedure is still the window's top-level one. Another overlay
    // that subclassed after us keeps calling into ours, so it cannot be unhooked
    // (and this DLL cannot be unloaded) without breaking its chain.
    bool IsTopLevel();

    // ImGui is single-threaded. Its input queue is appended by the window procedure
    // (the game thread) and consumed by NewFrame (the render thread); both sides
    // hold this lock across those calls. Recursive because ReleaseCapture inside the
    // handler re-enters the window procedure synchronously on the same thread.
    std::recursive_mutex& InputMutex();
}
