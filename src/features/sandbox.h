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
#include <string>
#include <vector>

// Sandbox: load any of the game's maps directly, with no save involved. The map
// list comes from the game's own asset registry and a map is opened with
// GameplayStatics.OpenLevel, the engine's plain level load. From the load until
// the title menu the game's saves are swallowed, so neither the sandbox nor a
// level or save the game moves on to from it can overwrite campaign progress.
namespace Sandbox
{
    struct MapEntry
    {
        std::string package; // "/Game/Maps/..." as the asset registry names it
        bool likelyMain = false; // looks like a persistent (top-level) map
    };

    struct Status
    {
        bool busy = false;
        bool listed = false;        // the asset registry has been read
        int  mapCount = 0;
        bool loading = false;       // a requested map has not arrived yet
        bool sandboxActive = false; // the running map was loaded by the sandbox
        bool holdingSaves = false;  // the sandbox's save block is up (until the title menu)
        bool savesBlocked = false;  // any owner of the save guard
        std::string currentMap;
        std::string message = "Read the game's map list, pick a map and load it. No save is needed or touched.";
    };

    Status GetStatus();
    std::vector<MapEntry> Maps(const char* filter, int maxCount, bool mainOnly);
    void RefreshMaps();                          // read the map list (game thread, queued)
    void OpenMap(const std::string& package);    // load it; saves stay blocked until the title menu
    void OpenMapByName(const std::string& name); // a typed package path, for when the list is unavailable
    void SetBlockSaves(bool on);                 // default on; off lifts a block that is up
    bool BlockSaves();
    void Tick();                                 // render thread: follows world changes
}
