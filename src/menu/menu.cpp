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
#include "../features/test_harness.h"
#include "menu.h"
#include "../core/exception_guard.h"
#include "../core/globals.h"
#include "../core/log.h"
#include "../features/features.h"
#include "../features/workbench.h"
#include "../features/bodyguards.h"
#include "../hooks/native_hooks.h"
#include "../hooks/ai_movement_hooks.h"
#include "../sdk/ue4.h"
#include "imgui.h"
#include <Windows.h>
#include <cfloat>
#include <algorithm>
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
        ImGui::TextWrapped("Spawn a companion to follow and defend you. Orders affect selected companions, or all companions when none are selected.");
        ImGui::Text("Companions: %d / 24   Selected: %d   Spawn queue: %d",
            Features::AiSquadCount(), Features::AiSelectedCount(), Features::AiSpawnQueueCount());

        int modelCount = Features::AiSpawnModelCount();
        if (f.aiSpawnModel < 0 || f.aiSpawnModel >= modelCount) f.aiSpawnModel = 0;
        ImGui::SetNextItemWidth(-175.0f);
        if (ImGui::BeginCombo("##companionModel", modelCount ? Features::AiSpawnModelName(f.aiSpawnModel) : "No loaded AI models"))
        {
            for (int i = 0; i < modelCount; ++i)
                if (ImGui::Selectable(Features::AiSpawnModelName(i), f.aiSpawnModel == i)) f.aiSpawnModel = i;
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(modelCount == 0 || Features::AiSquadCount() >= 24);
        if (ImGui::Button("Spawn companion", ImVec2(-FLT_MIN, 0))) Features::AiSpawnModel(f.aiSpawnModel);
        ImGui::EndDisabled();

        ImGui::SeparatorText("Orders");
        if (ImGui::Button("Follow + defend")) Features::AiOrderSelected(0);
        ImGui::SameLine();
        if (ImGui::Button("Follow only")) Features::AiOrderSelected(1);
        ImGui::SameLine();
        if (ImGui::Button("Hold position")) Features::AiOrderSelected(2);
        ImGui::SameLine();
        if (ImGui::Button("Attack nearest enemy")) Features::AiDispatchAttack();
        ImGui::SetNextItemWidth(220.0f);
        ImGui::SliderFloat("Follow distance", &f.aiFollowStopM, 1.0f, 8.0f, "%.1f m");
        ImGui::Checkbox("Keep companions alive", &f.aiInvincibleAllies);

        const auto guards = Bodyguards::Snapshot();
        if (guards.empty()) ImGui::TextDisabled("No regular companions. Spawn one above or recruit a nearby robot below.");
        if (ImGui::BeginTable("companionStatus", 4, ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
        {
            ImGui::TableSetupColumn("Select", ImGuiTableColumnFlags_WidthFixed, 45);
            ImGui::TableSetupColumn("Companion");
            ImGui::TableSetupColumn("State");
            ImGui::TableSetupColumn("Team", ImGuiTableColumnFlags_WidthFixed, 110);
            ImGui::TableHeadersRow();
            const auto nearby = Features::AiNearbyList(128);
            for (const auto& guard : guards)
            {
                ImGui::PushID(reinterpret_cast<void*>(guard.id));
                ImGui::TableNextRow(); ImGui::TableNextColumn();
                bool selected = Features::AiIsSelected(guard.id);
                if (ImGui::Checkbox("##select", &selected)) Features::AiToggleSelect(guard.id);
                ImGui::TableNextColumn(); ImGui::TextWrapped("%s", guard.name.c_str());
                ImGui::TableNextColumn(); ImGui::TextWrapped("%s (%.1f m)", guard.activity.c_str(), guard.distanceM);
                ImGui::TableNextColumn();
                ImGui::TextColored(guard.friendly ? ImVec4(.4f,.95f,.55f,1) : ImVec4(1,.65f,.2f,1),
                    "%s", guard.friendly ? "Friendly" : "Unconfirmed");
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        if (ImGui::Button("Clear selection")) Features::AiClearSelection();
        ImGui::SameLine();
        if (ImGui::Button("Release selected")) Features::AiReleaseRegularCompanions(true);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Restores their original AI and faction. Former enemies may attack again.");
        ImGui::SameLine();
        if (ImGui::Button("Release all")) Features::AiReleaseRegularCompanions(false);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Restores regular companions' original AI and faction.");

        if (ImGui::CollapsingHeader("Recruit nearby AI"))
        {
            ImGui::SetNextItemWidth(240);
            ImGui::SliderFloat("Search radius", &f.aiRadius, 10, 300, "%.0f m");
            if (ImGui::Button("Select nearby")) Features::AiSelectAllNearby();
            ImGui::SameLine();
            if (ImGui::Button("Recruit selected")) Features::AiRecruitSelected();
            ImGui::BeginChild("nearbyRecruit", ImVec2(0,180), ImGuiChildFlags_Borders);
            for (const auto& row : Features::AiNearbyList(64))
            {
                ImGui::PushID(reinterpret_cast<void*>(row.id));
                bool selected = row.selected;
                if (ImGui::Checkbox("##select", &selected)) Features::AiToggleSelect(row.id);
                ImGui::SameLine();
                ImGui::Text("%s   %.0f m%s", row.name.c_str(), row.distanceM, row.inSquad ? "  [companion]" : "");
                ImGui::PopID();
            }
            ImGui::EndChild();
        }
        if (ImGui::CollapsingHeader("More models and saved characters"))
        {
            static char search[128]{};
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::InputTextWithHint("##modelSearch", "Search robot, character, or boss type", search, sizeof(search));
            auto models = Features::AiSearchModels(search, 40);
            ImGui::BeginChild("modelSearchResults", ImVec2(0,180), ImGuiChildFlags_Borders);
            for (const auto& model : models)
            {
                ImGui::PushID(model.c_str());
                if (ImGui::SmallButton("Spawn")) Features::AiSpawnModelByName(model.c_str());
                ImGui::SameLine(); ImGui::TextUnformatted(model.c_str());
                ImGui::PopID();
            }
            ImGui::EndChild();
            if (ImGui::Button("Save selected models")) Features::AiSaveSelected();
            auto saved = Features::AiSavedCharacterNames();
            for (int i = 0; i < static_cast<int>(saved.size()); ++i)
            {
                ImGui::PushID(i);
                if (ImGui::SmallButton("Spawn saved")) Features::AiSpawnSavedCharacter(i);
                ImGui::SameLine(); ImGui::TextUnformatted(saved[i].c_str());
                ImGui::PopID();
            }
            ImGui::TextWrapped("Some encounter-specific bosses require their original level setup. A queued spawn is not a compatibility guarantee.");
        }
        if (ImGui::CollapsingHeader("World AI controls"))
        {
            ImGui::Checkbox("Freeze nearby enemies", &f.aiFreezeNearby);
            ImGui::Checkbox("Enemies fight each other", &f.aiFightEachOther);
            ImGui::Checkbox("Recover stuck companions by teleporting", &f.aiAllowTeleport);
            if (ImGui::Button("Kill selected")) Features::AiDispatchKill();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Kills selected units. Does not delete actor objects.");
            ImGui::SameLine();
            if (ImGui::Button("Kill nearby enemies")) Features::AiQueueKillNearby();
            if (ImGui::Button("Snapshot enemies")) Features::AiSnapshotZone();
            ImGui::SameLine();
            if (ImGui::Button("Respawn snapshot as companions")) Features::AiRespawnZone();
        }
    }

    void DrawHordeTab(Features::State& f)
    {
        const bool active = Features::HordeIsActive();

        // --- status strip ---------------------------------------------------
        if (active)
            ImGui::TextColored(ImVec4(0.40f, 0.95f, 0.55f, 1.0f), "%s", Features::HordeStatusText());
        else
            ImGui::TextColored(kAccent, "%s", Features::HordeStatusText());
        if (active)
        {
            ImGui::TextColored(ImVec4(1.0f, 0.78f, 0.30f, 1.0f),
                               "Run live -- the game is BLOCKED from saving. If you die, it auto-halts.");
        }
        ImGui::Spacing();

        // --- LOCATION -------------------------------------------------------
        ImGui::SeparatorText("Location  (where the round is fought)");
        BeginCard("hordeloc", 0);
        {
            int locCount = Features::HordeLocationCount();
            if (f.hordeLocation < 0 || f.hordeLocation >= locCount) f.hordeLocation = 0;
            const char* preview = Features::HordeLocationName(f.hordeLocation);

            ImGui::BeginDisabled(active); // don't change the destination mid-run
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::BeginCombo("##hordeloc", preview))
            {
                for (int i = 0; i < locCount; ++i)
                {
                    bool sel = (f.hordeLocation == i);
                    if (ImGui::Selectable(Features::HordeLocationName(i), sel))
                        f.hordeLocation = i;
                    if (sel) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("\"Here\" fights where you stand (no teleport). A saved arena teleports\n"
                                  "you there for the run and teleports you back when it ends. While a run\n"
                                  "is active the game NEVER saves, so the arena never overwrites progress.");

            static char arenaName[64] = "";
            ImGui::SetNextItemWidth(-110.0f);
            ImGui::InputTextWithHint("##arenaname", "name this spot (optional)", arenaName, sizeof(arenaName));
            ImGui::SameLine();
            if (ImGui::Button("Save here", ImVec2(-FLT_MIN, 0)))
            {
                Features::HordeSaveLocationHere(arenaName);
                arenaName[0] = '\0';
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Captures your CURRENT position as a reusable arena. Stand somewhere\n"
                                  "open (no story triggers) and save it, then pick it above.");

            if (f.hordeLocation >= 1)
            {
                ImGui::BeginDisabled(active);
                if (ImGui::SmallButton("Delete selected location"))
                {
                    Features::HordeDeleteLocation(f.hordeLocation);
                    f.hordeLocation = 0;
                }
                ImGui::EndDisabled();
            }
        }
        ImGui::EndChild();

        // --- ROUND SETTINGS -------------------------------------------------
        ImGui::SeparatorText("Round settings");
        BeginCard("hordeset", 0);
        {
            ImGui::BeginDisabled(active);
            ImGui::SetNextItemWidth(-140.0f);
            ImGui::SliderInt("Robots in wave 1", &f.hordePerRound, 1, 24);
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("How many robots the first wave spawns. Each later wave adds more.\n"
                                  "They stream in a few at a time so it never hitches.");
            LogCheckbox("Auto-advance waves (endless)", &f.hordeAutoAdvance, "hordeAutoAdvance");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("On: clear a wave and the next (bigger) one starts automatically.\n"
                                  "Off: clear a wave then press 'Next wave' yourself.");
            ImGui::TextDisabled("Robots are HOSTILE and fully killable (not invincible like the squad).");
            ImGui::TextDisabled("Tip: be near some robots first so their type is loaded to spawn from.");
        }
        ImGui::EndChild();

        // --- CONTROL --------------------------------------------------------
        ImGui::SeparatorText("Control");
        BeginCard("hordectl", 0);
        {
            if (!active)
            {
                if (AccentButton("START ROUND", 38.0f))
                {
                    LOG("UI: horde start");
                    Features::HordeStart();
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Teleports to the chosen location (if not 'Here'), blocks saving,\n"
                                      "and spawns the first wave of robots to hunt you.");
            }
            else
            {
                float half = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) / 2.0f;
                if (ImGui::Button(f.hordeAutoAdvance ? "Spawn extra wave" : "Next wave", ImVec2(half, 38.0f)))
                {
                    LOG("UI: horde next wave");
                    Features::HordeStart(); // while active, this pushes the next wave
                }
                ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.55f, 0.12f, 0.12f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.78f, 0.18f, 0.18f, 1.0f));
                if (ImGui::Button("STOP & RESTORE", ImVec2(-FLT_MIN, 38.0f)))
                {
                    LOG("UI: horde stop");
                    Features::HordeStop();
                }
                ImGui::PopStyleColor(2);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Ends the run: deletes every spawned robot, re-enables saving,\n"
                                      "and teleports you back to where you started.");
            }
            ImGui::Spacing();
            ImGui::TextColored(kAccent, "Wave %d", Features::HordeRound());
            ImGui::SameLine(0, 16); ImGui::Text("Alive: %d", Features::HordeAliveCount());
            ImGui::SameLine(0, 16); ImGui::Text("Queued: %d", Features::HordePendingCount());
            ImGui::SameLine(0, 16); ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.45f, 1.0f), "Kills: %d", Features::HordeKillCount());
        }
        ImGui::EndChild();

        ImGui::Spacing();
        ImGui::TextDisabled("Want to survive? Turn on God mode in the Player tab -- the robots stay\n"
                            "killable either way. With God mode off, dying instantly halts the run.");
    }
}

