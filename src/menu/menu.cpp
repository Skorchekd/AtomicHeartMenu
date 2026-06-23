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
#include "menu.h"
#include "../core/exception_guard.h"
#include "../core/globals.h"
#include "../core/log.h"
#include "../features/features.h"
#include "../sdk/ue4.h"
#include "imgui.h"
#include <Windows.h>
#include <cfloat>
#include <vector>
#include <string>

namespace
{
    // ---- Theme: clean dark "liquid" look with a single cyan accent -----------
    void ApplyTheme()
    {
        ImGuiStyle& s = ImGui::GetStyle();
        s.WindowRounding    = 9.0f;
        s.ChildRounding     = 8.0f;
        s.FrameRounding     = 6.0f;
        s.PopupRounding     = 6.0f;
        s.GrabRounding      = 6.0f;
        s.TabRounding       = 7.0f;
        s.ScrollbarRounding = 9.0f;
        s.WindowPadding     = ImVec2(14, 12);
        s.FramePadding      = ImVec2(11, 6);
        s.CellPadding       = ImVec2(8, 6);
        s.ItemSpacing       = ImVec2(10, 9);
        s.ItemInnerSpacing  = ImVec2(8, 6);
        s.ScrollbarSize     = 12.0f;
        s.GrabMinSize       = 13.0f;
        s.WindowBorderSize  = 0.0f;
        s.FrameBorderSize   = 0.0f;
        s.ChildBorderSize   = 1.0f;
        s.WindowTitleAlign  = ImVec2(0.5f, 0.5f);

        const ImVec4 accent  = ImVec4(0.16f, 0.80f, 0.96f, 1.00f); // cyan
        const ImVec4 accentD = ImVec4(0.13f, 0.45f, 0.58f, 1.00f);
        const ImVec4 bg      = ImVec4(0.065f, 0.075f, 0.095f, 0.97f);
        const ImVec4 panel   = ImVec4(0.115f, 0.130f, 0.160f, 1.00f);
        const ImVec4 panelHi = ImVec4(0.170f, 0.190f, 0.230f, 1.00f);

        ImVec4* c = s.Colors;
        c[ImGuiCol_WindowBg]            = bg;
        c[ImGuiCol_ChildBg]             = ImVec4(0.090f, 0.100f, 0.125f, 0.55f);
        c[ImGuiCol_PopupBg]             = ImVec4(0.075f, 0.085f, 0.110f, 0.98f);
        c[ImGuiCol_Border]              = ImVec4(0.22f, 0.25f, 0.30f, 0.45f);
        c[ImGuiCol_FrameBg]             = panel;
        c[ImGuiCol_FrameBgHovered]      = panelHi;
        c[ImGuiCol_FrameBgActive]       = panelHi;
        c[ImGuiCol_TitleBg]             = ImVec4(0.055f, 0.065f, 0.085f, 1.0f);
        c[ImGuiCol_TitleBgActive]       = ImVec4(0.085f, 0.110f, 0.140f, 1.0f);
        c[ImGuiCol_MenuBarBg]           = panel;
        c[ImGuiCol_ScrollbarBg]         = ImVec4(0, 0, 0, 0);
        c[ImGuiCol_ScrollbarGrab]       = accentD;
        c[ImGuiCol_ScrollbarGrabHovered]= accent;
        c[ImGuiCol_ScrollbarGrabActive] = accent;
        c[ImGuiCol_CheckMark]           = accent;
        c[ImGuiCol_SliderGrab]          = accent;
        c[ImGuiCol_SliderGrabActive]    = ImVec4(0.35f, 0.92f, 1.00f, 1.0f);
        c[ImGuiCol_Button]              = panel;
        c[ImGuiCol_ButtonHovered]       = accentD;
        c[ImGuiCol_ButtonActive]        = accent;
        c[ImGuiCol_Header]              = ImVec4(0.16f, 0.19f, 0.24f, 1.0f);
        c[ImGuiCol_HeaderHovered]       = accentD;
        c[ImGuiCol_HeaderActive]        = accentD;
        c[ImGuiCol_Separator]           = ImVec4(0.22f, 0.25f, 0.30f, 0.50f);
        c[ImGuiCol_SeparatorHovered]    = accent;
        c[ImGuiCol_SeparatorActive]     = accent;
        c[ImGuiCol_Tab]                 = ImVec4(0.10f, 0.12f, 0.15f, 1.0f);
        c[ImGuiCol_TabHovered]          = accentD;
        c[ImGuiCol_TabSelected]         = ImVec4(0.15f, 0.30f, 0.38f, 1.0f);
        c[ImGuiCol_TabDimmed]           = ImVec4(0.10f, 0.12f, 0.15f, 1.0f);
        c[ImGuiCol_TabDimmedSelected]   = ImVec4(0.12f, 0.16f, 0.20f, 1.0f);
        c[ImGuiCol_Text]                = ImVec4(0.91f, 0.93f, 0.96f, 1.0f);
        c[ImGuiCol_TextDisabled]        = ImVec4(0.46f, 0.50f, 0.57f, 1.0f);
        c[ImGuiCol_PlotHistogram]       = accent;
        c[ImGuiCol_PlotHistogramHovered]= ImVec4(0.35f, 0.92f, 1.00f, 1.0f);
    }

