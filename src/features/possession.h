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
#include "../sdk/ue4.h"
#include <string>
#include <vector>

// Playing as another character. The player controller possesses an AI character
// (a robot, a boss, a companion); the menu supplies its movement, mouse look and
// camera, since AI characters bind no player input, and fires its own abilities
// from hotkeys. The player's own character stays where it was, invulnerable, and
// the game does not save until control returns to it. Experimental: some
// characters' abilities only work while their own AI is in charge.
namespace Possession
{
    struct Settings
    {
        bool  thirdPerson     = true;  // camera behind the character; off = through its eyes
        float cameraDistanceM = 4.5f;
        float cameraHeightM   = 1.2f;
        float lookSensitivity = 1.0f;
        bool  invertY         = false;
        bool  invulnerable    = true;  // the character being played takes no damage
    };

    struct Status
    {
        bool active = false;
        bool busy = false;
        std::string character;              // what is being played
        float healthFrac = -1.0f;
        std::vector<std::string> abilities; // index + 1 = number key
        std::string message = "Take control of the character under your crosshair, or pick one from the list.";
    };

    Settings& Config();
    Status GetStatus();
    bool IsActive();
    bool IsControlled(UE::UObject* actor);   // the character being played right now

    void BeginAimed();                       // the character under the crosshair
    void BeginListed(unsigned long long id); // a character from the AI list
    void End();                              // back to your own character
    void Toggle();                           // hotkey: take the crosshair character, or go back
    void UseAbility(int index);              // fire the played character's ability

    void Tick();                             // render thread, every frame
    void NoteMouseDelta(long dx, long dy);   // window procedure: raw mouse motion
    void CleanupGameThread();                // eject: back to your own character (game thread)
}