// ---- Hook Testing tab: pretest native detours + AI stability ----------------
// Free function (internal linkage) so Menu::Render can call it unqualified.
static void RenderHookTestingTab()
{
    using NativeHooks::State;
    using NativeHooks::Kind;

    ImGui::TextWrapped("Hook Diagnostics and the experimental hook-driven bodyguard controller. "
                       "Code hooks scan executable PE sections only; ambiguous or semantically "
                       "invalid targets are refused. This tab owns its bodyguard experiment and "
                       "does not enable the normal AI/Squad tab's controls.");
    ImGui::Spacing();

    // ---- crash-guard headline -------------------------------------------
    ImGui::SeparatorText("Crash guard  (the random AI null-target crash)");
    if (NativeHooks::CrashGuardActive())
        ImGui::TextColored(ImVec4(0.40f, 0.95f, 0.55f, 1.0f),
                           "* ACTIVE  -  null-this AI crash neutralised by a live detour");
    else
        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.40f, 1.0f),
                           "* NOT ACTIVE  -  signature unresolved; running SDK-only (crash still possible)");
    ImGui::Text("Null-target derefs caught: %llu", (unsigned long long)NativeHooks::TotalGuarded());
    ImGui::SameLine();
    ImGui::TextDisabled("(each one would have been an EXCEPTION_ACCESS_VIOLATION)");
    ImGui::TextWrapped("CrashGuard.ContextAllyComp is the required crash firewall. Other helper "
                       "hooks are optional diagnostics/friendship helpers and are refused until "
                       "uniquely resolved. Optional ambiguity does not mean the active crash guard "
                       "is broken.");
    ImGui::TextWrapped("Historical root cause: player was written into TargetAlly even though the "
                       "player is AHBaseCharacter, not AHAICharacter. The hook-debug bodyguard path "
                       "now follows/focuses/protects the player without storing player in TargetAlly.");

    // ---- signature table -------------------------------------------------
    ImGui::Spacing();
    ImGui::SeparatorText("Hook resolution");
    if (ImGui::Button("Rescan Signatures"))
    {
        LOG("UI: native hook rescan requested");
        NativeHooks::Rescan();
        Features::RescanHookMovementResolvers();
    }
    ImGui::SameLine();
    if (ImGui::Button("Dump AI Hook Status"))
        Features::DumpHookAiStatus();
    ImGui::SameLine();
    if (ImGui::Button("Clear Hook Logs"))
        Log::Clear();
    ImGui::TextDisabled("code signatures: executable sections only; ambiguous targets fail closed");

    if (ImGui::BeginTable("hooks", 10,
        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollX |
        ImGuiTableFlags_SizingFixedFit, ImVec2(0, 260)))
    {
        ImGui::TableSetupColumn("Hook", ImGuiTableColumnFlags_WidthFixed, 260.0f);
        ImGui::TableSetupColumn("Need");
        ImGui::TableSetupColumn("State");
        ImGui::TableSetupColumn("Resolver", ImGuiTableColumnFlags_WidthFixed, 120.0f);
        ImGui::TableSetupColumn("RVA");
        ImGui::TableSetupColumn("Matches");
        ImGui::TableSetupColumn("Scan");
        ImGui::TableSetupColumn("Sanity");
        ImGui::TableSetupColumn("Calls / guarded");
        ImGui::TableSetupColumn("Last guard / caller", ImGuiTableColumnFlags_WidthFixed, 150.0f);
        ImGui::TableHeadersRow();

        for (int i = 0; i < NativeHooks::Count(); ++i)
        {
            const NativeHooks::Entry& e = NativeHooks::Get(i);
            ImGui::TableNextRow();

            ImGui::TableNextColumn();
            ImGui::TextUnformatted(e.name);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s\n\n%s\n\nsig: %s", e.purpose, e.detail ? e.detail : "", e.sig);

            ImGui::TableNextColumn();
            ImGui::TextUnformatted(e.required ? "required" : "optional");

            ImGui::TableNextColumn();
            ImVec4 col; const char* st;
            switch (e.state)
            {
                case State::Resolved:      col = ImVec4(0.40f, 0.95f, 0.55f, 1.0f); st = (e.kind == Kind::Detour) ? "active/detour" : "resolved"; break;
                case State::Unresolved:    col = ImVec4(1.00f, 0.55f, 0.40f, 1.0f); st = "missing";      break;
                case State::Ambiguous:     col = ImVec4(1.00f, 0.80f, 0.30f, 1.0f); st = "ambiguous";    break;
                case State::Skipped:       col = ImVec4(0.65f, 0.65f, 0.65f, 1.0f); st = "skipped";      break;
                case State::SanityFailed:  col = ImVec4(1.00f, 0.45f, 0.40f, 1.0f); st = "failed sanity"; break;
                case State::InstallFailed: col = ImVec4(1.00f, 0.45f, 0.40f, 1.0f); st = "install failed"; break;
                default:                   col = ImVec4(0.60f, 0.60f, 0.60f, 1.0f); st = "pending";      break;
            }
            ImGui::TextColored(col, "%s", st);

            ImGui::TableNextColumn();
            ImGui::TextUnformatted(NativeHooks::ResolverName(e.resolver));

            ImGui::TableNextColumn();
            if (e.addr && G::moduleBase)
                ImGui::Text("0x%llX", (unsigned long long)((uintptr_t)e.addr - (uintptr_t)G::moduleBase));
            else
                ImGui::TextDisabled("-");

            ImGui::TableNextColumn();
            ImGui::Text("%d", e.matches);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("direct signature matches: %d", e.directMatches);

            ImGui::TableNextColumn();
            ImGui::TextUnformatted(e.executableOnly ? "exec only" : "whole module");

            ImGui::TableNextColumn();
            ImGui::TextColored(e.sanityPassed ? ImVec4(0.40f,0.95f,0.55f,1.0f) : ImVec4(0.75f,0.65f,0.45f,1.0f),
                               "%s", e.sanityPassed ? "pass" : "no");

            ImGui::TableNextColumn();
            ImGui::Text("%llu / %llu", (unsigned long long)e.calls.load(), (unsigned long long)e.guarded.load());

            ImGui::TableNextColumn();
            uint64_t lastMs = e.lastGuardMs.load();
            uintptr_t caller = e.lastCaller.load();
            if (lastMs)
                ImGui::Text("%llums ago", (unsigned long long)(GetTickCount64() - lastMs));
            else
                ImGui::TextDisabled("never");
            if (caller && G::moduleBase)
                ImGui::TextDisabled("caller +0x%llX", (unsigned long long)(caller - (uintptr_t)G::moduleBase));
        }

        // Reflected friendship interception: verified by SDK metadata and ReVa,
        // but intentionally not represented as a raw prologue detour.
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::TextUnformatted("Friendship.AIUtils.AreFriendlyCharacters");
        ImGui::TableNextColumn(); ImGui::TextUnformatted("optional");
        ImGui::TableNextColumn();
        if (Features::HookBodyguardModeActive()) ImGui::TextColored(ImVec4(0.40f,0.95f,0.55f,1), "active/hook");
        else if (Features::HookFriendshipResolved()) ImGui::TextColored(ImVec4(0.40f,0.95f,0.55f,1), "resolved");
        else ImGui::TextDisabled("skipped");
        ImGui::TableNextColumn(); ImGui::TextUnformatted("SDK + ReVa");
        ImGui::TableNextColumn(); ImGui::TextUnformatted("0x225BDA0*");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("ReVa-confirmed native exec thunk; interception uses the unique reflected UFunction.");
        ImGui::TableNextColumn(); ImGui::Text("%d", Features::HookFriendshipResolved() ? 1 : 0);
        ImGui::TableNextColumn(); ImGui::TextUnformatted("metadata");
        ImGui::TableNextColumn(); ImGui::TextUnformatted(Features::HookFriendshipResolved() ? "pass" : "no");
        ImGui::TableNextColumn(); ImGui::Text("%llu / -", (unsigned long long)Features::HookFriendshipForceCount());
        ImGui::TableNextColumn(); ImGui::TextDisabled("n/a");

        const AiMovementHooks::Status move = AiMovementHooks::GetStatus();
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::TextUnformatted("Movement.Mercuna.MoveToLocationNative");
        ImGui::TableNextColumn(); ImGui::TextUnformatted("optional");
        ImGui::TableNextColumn();
        if (move.moveHelperResolved) ImGui::TextColored(ImVec4(0.40f,0.95f,0.55f,1), "resolved"); else ImGui::TextDisabled("missing");
        ImGui::TableNextColumn(); ImGui::TextUnformatted("callsite-derived");
        ImGui::TableNextColumn();
        if (move.moveHelper && G::moduleBase) ImGui::Text("0x%llX", (unsigned long long)(move.moveHelper-(uintptr_t)G::moduleBase)); else ImGui::TextDisabled("-");
        ImGui::TableNextColumn(); ImGui::Text("%d", move.moveHelperResolved?1:0);
        ImGui::TableNextColumn(); ImGui::TextUnformatted("exec only");
        ImGui::TableNextColumn(); ImGui::TextUnformatted(move.moveHelperResolved?"pass":"no");
        ImGui::TableNextColumn(); ImGui::Text("%llu / %llu", (unsigned long long)move.nativeMovesIssued, (unsigned long long)move.nativeStopsIssued);
        ImGui::TableNextColumn(); ImGui::TextDisabled("ReVa wrapper +0x275");

        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::TextUnformatted("Movement.Mercuna.Ownership");
        ImGui::TableNextColumn(); ImGui::TextUnformatted("optional");
        ImGui::TableNextColumn();
        if (move.mercunaDetoursLive) ImGui::TextColored(ImVec4(0.40f,0.95f,0.55f,1), "active/detour"); else ImGui::TextDisabled("skipped");
        ImGui::TableNextColumn(); ImGui::TextUnformatted("vtable-derived");
        ImGui::TableNextColumn();
        if (move.moveRequestTarget && G::moduleBase) ImGui::Text("0x%llX", (unsigned long long)(move.moveRequestTarget-(uintptr_t)G::moduleBase)); else ImGui::TextDisabled("-");
        ImGui::TableNextColumn(); ImGui::Text("%d", move.mercunaDetoursLive?1:0);
        ImGui::TableNextColumn(); ImGui::TextUnformatted("exec only");
        ImGui::TableNextColumn(); ImGui::TextUnformatted(move.mercunaDetoursLive?"pass":"no");
        ImGui::TableNextColumn(); ImGui::Text("%llu / %llu", (unsigned long long)move.externalMovesBlocked,
                                             (unsigned long long)(move.externalStopsBlocked+move.externalCancelsBlocked));
        ImGui::TableNextColumn(); ImGui::Text("owned %d/%d", move.ownedComponents, move.registeredComponents);

        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::TextUnformatted("Movement.AIController.Ownership");
        ImGui::TableNextColumn(); ImGui::TextUnformatted("optional");
        ImGui::TableNextColumn();
        if (move.controllerDetoursLive) ImGui::TextColored(ImVec4(0.40f,0.95f,0.55f,1), "active/detour"); else ImGui::TextDisabled("waiting");
        ImGui::TableNextColumn(); ImGui::TextUnformatted("vtable-derived");
        ImGui::TableNextColumn();
        if (move.controllerMoveTarget && G::moduleBase) ImGui::Text("0x%llX", (unsigned long long)(move.controllerMoveTarget-(uintptr_t)G::moduleBase)); else ImGui::TextDisabled("-");
        ImGui::TableNextColumn(); ImGui::Text("%d", move.controllerDetoursLive?1:0);
        ImGui::TableNextColumn(); ImGui::TextUnformatted("exec only");
        ImGui::TableNextColumn(); ImGui::TextUnformatted(move.controllerDetoursLive?"pass":"no");
        ImGui::TableNextColumn(); ImGui::Text("%llu/%llu | block %llu/%llu",
                                             (unsigned long long)move.controllerMovesIssued,
                                             (unsigned long long)move.controllerStopsIssued,
                                             (unsigned long long)move.controllerMovesBlocked,
                                             (unsigned long long)move.controllerStopsBlocked);
        ImGui::TableNextColumn(); ImGui::TextDisabled("Move +0x790 / Stop +0x728");

        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::TextUnformatted("Movement.Twin.MixedNavigationTransition");
        ImGui::TableNextColumn(); ImGui::TextUnformatted("optional");
        ImGui::TableNextColumn();
        if (move.mixedTransitionResolved) ImGui::TextColored(ImVec4(0.40f,0.95f,0.55f,1), "resolved"); else ImGui::TextDisabled("skipped");
        ImGui::TableNextColumn(); ImGui::TextUnformatted("callsite-derived");
        ImGui::TableNextColumn();
        if (move.mixedTransition && G::moduleBase) ImGui::Text("0x%llX", (unsigned long long)(move.mixedTransition-(uintptr_t)G::moduleBase)); else ImGui::TextDisabled("-");
        ImGui::TableNextColumn(); ImGui::Text("%d", move.mixedTransitionResolved?1:0);
        ImGui::TableNextColumn(); ImGui::TextUnformatted("exec only");
        ImGui::TableNextColumn(); ImGui::TextUnformatted(move.mixedTransitionResolved?"pass":"no");
        ImGui::TableNextColumn(); ImGui::Text("%llu / -", (unsigned long long)move.mixedTransitions);
        ImGui::TableNextColumn(); ImGui::TextDisabled("2D/3D native transition");
        ImGui::EndTable();
    }

    // ---- dry-run validation ---------------------------------------------
    ImGui::Spacing();
    ImGui::SeparatorText("Dry-run validation  (read-only; never mutates game state)");
    static Features::HookBodyguardValidation validation{};
    if (ImGui::Button("Validate Bodyguard Pair"))
        validation = Features::ValidateHookBodyguardPair(true);
    ImGui::SameLine();
    if (ImGui::Button("Validate TargetAlly Assignment"))
    {
        LOG("[AI-DRYRUN] Validate TargetAlly Assignment requested");
        validation = Features::ValidateHookBodyguardPair(true);
    }
    if (G::sdkReady.load())
    {
        if (!validation.player)
            validation = Features::ValidateHookBodyguardPair(false);
        ImGui::Text("Selected guard: %p  %s  [%s]", (void*)validation.guard,
                    validation.guardName.c_str(), validation.guardClass.c_str());
        ImGui::Text("Player:        %p  %s  [%s]", (void*)validation.player,
                    validation.playerName.c_str(), validation.playerClass.c_str());
        ImGui::Text("Guard managed / AHAICharacter: %s / %s",
                    validation.guardManaged ? "yes" : "no", validation.guardIsAHAICharacter ? "yes" : "no");
        ImGui::Text("Player AHBaseCharacter / AHAICharacter: %s / %s",
                    validation.playerIsAHBaseCharacter ? "yes" : "no", validation.playerIsAHAICharacter ? "yes" : "no");
        ImGui::Text("Legacy TargetAlly(player) unsafe: %s", validation.legacyTargetAllyWouldBeUnsafe ? "YES" : "no");
        ImGui::TextColored(ImVec4(0.40f,0.95f,0.55f,1), "Current code would attempt unsafe TargetAlly: %s",
                           validation.currentCodeWouldAssignUnsafeTargetAlly ? "YES" : "NO");
        ImGui::Text("Friendship hook would force friendly: %s", validation.friendshipWouldForce ? "yes" : "no");
        ImGui::Text("Crash guard: %s", validation.crashGuardActive ? "ACTIVE" : "INACTIVE");
        ImGui::Text("Movement backend: %s", validation.movementBackend.empty()?"not evaluated":validation.movementBackend.c_str());
        ImGui::Text("Nav / controller: %p / %p    owned: %s", (void*)validation.navigationComponent,
                    (void*)validation.controller, validation.nativeMovementOwned?"yes":"no");
        ImGui::Text("Native helper / Mercuna detours: %s / %s    mixed-nav mode: %u",
                    validation.nativeMoveHelperResolved?"resolved":"missing",
                    validation.nativeMercunaDetoursLive?"live":"waiting",
                    (unsigned)validation.currentNavigationMode);
        ImGui::Text("Unsafe TargetAlly assignments skipped: %llu",
                    (unsigned long long)Features::UnsafeTargetAllySkipCount());
    }
    else
        ImGui::TextDisabled("SDK not resolved -- live validation unavailable.");

    // ---- Hook Debug experimental bodyguards ------------------------------
    ImGui::Spacing();
    ImGui::SeparatorText("Experimental hook-driven bodyguards");
    ImGui::TextWrapped("This is the Hook Diagnostics experiment, not the normal AI/Squad tab. "
                       "Our ProcessEvent pump owns spawn scheduling, follow goals, protection, "
                       "threat selection, attacks and native animation-driving AI calls. The player "
                       "is used as follow/focus/protect target, never as TargetAlly.");
    bool hookMode = Features::HookBodyguardModeActive();
    if (ImGui::Checkbox("Enable experimental hook bodyguards", &hookMode))
        Features::SetHookBodyguardMode(hookMode);
    bool forensics = Features::HookTwinForensicsActive() || Features::Get().hookTwinForensics;
    if (ImGui::Checkbox("NUKE Twin root-cause forensics (continuous dumps)", &forensics))
        Features::SetHookTwinForensics(forensics);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Writes hook_twin_events.jsonl plus 2 Hz state snapshots under AtomicHeartMenu_diagnostics\\togglelogsN. Use when the Twin enters the bad pose/state.");
    if (const char* dir = Features::DebugLastDumpDir(); dir && *dir)
        ImGui::TextDisabled("Last dump dir: %s", dir);
    ImGui::Text("Friendship hook: %s    forced pairs: %llu",
                Features::HookFriendshipResolved() ? "resolved" : "unavailable",
                (unsigned long long)Features::HookFriendshipForceCount());
    ImGui::Text("Ownership lock: %s    un-ally calls blocked: %llu",
                Features::OwnershipLockActive() ? "active" : "inactive",
                (unsigned long long)Features::OwnershipSwallowCount());
    {
        const AiMovementHooks::Status move = AiMovementHooks::GetStatus();
        ImGui::Text("Native movement: helper %s | Mercuna %s | Controller %s | Twin mixed-nav %s",
                    move.moveHelperResolved?"resolved":"missing",
                    move.mercunaDetoursLive?"live":"waiting for component",
                    move.controllerDetoursLive?"live":"waiting for controller",
                    move.mixedTransitionResolved?"resolved":"waiting for Twin");
        ImGui::Text("Native commands move/stop: %llu / %llu    external move/stop/cancel blocked: %llu / %llu / %llu",
                    (unsigned long long)move.nativeMovesIssued, (unsigned long long)move.nativeStopsIssued,
                    (unsigned long long)move.externalMovesBlocked, (unsigned long long)move.externalStopsBlocked,
                    (unsigned long long)move.externalCancelsBlocked);
        ImGui::Text("Controller direct Move/Stop issued: %llu / %llu    blocked: %llu / %llu",
                    (unsigned long long)move.controllerMovesIssued,
                    (unsigned long long)move.controllerStopsIssued,
                    (unsigned long long)move.controllerMovesBlocked,
                    (unsigned long long)move.controllerStopsBlocked);
    }

    // ---- stress test -----------------------------------------------------
    ImGui::Spacing();
    ImGui::SeparatorText("Hook bodyguard spawn");
    static int hookSpawnModel = 0;
    int hookModelCount = Features::AiSpawnModelCount();
    if (hookSpawnModel < 0 || hookSpawnModel >= hookModelCount) hookSpawnModel = 0;
    const char* hookPreview = hookModelCount > 0
        ? Features::AiSpawnModelName(hookSpawnModel) : "(no live AI classes discovered yet)";
    ImGui::TextDisabled("Loaded runtime class (recommended for Twins and bosses)");
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::BeginCombo("##hookspawnmodel", hookPreview))
    {
        for (int i = 0; i < hookModelCount; ++i)
        {
            bool selected = hookSpawnModel == i;
            if (ImGui::Selectable(Features::AiSpawnModelName(i), selected)) hookSpawnModel = i;
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    if (ImGui::Button("Spawn selected as Hook bodyguard", ImVec2(-FLT_MIN, 0)))
        Features::HookAiSpawnModel(hookSpawnModel);
    ImGui::TextDisabled("Spawn queue: %d", Features::AiSpawnQueueCount());

    static int hookBossPreset = 0;
    int hookBossCount = Features::AiBossPresetCount();
    if (hookBossPreset < 0 || hookBossPreset >= hookBossCount) hookBossPreset = 0;
    const char* hookBossPreview = hookBossCount > 0 ? Features::AiBossPresetName(hookBossPreset) : "(no boss presets)";
    ImGui::TextDisabled("Boss presets (base game + DLC)");
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::BeginCombo("##hookbosspreset", hookBossPreview))
    {
        for (int i = 0; i < hookBossCount; ++i)
        {
            bool selected = hookBossPreset == i;
            if (ImGui::Selectable(Features::AiBossPresetName(i), selected)) hookBossPreset = i;
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    if (ImGui::Button("Spawn boss preset as Hook bodyguard", ImVec2(-FLT_MIN, 0)))
        Features::HookAiSpawnBossPreset(hookBossPreset);

    static char hookModelSearch[80]{};
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##hookmodelsearch", "Search all character classes (Twin, boss, Vova...)",
                             hookModelSearch, sizeof(hookModelSearch));
    std::vector<std::string> hookSearchResults = Features::AiSearchModels(hookModelSearch, 40);
    if (ImGui::TreeNodeEx("All discovered/loadable character classes", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::BeginChild("hook_model_results", ImVec2(0, 125), true);
        for (int i = 0; i < (int)hookSearchResults.size(); ++i)
        {
            ImGui::PushID(9000 + i);
            if (ImGui::Button("Spawn Hook", ImVec2(92, 0)))
                Features::HookAiSpawnModelByName(hookSearchResults[i].c_str());
            ImGui::SameLine();
            ImGui::TextUnformatted(hookSearchResults[i].c_str());
            ImGui::PopID();
        }
        ImGui::EndChild();
        ImGui::TextDisabled("Loaded runtime classes are safest. On-demand boss classes remain guarded by the existing spawn queue.");
        ImGui::TreePop();
    }

    ImGui::Spacing();
    ImGui::SeparatorText("Hook bodyguard controls");
    ImGui::TextWrapped("These controls activate the experimental hook mode before mutating state. "
                       "Follow uses a fixed 2 m hold ring with native Mercuna ownership, or one protected "
                       "AIController move request when Mercuna is absent. There is no shared squad blackboard, "
                       "per-frame movement-input, raw velocity, or periodic path flush in this mode. Threat perception is 360 degrees but only engages the "
                       "12 m protection perimeter (or an active attacker); native attacks retain "
                       "their animations and the first confirmed hit kills the robot.");
    const float bw = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) / 2.0f;
    if (ImGui::Button("Recruit nearby -> hook roster", ImVec2(bw, 0)))
    { LOG("UI: [hooktest] recruit nearby"); Features::HookAiRecruitNearby(); }
    ImGui::SameLine();
    if (ImGui::Button("Hook roster follow me", ImVec2(-FLT_MIN, 0)))
    { LOG("UI: [hooktest] follow player"); Features::HookAiFollow(); }
    if (ImGui::Button("Spawn nearest as bodyguard", ImVec2(bw, 0)))
    { LOG("UI: [hooktest] spawn bodyguard"); Features::HookAiSpawnBodyguard(); }
    ImGui::SameLine();
    if (ImGui::Button("Hook roster attack", ImVec2(-FLT_MIN, 0)))
    { LOG("UI: [hooktest] dispatch attack"); Features::HookAiAttack(); }
    if (ImGui::Button("Release hook roster", ImVec2(bw, 0)))
    { LOG("UI: [hooktest] release squad"); Features::HookAiRelease(); }
    ImGui::SameLine();
    if (ImGui::Button("Delete hook roster", ImVec2(-FLT_MIN, 0)))
    { LOG("UI: [hooktest] delete hook roster"); Features::HookAiDeleteRoster(); }
    ImGui::Text("Hook roster: %d    Global squad: %d", Features::HookAiCount(), Features::AiSquadCount());
}

void Menu::Render()
{
    auto& f = Features::Get();

    static bool themed = false;
    static float uiScale = 1.0f;
    if (!themed)
    {
        ApplyTheme();
        const float height = ImGui::GetIO().DisplaySize.y;
        uiScale = height >= 1300 ? 1.25f : 1.0f;
        ImGui::GetStyle().ScaleAllSizes(uiScale);
        ImGui::GetStyle().FontSizeBase = 17.0f;
        ImGui::GetStyle().FontScaleMain = uiScale;
        themed = true;
    }

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

    const ImVec2 display = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowSize(ImVec2((std::min)(960.0f * uiScale, display.x - 40),
        (std::min)(740.0f * uiScale, display.y - 40)), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(ImVec2(660, 460), ImVec2(display.x, display.y));
    if (!ImGui::Begin("ATOMIC HEART  |  Menu###AtomicHeartMenu", nullptr, ImGuiWindowFlags_NoCollapse))
    { ImGui::End(); return; }

    // SDK status pill
    if (G::sdkReady.load())
        ImGui::TextColored(ImVec4(0.40f, 0.95f, 0.55f, 1.0f), "Connected  |  %d loaded objects", UE::NumObjects());
    else
        ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.40f, 1.0f), "Waiting for the game connection. Details are available in Diagnostics.");
    ImGui::Separator();

    static int page = 0;
    static const char* pages[] = { "Player", "Weapons", "AI / Squad", "Horde Rounds", "Nora", "World",
        "Visuals", "Rendering", "Tools", "Agent tests", "Diagnostics", "Hook diagnostics" };
    const float footer = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y;
    ImGui::BeginChild("navigation", ImVec2(174 * uiScale, -footer), true);
    for (int i = 0; i < IM_ARRAYSIZE(pages); ++i)
    {
        if (i == 9) { ImGui::Spacing(); ImGui::SeparatorText("Development"); }
        if (ImGui::Selectable(pages[i], page == i, 0, ImVec2(0, 25 * uiScale))) page = i;
    }
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("feature_content", ImVec2(0, -footer), true);
    ImGui::PushID(page);
    {
        if (page == 0)
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
        }

        if (page == 1)
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
        }

        if (page == 2)
        {
            DrawAiTab(f);
        }

        if (page == 3)
        {
            DrawHordeTab(f);
        }

        if (page == 4)
        {
            auto state = Workbench::GetStatus();
            ImGui::SeparatorText("Portable Nora");
            ImGui::TextWrapped("The game's full Nora Blueprint, with its own animation, sound and interaction components.");
            ImGui::TextColored(state.noraReady ? ImVec4(0.3f, 0.9f, 0.6f, 1) : ImVec4(0.9f, 0.7f, 0.3f, 1),
                "%s", state.noraInUse ? "IN USE" : state.noraReady ? "READY" : state.noraPresent ? "INITIALIZING" : "NOT SPAWNED");
            ImGui::BeginDisabled(state.busy);
            ImGui::BeginDisabled(state.noraInUse);
            if (ImGui::Button(state.noraPresent ? "Move Nora nearby" : "Spawn Nora nearby")) Workbench::SpawnNora();
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::BeginDisabled(!state.noraPresent || state.noraInUse);
            if (ImGui::Button("Remove portable Nora")) Workbench::RemoveNora();
            ImGui::EndDisabled();
            ImGui::BeginDisabled(!state.noraReady || state.noraInUse);
            if (ImGui::Button("Use Nora")) Workbench::UseNora();
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::BeginDisabled(!state.noraInUse);
            if (ImGui::Button("Close Nora dialogue")) Workbench::CloseNoraDialogue();
            ImGui::EndDisabled();
            if (state.noraInUse && ImGui::TreeNode("Dialogue response controls"))
            {
                ImGui::TextWrapped("Choose the numbered response currently shown by the game.");
                for (int choice = 1; choice <= 6; ++choice)
                {
                    ImGui::PushID(choice);
                    if (choice > 1) ImGui::SameLine();
                    if (ImGui::Button(std::to_string(choice).c_str())) Workbench::ChooseNoraResponse(choice);
                    ImGui::PopID();
                }
                ImGui::TreePop();
            }
            ImGui::TextWrapped("Spawn in a clear, level space. You can also interact with her normally using F.");
            ImGui::SeparatorText("Crafting and skills");
            static float priceScale = 1.0f;
            ImGui::SliderFloat("Cost multiplier", &priceScale, 0.0f, 2.0f, "%.2fx");
            if (ImGui::Button("Apply prices")) Workbench::SetPrices(priceScale);
            ImGui::SameLine();
            if (ImGui::Button("Free crafting and skills")) { priceScale = 0; Workbench::SetPrices(0); }
            if (ImGui::Button("Restore original prices")) { priceScale = 1; Workbench::SetPrices(1); }
            ImGui::Text("Applied %.2fx to %d price lists", state.priceMultiplier, state.pricesChanged);
            if (state.scanPercent < 100) ImGui::ProgressBar(state.scanPercent / 100.0f, ImVec2(-1, 0), "Scanning loaded assets");
            ImGui::TextWrapped("Includes crafting, weapon upgrades and skill costs. Reopen crafting after applying. Purchases and skill unlocks follow the game's normal save behavior.");
            bool recipes = state.unlockRecipes;
            if (ImGui::Checkbox("Unlock recipe requirements", &recipes)) Workbench::SetRecipeUnlocks(recipes);
            ImGui::Text("Recipes learned: %d   Failed: %d", state.recipesLearned, state.recipeFailures);
            ImGui::TextWrapped("Learns recipes as their assets load. Learned recipes remain in your save after this option is disabled.");
            if (ImGui::Button("Unlock all skills")) Workbench::UnlockSkills();
            ImGui::EndDisabled();
            ImGui::Separator();
            ImGui::TextWrapped("%s", state.message.c_str());
        }

        if (page == 5)
        {
            ImGui::SeparatorText("Stages and free roam");
            const auto worldStatus = Workbench::GetStatus();
            ImGui::BeginDisabled(worldStatus.busy);
            if (ImGui::Button("Stage selector (experimental)")) Workbench::ToggleDebugMenu();
            ImGui::SameLine();
            if (ImGui::Button("Close stage selector")) Workbench::CloseDebugMenu();
            if (ImGui::Button("Continue open-world save")) Workbench::ContinueOpenWorld();
            ImGui::TextWrapped("Use these controls from the title main menu. Stage selection is not yet verified in this shipping build. Open-world continuation requires an eligible post-game save and does not remove campaign objectives.");
            bool hidden = worldStatus.objectiveHidden;
            if (ImGui::Checkbox("Hide tracked objective", &hidden)) Workbench::SetObjectiveHidden(hidden);
            ImGui::TextDisabled("Objective display only; mission scripts continue in campaign saves.");
            ImGui::EndDisabled();
            ImGui::TextWrapped("%s", worldStatus.message.c_str());
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
        }

        if (page == 6)
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
                ImGui::SetTooltip("Recolors actual shader colour parameters, including inherited materials and layers.\n"
                                  "Original materials are restored when disabled or when the weapon changes.");
            if (f.weaponRgb) ImGui::Text("Shader colour parameters: %d", Features::WeaponColorParameterCount());
            if (!f.weaponRgbRainbow)
                ImGui::ColorEdit3("Gun color", f.weaponRgbColor, ImGuiColorEditFlags_NoInputs);
        }

        if (page == 7)
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
        }

        if (page == 8)
        {
            ImGui::SeparatorText("Readability");
            ImGui::SliderFloat("Text scale", &ImGui::GetStyle().FontScaleMain, 0.9f, 1.75f, "%.2fx");
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
        }

        if (page == 9)
        {
            TestHarness::Draw();
        }

        if (page == 10)
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
            ImGui::SeparatorText("Offset verification");
            ImGui::TextWrapped("Reads every reflected member offset off the running game and diffs "
                               "it against offsets.h, so a game patch names the entries it moved "
                               "instead of showing up as a silent misbehaviour. No Dumper-7 run "
                               "needed. Results go to AtomicHeartMenu.log.");
            if (ImGui::Button("Verify member offsets", ImVec2(-FLT_MIN, 0)))
            {
                LOG("UI: member offset verification requested");
                Features::DebugVerifyMemberOffsets();
            }

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
        }

        if (page == 11)
        {
            RenderHookTestingTab();
        }

    }

    ImGui::PopID();
    ImGui::EndChild();
    ImGui::Separator();
    ImGui::TextDisabled("INSERT  Show / hide     END  Restore and eject");
    ImGui::SameLine();
    if (ImGui::SmallButton("Close menu")) G::menuOpen = false;
    ImGui::End();
}