    const ImVec4 kAccent = ImVec4(0.16f, 0.80f, 0.96f, 1.00f);

    bool LogCheckbox(const char* label, bool* value, const char* key)
    {
        if (!ImGui::Checkbox(label, value))
            return false;

        LOG("UI: %s -> %s", key, *value ? "ON" : "OFF");
        return true;
    }

    // A full-width accent button (primary action).
    bool AccentButton(const char* label, float height = 0.0f)
    {
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.13f, 0.50f, 0.62f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.18f, 0.66f, 0.80f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  kAccent);
        bool pressed = ImGui::Button(label, ImVec2(-FLT_MIN, height));
        ImGui::PopStyleColor(3);
        return pressed;
    }

    // Start a titled "card" panel. End with ImGui::EndChild().
    void BeginCard(const char* id, float height)
    {
        ImGui::BeginChild(id, ImVec2(0, height), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY);
    }

    volatile LONG g_dumpRunning = 0;

    DWORD WINAPI DumpObjectsThread(LPVOID)
    {
        try
        {
            LOG("Object dump started");
            int n = UE::NumObjects();
            for (int i = 0; i < n && i < 200; ++i)
            {
                if (auto* o = UE::GetObjectByIndex(i))
                    LOG("[%d] %s", i, o->GetFullName().c_str());
            }
            UE::WriteObjectMapDump("manual-debug-button");
            LOG("Object dump complete");
        }
        catch (...) { LOG("Dump: exception (ignored)"); }
        InterlockedExchange(&g_dumpRunning, 0);
        return 0;
    }

    void StartObjectDump()
    {
        if (InterlockedCompareExchange(&g_dumpRunning, 1, 0) != 0)
        {
            LOG("Object dump already running");
            return;
        }

        HANDLE thread = CreateThread(nullptr, 0, DumpObjectsThread, nullptr, 0, nullptr);
        if (thread)
        {
            CloseHandle(thread);
        }
        else
        {
            InterlockedExchange(&g_dumpRunning, 0);
            LOG("Object dump thread create failed err=%lu", GetLastError());
        }
    }

    // ===================================================================
    //  AI / SQUAD tab -- the headline surface: full control over the AI.
    // ===================================================================
    void DrawAiTab(Features::State& f)
    {
        // --- status strip ---------------------------------------------------
        ImGui::TextColored(kAccent, "Enemies: %d", Features::AiCachedCount());
        ImGui::SameLine(0, 16); ImGui::Text("Squad: %d", Features::AiSquadCount());
        ImGui::SameLine(0, 16); ImGui::Text("Selected: %d", Features::AiSelectedCount());
        int stream = Features::AiSpawnQueueCount();
        if (stream > 0) { ImGui::SameLine(0, 16); ImGui::TextColored(kAccent, "Spawning: %d", stream); }
        ImGui::SetNextItemWidth(-110.0f);
        ImGui::SliderFloat("Radius (m)", &f.aiRadius, 10.0f, 300.0f, "%.0f");

        ImGui::Spacing();

        // --- ROSTER: nearby AI, select + recruit + dispatch -----------------
        ImGui::SeparatorText("AI control  (select -> highlighted in-world)");
        BeginCard("rostercard", 0);
        {
            float third = (ImGui::GetContentRegionAvail().x - 2 * ImGui::GetStyle().ItemSpacing.x) / 3.0f;
            if (ImGui::Button("Select all", ImVec2(third, 0))) Features::AiSelectAllNearby();
            ImGui::SameLine();
            if (ImGui::Button("Clear sel.", ImVec2(third, 0))) Features::AiClearSelection();
            ImGui::SameLine();
            if (AccentButton("Recruit sel.")) Features::AiRecruitSelected();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Recruited AI join your SQUAD: they walk-follow you and fight your\n"
                                  "threats. This is the explicit recruit -- no auto-recruit toggle.");

            if (ImGui::Button("Recruit all nearby", ImVec2(third, 0))) Features::AiRecruitNearby();
            ImGui::SameLine();
            if (ImGui::Button("Attack >", ImVec2(third, 0))) Features::AiDispatchAttack();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Send the selected units (or whole squad) at the nearest enemy.");
            ImGui::SameLine();
            if (ImGui::Button("Kill sel.", ImVec2(-FLT_MIN, 0))) Features::AiDispatchKill();

            if (ImGui::Button("Save selected to DB", ImVec2(-FLT_MIN, 0))) Features::AiSaveSelected();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Adds the selected units' character types to your saved model library\n"
                                  "(below) so you can re-spawn them anywhere, anytime.");

            ImGui::Spacing();
            // scrollable nearby list with per-row select toggles
            ImGui::BeginChild("ailist", ImVec2(0, 150), ImGuiChildFlags_Borders);
            std::vector<Features::AiListEntry> list = Features::AiNearbyList(40);
            if (list.empty())
                ImGui::TextDisabled("No AI nearby (move closer to enemies).");
            for (const Features::AiListEntry& e : list)
            {
                ImGui::PushID((int)(e.id & 0xFFFFFFFF));
                bool sel = e.selected;
                if (ImGui::Checkbox("##sel", &sel))
                    Features::AiToggleSelect(e.id);
                ImGui::SameLine();
                // Delete = remove the actor from the game outright (K2_DestroyActor).
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.12f, 0.12f, 1.0f));
                if (ImGui::SmallButton("Del")) Features::AiDeleteActor(e.id);
                ImGui::PopStyleColor();
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Delete this actor from the world (gone, not just killed).");
                ImGui::SameLine();
                ImVec4 col = e.inSquad ? ImVec4(0.4f, 0.95f, 0.55f, 1.0f)
                                       : ImVec4(0.85f, 0.87f, 0.9f, 1.0f);
                ImGui::TextColored(col, "%-20s %4.0fm%s", e.name.c_str(), e.distanceM,
                                   e.inSquad ? "  [squad]" : "");
                ImGui::PopID();
            }
            ImGui::EndChild();

            if (ImGui::Button("Stand down squad", ImVec2(third, 0)))      Features::AiReleaseSquad();
            ImGui::SameLine();
            if (ImGui::Button("Release selected", ImVec2(-FLT_MIN, 0)))   Features::AiReleaseSelected();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Turns them back into normal, KILLABLE enemies (forced hostile to\n"
                                  "you through the engine -- no more 'stuck invincible after release').");
        }
        ImGui::EndChild();

        // --- SPAWN ----------------------------------------------------------
        ImGui::SeparatorText("Spawn ally / boss");
        BeginCard("spawncard", 0);
        {
            int modelCount = Features::AiSpawnModelCount();
            if (f.aiSpawnModel < 0 || f.aiSpawnModel >= modelCount) f.aiSpawnModel = 0;
            const char* preview = modelCount > 0
                ? Features::AiSpawnModelName(f.aiSpawnModel)
                : "(no models loaded yet)";

            ImGui::TextDisabled("Model (live, loaded enemy/boss types)");
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::BeginCombo("##model", preview))
            {
                for (int i = 0; i < modelCount; ++i)
                {
                    bool sel = (f.aiSpawnModel == i);
                    if (ImGui::Selectable(Features::AiSpawnModelName(i), sel))
                        f.aiSpawnModel = i;
                    if (sel) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            if (AccentButton(modelCount > 0 ? "Spawn selected model" : "Spawn (no models yet)"))
            {
                bool ok = Features::AiSpawnModel(f.aiSpawnModel);
                LOG("UI: spawn model %d -> %s", f.aiSpawnModel, ok ? "queued" : "failed");
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Spawns the chosen LIVE class (incl. bosses like the Twins if loaded)\n"
                                  "as a squad member. STREAMED one-at-a-time so it never freezes --\n"
                                  "press a few times for a squad.");
            if (ImGui::Button("Clone nearest enemy", ImVec2(-FLT_MIN, 0)))
                Features::AiSpawnBodyguard();

            // --- search + spawn ANY model in the whole game + DLC ----------
            ImGui::Spacing();
            ImGui::TextDisabled("Search ANY model (whole game + DLC, incl. bosses)");
            static char modelSearch[64] = "";
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::InputTextWithHint("##modelsearch", "type to filter: twin, robot, larisa, mutant...",
                                     modelSearch, sizeof(modelSearch));
            std::vector<std::string> results = Features::AiSearchModels(modelSearch, 200);
            ImGui::BeginChild("modellist", ImVec2(0, 160), ImGuiChildFlags_Borders);
            if (results.empty())
                ImGui::TextDisabled("Loading full asset list (first open takes a second)... then type to filter.");
            for (int i = 0; i < (int)results.size(); ++i)
            {
                ImGui::PushID(5000 + i);
                // Fixed-width Spawn button so the model NAME stays visible beside it
                // (AccentButton fills the whole row -> name was hidden).
                ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.13f, 0.50f, 0.62f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.18f, 0.66f, 0.80f, 1.0f));
                bool go = ImGui::Button("Spawn", ImVec2(64, 0));
                ImGui::PopStyleColor(2);
                if (go) Features::AiSpawnModelByName(results[i].c_str());
                ImGui::SameLine();
                ImGui::TextUnformatted(results[i].c_str());
                ImGui::PopID();
            }
            ImGui::EndChild();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Every character TYPE currently loaded -- spawns it as a bodyguard.\n"
                                  "Combat vs non-combat is auto-detected (robots fight; civilians/Larisa\n"
                                  "just follow), so it never crashes on a peaceful model.");
        }
        ImGui::EndChild();

        // --- saved characters (spawn anywhere, no proximity) ----------------
        ImGui::SeparatorText("Saved characters  (spawn anywhere)");
        BeginCard("savedcard", 0);
        {
            static char saveName[64] = "";
            ImGui::SetNextItemWidth(-96.0f);
            ImGui::InputTextWithHint("##bgname", "name (optional)", saveName, sizeof(saveName));
            ImGui::SameLine();
            if (ImGui::Button("Save##nearest", ImVec2(-FLT_MIN, 0)))
            {
                if (Features::AiSaveNearestCharacter(saveName)) saveName[0] = '\0';
            }
            std::vector<std::string> names = Features::AiSavedCharacterNames();
            if (names.empty())
                ImGui::TextDisabled("None yet. Stand near an enemy and Save.");
            for (int i = 0; i < (int)names.size(); ++i)
            {
                ImGui::PushID(1000 + i);
                if (ImGui::Button("Spawn")) Features::AiSpawnSavedCharacter(i);
                ImGui::SameLine();
                if (ImGui::Button("X")) { Features::AiDeleteSavedCharacter(i); ImGui::PopID(); break; }
                ImGui::SameLine();
                ImGui::TextUnformatted(names[i].c_str());
                ImGui::PopID();
            }
            ImGui::TextDisabled("Loads the type on demand -- no need to be near it.");
        }
        ImGui::EndChild();

        // --- settings + crowd control ---------------------------------------
        ImGui::SeparatorText("Settings");
        BeginCard("setcard", 0);
        {
            LogCheckbox("Invincible squad (keep them alive)", &f.aiInvincibleAllies, "aiInvincibleAllies");
            LogCheckbox("Squad fights for you (obliterate threats)", &f.aiSquadAggressive, "aiSquadAggressive");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("ON (default): combat-capable guards force-attack nearby enemies with\n"
                                  "massively boosted damage. Non-combat NPCs (Larisa, civilians) are\n"
                                  "auto-detected and just follow -- no crash. Your guards NEVER attack\n"
                                  "you, even if you shoot them. OFF: peaceful followers (no fighting).");
            ImGui::SetNextItemWidth(-140.0f);
            ImGui::SliderFloat("Follow stop dist (m)", &f.aiFollowStopM, 0.5f, 8.0f, "%.1f");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("How close squad members stop behind you. They path-follow smoothly\n"
                                  "and stop cleanly at this distance (no circling). 1.0-1.5 = tight.");
            LogCheckbox("Allow teleport (only if a member gets stuck)", &f.aiAllowTeleport, "aiAllowTeleport");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Off (default): squad members WALK to you with real locomotion.\n"
                                  "On: if one can't path (nav hole) and stops making progress, it snaps.");
            LogCheckbox("Enemies fight each other", &f.aiFightEachOther, "aiFightEachOther");
            LogCheckbox("Freeze nearby AI", &f.aiFreezeNearby, "aiFreezeNearby");

            ImGui::Spacing();
            ImGui::TextDisabled("Zone respawn:");
            float half = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) / 2.0f;
            if (ImGui::Button("Snapshot zone", ImVec2(half, 0))) Features::AiSnapshotZone();
            ImGui::SameLine();
            if (ImGui::Button("Respawn zone", ImVec2(-FLT_MIN, 0))) Features::AiRespawnZone();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Snapshot records the current enemies; Respawn brings that whole set\n"
                                  "back (as your allies) -- e.g. after you've cleared the area.");
            ImGui::Text("Snapshot: %d type(s)", Features::AiZoneSnapshotCount());

            ImGui::Spacing();
            ImGui::TextDisabled("Whole level:");
            if (ImGui::Button("KILL ALL", ImVec2(half, 0)))     Features::AiQueueKillAll();
            ImGui::SameLine();
            if (ImGui::Button("LAUNCH ALL", ImVec2(-FLT_MIN, 0))) Features::AiQueueLaunchAll();
            if (ImGui::Button("KILL ALL (deep sweep)", ImVec2(-FLT_MIN, 0))) Features::AiKillAllDeep();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Sweeps EVERY object for live enemies -- catches the rare attacker\n"
                                  "the normal scan misses (the 'unkillable, still attacking' one that\n"
                                  "regular Kill All can't reach). Use this if an enemy won't die.");
        }
        ImGui::EndChild();
    }
}

