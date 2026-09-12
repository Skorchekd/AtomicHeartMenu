// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Skorchekd. Preserve attribution to Skorchekd, Dumper-7
// (Encryqed), MinHook (Tsuda Kageyu), and Dear ImGui (ocornut). See LICENSE/NOTICE.
#pragma once
#include <string>

namespace Workbench
{
    struct Status
    {
        bool busy = false;
        bool noraPresent = false;
        bool noraReady = false;
        bool noraInUse = false;
        bool objectiveHidden = false;
        bool unlockRecipes = false;
        int recipesLearned = 0, recipeFailures = 0;
        int pricesChanged = 0;
        int scanPercent = 100;
        float priceMultiplier = 1.0f;
        std::string message = "Spawn Nora nearby, then interact with her normally.";
    };
    Status GetStatus();
    std::string TestSnapshotJson(); // game thread only
    void Tick();
    void WorkerTick(); // idle worker: read-only discovery and cache I/O
    void SpawnNora();
    void UseNora();
    void RemoveNora();
    void CloseNoraDialogue();
    void ChooseNoraResponse(int choice);
    void SetPrices(float multiplier);
    void SetRecipeUnlocks(bool enabled);
    void UnlockSkills();
    void ToggleDebugMenu();
    void CloseDebugMenu();
    void ContinueOpenWorld();
    void SetObjectiveHidden(bool hidden);
    bool FilterProcessEvent(void* object, void* function, void* params);
    bool CleanupGameThread();
}