void Menu::Render()
{
    auto& f = Features::Get();

    static bool themed = false;
    if (!themed) { ApplyTheme(); themed = true; }

    // --- always-on overlay (coords / status), independent of the window -----
    if (f.showCoords && G::sdkReady.load())
    {
        ImGui::SetNextWindowBgAlpha(0.35f);
        ImGui::SetNextWindowPos(ImVec2(12, 12));
        ImGui::Begin("##coords", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
            ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove);
        auto loc = Features::LastLocation();
        ImGui::Text("X %.0f  Y %.0f  Z %.0f", loc.X, loc.Y, loc.Z);
        ImGui::End();
    }

    if (!G::menuOpen.load()) return;

    ImGui::SetNextWindowSize(ImVec2(560, 640), ImGuiCond_FirstUseEver);
    ImGui::Begin("ATOMIC  -  internal menu   [INSERT]", nullptr, ImGuiWindowFlags_NoCollapse);

    // SDK status pill
    if (G::sdkReady.load())
        ImGui::TextColored(ImVec4(0.40f, 0.95f, 0.55f, 1.0f), "* SDK resolved   (%d objects)", UE::NumObjects());
    else
        ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.40f, 1.0f), "* SDK NOT resolved  -  check offsets.h");
    ImGui::Separator();

    if (ImGui::BeginTabBar("tabs", ImGuiTabBarFlags_FittingPolicyScroll))
    {
        if (ImGui::BeginTabItem("Player"))
        {
            ImGui::SeparatorText("Survival");
            LogCheckbox("God mode (zero incoming damage)", &f.godMode, "godMode");
            LogCheckbox("Infinite stamina", &f.infiniteStamina, "infiniteStamina");
            LogCheckbox("Infinite energy (abilities)", &f.infiniteEnergy, "infiniteEnergy");
            LogCheckbox("Infinite air (no drowning)", &f.infiniteAir, "infiniteAir");
            if (ImGui::Button("Heal to full now"))
            {
                LOG("UI: heal to full");
                Features::FullHeal();
            }

            ImGui::SeparatorText("Movement");
            LogCheckbox("Fly", &f.flyHack, "flyHack");
            LogCheckbox("Noclip (fly through walls)", &f.noclip, "noclip");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Disables the player's collision and free-flies with\n"
                                  "W/A/S/D, Space (up), Shift (down). Movement x sets speed.");
            LogCheckbox("Fly streaming assist", &f.flyStreamingAssist, "flyStreamingAssist");
            if (f.hasFlyStart && ImGui::Button("Return to fly start"))
                Features::ReturnToFlyStart();
            LogCheckbox("Speed hack", &f.speedHack, "speedHack");
            ImGui::SetNextItemWidth(-110.0f);
            ImGui::SliderFloat("Movement x", &f.speedMult, 1.0f, 10.0f, "%.1f");
            if (ImGui::IsItemDeactivatedAfterEdit())
                LOG("UI: speedMult -> %.1f", f.speedMult);

            ImGui::SeparatorText("Body");
            LogCheckbox("Custom scale (giant / tiny)", &f.customScale, "customScale");
            ImGui::SetNextItemWidth(-110.0f);
            ImGui::SliderFloat("Scale", &f.playerScale, 0.2f, 8.0f, "%.2f");
            if (ImGui::IsItemDeactivatedAfterEdit())
                LOG("UI: playerScale -> %.2f", f.playerScale);
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Weapons"))
        {
            ImGui::SeparatorText("Give weapon");
            static int selectedWeapon = 0;
            const Features::WeaponEntry* weapons = Features::WeaponList();
            int weaponCount = Features::WeaponCount();
            if (weaponCount > 0)
            {
                if (selectedWeapon < 0 || selectedWeapon >= weaponCount)
                    selectedWeapon = 0;

                ImGui::SetNextItemWidth(-FLT_MIN);
                if (ImGui::BeginCombo("##weapon", weapons[selectedWeapon].label))
                {
                    for (int i = 0; i < weaponCount; ++i)
                    {
                        bool selected = selectedWeapon == i;
                        if (ImGui::Selectable(weapons[i].label, selected))
                        {
                            selectedWeapon = i;
                            LOG("UI: selectedWeapon -> %s", weapons[i].label);
                        }
                        if (selected)
                            ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }

                float half = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) / 2.0f;
                if (ImGui::Button("Give selected", ImVec2(half, 0)))
                {
                    LOG("UI: give selected weapon -> %s", weapons[selectedWeapon].label);
                    Features::GiveWeapon(selectedWeapon, true);
                }
                ImGui::SameLine();
                if (ImGui::Button("Give all", ImVec2(-FLT_MIN, 0)))
                {
                    LOG("UI: give all weapons");
                    Features::GiveAllWeapons(false);
                }
            }

            ImGui::SeparatorText("Combat");
            LogCheckbox("One-hit kill (massive outgoing damage)", &f.oneHitKill, "oneHitKill");
            LogCheckbox("Infinite ammo (reserve + magazine)", &f.infiniteAmmo, "infiniteAmmo");
            if (ImGui::Button("Refill ammo now"))
            {
                LOG("UI: refill ammo now");
                Features::RefillAmmoNow();
            }

            ImGui::SeparatorText("Upgrades");
            if (ImGui::Button("Max weapon upgrades (current)"))
            {
                LOG("UI: max weapon upgrades");
                Features::MaxWeaponUpgrades();
            }
            ImGui::TextDisabled("Calls BaseWeapon.FullUpgrade on the equipped weapon.\nSwitch weapon and press again to max each.");
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("AI / Squad"))
        {
            DrawAiTab(f);
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("World"))
        {
            LogCheckbox("Show coordinates", &f.showCoords, "showCoords");
            float half = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) / 2.0f;
            if (ImGui::Button("Save position", ImVec2(half, 0))) Features::SavePosition();
            ImGui::SameLine();
            if (ImGui::Button("Teleport to saved", ImVec2(-FLT_MIN, 0)))
            {
                LOG("UI: teleport button pressed");
                Features::TeleportToSaved();
            }
            if (f.hasSaved)
                ImGui::TextDisabled("Saved: %.0f %.0f %.0f", f.savedLocation.X, f.savedLocation.Y, f.savedLocation.Z);

            ImGui::SeparatorText("Time");
            LogCheckbox("Bullet time (matrix mode)", &f.bulletTime, "bulletTime");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Slows the whole world but keeps YOU at full speed -- you move\n"
                                  "many times faster than everything else.");
            ImGui::SetNextItemWidth(-110.0f);
            ImGui::SliderFloat("World speed", &f.bulletTimeScale, 0.05f, 1.0f, "%.2f");
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Visuals"))
        {
            ImGui::SeparatorText("Camera");
            LogCheckbox("Custom FOV", &f.customFov, "customFov");
            ImGui::SetNextItemWidth(-110.0f);
            ImGui::SliderFloat("FOV", &f.fovValue, 60.0f, 170.0f, "%.0f");
            if (ImGui::IsItemDeactivatedAfterEdit())
                LOG("UI: fovValue -> %.0f", f.fovValue);

            ImGui::SeparatorText("ESP");
            LogCheckbox("Enemy ESP", &f.espEnabled, "espEnabled");
            LogCheckbox("Box", &f.espBox, "espBox");
            ImGui::SameLine();
            LogCheckbox("Corners", &f.espCornerBox, "espCornerBox");
            LogCheckbox("Filled chams", &f.espFilled, "espFilled");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(110.0f);
            ImGui::SliderFloat("Fill", &f.espFillAlpha, 0.0f, 1.0f, "%.2f");
            LogCheckbox("Snapline", &f.espSnapline, "espSnapline");
            ImGui::SameLine();
            LogCheckbox("Health", &f.espHealthbar, "espHealthbar");
            LogCheckbox("Distance", &f.espDistance, "espDistance");
            ImGui::SetNextItemWidth(-110.0f);
            ImGui::SliderFloat("ESP range (m)", &f.espMaxDistance, 25.0f, 600.0f, "%.0f");
            LogCheckbox("Rainbow", &f.espRainbow, "espRainbow");
            if (!f.espRainbow)
                ImGui::ColorEdit3("ESP color", f.espColor, ImGuiColorEditFlags_NoInputs);

            ImGui::SeparatorText("Crosshair");
            LogCheckbox("Crosshair", &f.crosshair, "crosshair");
            if (!f.espRainbow)
                ImGui::ColorEdit3("Crosshair color", f.crosshairColor, ImGuiColorEditFlags_NoInputs);

            ImGui::SeparatorText("Chams (recolor enemy models)");
            LogCheckbox("Chams", &f.chamsEnabled, "chamsEnabled");
            ImGui::SameLine();
            LogCheckbox("Rainbow##chams", &f.chamsRainbow, "chamsRainbow");
            if (!f.chamsRainbow)
                ImGui::ColorEdit3("Chams color", f.chamsColor, ImGuiColorEditFlags_NoInputs);
            ImGui::SetNextItemWidth(-110.0f);
            ImGui::SliderFloat("Glow", &f.chamsEmissive, 0.0f, 20.0f, "%.1f");
            LogCheckbox("Through walls (custom depth)", &f.chamsThroughWalls, "chamsThroughWalls");

            ImGui::SeparatorText("Weapon");
            LogCheckbox("RGB gun (recolor equipped weapon)", &f.weaponRgb, "weaponRgb");
            ImGui::SameLine();
            LogCheckbox("Rainbow##wpn", &f.weaponRgbRainbow, "weaponRgbRainbow");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Drives RGB on dynamic instances of the equipped weapon materials.\n"
                                  "Texture-locked slots use a simple forced parent, and original\n"
                                  "materials are restored when disabled or when the weapon changes.");
            if (!f.weaponRgbRainbow)
                ImGui::ColorEdit3("Gun color", f.weaponRgbColor, ImGuiColorEditFlags_NoInputs);
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Render"))
        {
            ImGui::SeparatorText("World / sky color");
            LogCheckbox("Tint world lights (RGB sky)", &f.worldTint, "worldTint");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Recolors every light in the loaded level (the directional 'sun'\n"
                                  "included) so the whole scene + sky take your colour. The light list\n"
                                  "is cached and only re-applied when the colour visibly changes, so it\n"
                                  "no longer tanks frame times. Off resets lights to white.");
            ImGui::SameLine();
            LogCheckbox("Rainbow##world", &f.worldTintRainbow, "worldTintRainbow");
            if (!f.worldTintRainbow)
                ImGui::ColorEdit3("World color", f.worldTintColor, ImGuiColorEditFlags_NoInputs);
            ImGui::SetNextItemWidth(-110.0f);
            ImGui::SliderFloat("Cycle speed", &f.worldTintCycle, 0.05f, 2.0f, "%.2f");

            ImGui::SeparatorText("Console command");
            static char consoleBuf[256] = "";
            ImGui::SetNextItemWidth(-70.0f);
            bool enter = ImGui::InputText("##cmd", consoleBuf, sizeof(consoleBuf),
                                          ImGuiInputTextFlags_EnterReturnsTrue);
            ImGui::SameLine();
            if ((ImGui::Button("Run") || enter) && consoleBuf[0])
            {
                LOG("UI: console run -> %s", consoleBuf);
                Features::RunConsoleCommand(consoleBuf);
            }

            auto Cmd = [](const char* label, const char* command)
            {
                if (ImGui::Button(label))
                {
                    LOG("UI: console preset -> %s", command);
                    Features::RunConsoleCommand(command);
                }
            };

            ImGui::SeparatorText("Viewmodes");
            Cmd("Wireframe", "viewmode wireframe"); ImGui::SameLine();
            Cmd("Unlit",     "viewmode unlit");     ImGui::SameLine();
            Cmd("Lit",       "viewmode lit");       ImGui::SameLine();
            Cmd("Reflections", "viewmode reflections");

            ImGui::SeparatorText("Show flags");
            Cmd("Fog",        "show Fog");              ImGui::SameLine();
            Cmd("PostFX",     "show PostProcessing");   ImGui::SameLine();
            Cmd("Bloom",      "show Bloom");            ImGui::SameLine();
            Cmd("AO",         "show AmbientOcclusion");
            Cmd("Decals",     "show Decals");           ImGui::SameLine();
            Cmd("Particles",  "show Particles");        ImGui::SameLine();
            Cmd("Shadows",    "show DynamicShadows");

            ImGui::SeparatorText("Look tweaks");
            Cmd("Bloom x4",   "r.BloomQuality 5");      ImGui::SameLine();
            Cmd("Sharpen",    "r.Tonemapper.Sharpen 2");ImGui::SameLine();
            Cmd("Saturate+",  "r.Color.Max 1.5");
            Cmd("Super-res",  "r.ScreenPercentage 150");ImGui::SameLine();
            Cmd("Res 100%",   "r.ScreenPercentage 100");

            ImGui::SeparatorText("Utility");
            Cmd("Slomo 0.2",  "slomo 0.2");             ImGui::SameLine();
            Cmd("Slomo 1.0",  "slomo 1");               ImGui::SameLine();
            Cmd("Debug cam",  "ToggleDebugCamera");
            Cmd("Stat FPS",   "stat fps");              ImGui::SameLine();
            Cmd("Stat Unit",  "stat unit");             ImGui::SameLine();
            Cmd("Hi-res shot","HighResShot 2");
            ImGui::TextDisabled("Most 'show'/'viewmode' commands toggle -- press again to revert.");
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Misc"))
        {
            ImGui::SeparatorText("Puzzles");
            LogCheckbox("Instant puzzle resolve", &f.instantPuzzleResolve, "instantPuzzleResolve");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Toggles Atomic Heart's own SetInstantPuzzleResolve AND auto-completes\n"
                                  "interactive puzzles as you reach them (minigames + door locks).\n"
                                  "Discovery runs on the worker thread, so it never freezes the game.");
            if (ImGui::Button("Solve / pass current puzzle"))
            {
                LOG("UI: solve/pass current puzzle");
                Features::SolveCurrentPuzzle();
            }
            float half = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) / 2.0f;
            if (ImGui::Button("Unlock current lock", ImVec2(half, 0)))
            {
                LOG("UI: unlock current lock");
                Features::UnlockCurrentLock();
            }
            ImGui::SameLine();
            if (ImGui::Button("Win active QTE", ImVec2(-FLT_MIN, 0)))
            {
                LOG("UI: win active QTE");
                Features::WinCurrentQTE();
            }

            ImGui::SeparatorText("Mission objectives");
            if (ImGui::Button("Skip current objective", ImVec2(half, 0)))
            {
                LOG("UI: skip current objective");
                Features::SkipObjective();
            }
            ImGui::SameLine();
            if (ImGui::Button("Complete active quests", ImVec2(-FLT_MIN, 0)))
            {
                LOG("UI: complete active quests");
                Features::CompleteActiveQuests();
            }
            ImGui::TextDisabled("Drives the game's own quest debug -- skips objective gates / blockers.");
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Debug"))
        {
            ImGui::TextWrapped("Confirm the SDK is wired, then find classes/functions to cheat on.");
            ImGui::Spacing();
            if (G::sdkReady.load())
            {
                UE::UObject* world = UE::GetWorld();
                UE::UObject* pawn  = UE::GetLocalPawn();
                ImGui::Text("GWorld:   %p", (void*)world);
                ImGui::Text("Pawn:     %p  %s", (void*)pawn, pawn ? pawn->GetName().c_str() : "");
                ImGui::Text("PlayerController: %p", (void*)UE::GetPlayerController());
            }
            ImGui::Spacing();
            ImGui::TextDisabled("Dump writes first 200 to log plus full TSV beside the game exe.");
            if (ImGui::Button("Dump objects"))
            {
                LOG("UI: object dump requested");
                StartObjectDump();
            }
            ImGui::Spacing();
            ImGui::SeparatorText("Live diagnostics");
            ImGui::TextWrapped("Snapshots now BUILD on the game thread but WRITE on a worker thread "
                               "(disk + the 360k-object scan are off-thread), so saving no longer "
                               "freezes the game. Bundles include full named-field reflection of any "
                               "class -- not just ones we hard-coded.");
            if (ImGui::Button("Write full snapshot", ImVec2(-FLT_MIN, 0)))
            {
                LOG("UI: full diagnostic snapshot requested");
                Features::DebugDumpGameSnapshot();
            }
            LogCheckbox("Live diagnostic capture (togglelogsN)", &f.debugLiveDump, "debugLiveDump");
            if (const char* dir = Features::DebugLastDumpDir(); dir && *dir)
                ImGui::TextWrapped("Last diagnostics: %s", dir);

            ImGui::Spacing();
            ImGui::SeparatorText("Crash guard");
            auto skipper = ExceptionGuard::GetInstructionSkipperOptions();
            bool skipperChanged = false;
            skipperChanged |= ImGui::Checkbox("Skip faulting instruction", &skipper.enabled);
            ImGui::BeginDisabled(!skipper.enabled);
            skipperChanged |= ImGui::Checkbox("Skip all logged exception types", &skipper.skipAllExceptions);
            skipperChanged |= ImGui::Checkbox("Allow external code skipping", &skipper.allowExternalInstructions);
            ImGui::EndDisabled();
            if (skipperChanged)
                ExceptionGuard::SetInstructionSkipperOptions(skipper);
            ImGui::TextDisabled("Default scope is AV-only inside AtomicHeartMenu.dll; external skipping is risky.");

            ImGui::Spacing();
            ImGui::SeparatorText("Discovery  (reflect + DETOUR data, ANY actor by name)");
            ImGui::TextWrapped("Type part of an actor's name/class (e.g. \"Larisa\", "
                               "\"ReasignTargetOnFollow\", \"NPC\") to dump its complete field set "
                               "PLUS detour-hook data: vtables (native virtual fns), every "
                               "component's vtable (incl. Mercuna nav), and module+RVA addresses "
                               "for everything. Pure read-only worker thread; cannot freeze.");
            static char targetName[64] = "Larisa";
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::InputTextWithHint("##target", "actor name substring", targetName, sizeof(targetName));
            if (ImGui::Button("Dump targeted actor (full reflection)", ImVec2(-FLT_MIN, 0)))
            {
                LOG("UI: targeted dump -> %s", targetName);
                Features::DebugDumpTargetedActor(targetName);
            }

            ImGui::Spacing();
            ImGui::TextWrapped("Native-driver trace: latch the target, enable live capture, then make "
                               "it move. trace.jsonl logs every UFunction fired on it + raw params -- "
                               "this reveals what to detour to force-follow it.");
            float thalf = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) / 2.0f;
            if (ImGui::Button("Set trace target", ImVec2(thalf, 0)))
            {
                LOG("UI: set trace target -> %s", targetName);
                Features::DebugSetTraceTarget(targetName);
            }
            ImGui::SameLine();
            if (ImGui::Button("Clear trace target", ImVec2(-FLT_MIN, 0)))
                Features::DebugSetTraceTarget("");
            if (const char* tn = Features::DebugTraceTargetName(); tn && *tn)
                ImGui::TextWrapped("Tracing: %s", tn);
            else
                ImGui::TextDisabled("No trace target set.");
            ImGui::Spacing();
            ImGui::SeparatorText("Out-of-bounds finder");
            ImGui::TextWrapped("Stand where the game teleports you (e.g. the lighthouse edge) and press "
                               "this -- it logs nearby volume/trigger classes to AtomicHeartMenu.log so "
                               "the OOB teleporter can be identified and disabled precisely.");
            if (ImGui::Button("Dump nearby volumes"))
            {
                LOG("UI: dump nearby volumes");
                Features::DumpNearbyVolumes();
            }
            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }

    ImGui::Separator();
    ImGui::TextDisabled("Single-player only.  Eject with END.");
    ImGui::End();
}
