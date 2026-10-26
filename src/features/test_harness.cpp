// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Skorchekd. See LICENSE and NOTICE for attribution terms.
#include "test_harness.h"
#include "features.h"
#include "workbench.h"
#include "../core/globals.h"
#include "../core/log.h"
#include <Windows.h>
#include <imgui.h>
#include <atomic>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>

namespace
{
    std::atomic<bool> enabled{false}, samplePending{false};
    std::mutex mutex;
    std::string observation = "null", message = "Disabled", pendingCommand;
    float pendingValue = 0;
    unsigned long long pendingId = 0, acceptedId = 0, processedId = 0;
    const unsigned long long session = (GetTickCount64() << 16) ^ GetCurrentProcessId();
    unsigned long long sampledAt = 0;
    std::filesystem::path directory;

    std::string Quote(const std::string& value)
    {
        std::ostringstream out; out << '"';
        for (unsigned char c : value)
        {
            if (c == '"' || c == '\\') out << '\\' << c;
            else if (c < 32) out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << int(c) << std::dec;
            else out << c;
        }
        out << '"'; return out.str();
    }
    void Observe()
    {
        if (!enabled || samplePending.exchange(true)) return;
        if (!Features::QueueGameAction([]
        {
            try
            {
                const auto sample = Features::TestSnapshotJson();
                const auto workbench = Workbench::TestSnapshotJson();
                std::lock_guard<std::mutex> lock(mutex);
                observation = "{\"game\":" + sample + ",\"workbench\":" + workbench + "}";
                sampledAt = GetTickCount64();
            }
            catch (...)
            {
                std::lock_guard<std::mutex> lock(mutex);
                message = "Observation failed; no test pass recorded";
            }
            samplePending = false;
        })) samplePending = false;
    }
}

void TestHarness::WorkerTick()
{
    static bool initialized = false;
    if (!initialized)
    {
        wchar_t path[32768]{};
        DWORD length = GetModuleFileNameW(static_cast<HMODULE>(G::hModule), path, 32768);
        if (!length || length >= 32768) return;
        directory = std::filesystem::path(path).parent_path() / "AtomicHeartMenu.tests";
        // Explicit local opt-in for development. Never enabled in a normal install.
        std::ifstream optIn(directory / "enable.once");
        std::string text; std::getline(optIn, text); optIn.close();
        if (text == "enable")
        {
            enabled = true;
            message = "Enabled; awaiting observations";
            std::error_code error; std::filesystem::remove(directory / "enable.once", error);
        }
        initialized = true;
    }
    static ULONGLONG lastMs = 0;
    const auto now = GetTickCount64();
    if (now - lastMs < 500) return;
    lastMs = now;
    if (!enabled) return;
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) return;

    const auto commandPath = directory / "command.txt";
    const auto size = std::filesystem::file_size(commandPath, error);
    if (!error && size > 0 && size <= 256)
    {
        std::ifstream input(commandPath);
        unsigned long long requestedSession = 0, id = 0;
        std::string command, trailing; float value = 0;
        if (input >> requestedSession >> id >> command >> value && !(input >> trailing) &&
            requestedSession == session && std::isfinite(value))
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (id > acceptedId && pendingCommand.empty())
            {
                acceptedId = id; pendingId = id; pendingCommand = command; pendingValue = value;
                message = "Command received; awaiting render dispatch";
            }
        }
    }
    Observe();
    std::string json;
    {
        std::lock_guard<std::mutex> lock(mutex);
        std::ostringstream out;
        out << "{\"protocol\":1,\"session\":" << session << ",\"pid\":" << GetCurrentProcessId()
            << ",\"written_at_ms\":" << now << ",\"sampled_at_ms\":" << sampledAt
            << ",\"game_thread_sample_age_ms\":" << (sampledAt ? now - sampledAt : 0)
            << ",\"accepted_id\":" << acceptedId << ",\"processed_id\":" << processedId
            << ",\"message\":" << Quote(message) << ",\"observation\":" << observation << "}\n";
        json = out.str();
    }
    const auto temp = directory / "status.tmp";
    { std::ofstream output(temp, std::ios::binary | std::ios::trunc); output << json; if (!output.good()) return; }
    MoveFileExW(temp.c_str(), (directory / "status.json").c_str(), MOVEFILE_REPLACE_EXISTING);
}

void TestHarness::RenderTick()
{
    if (!enabled) return;
    std::string command; float value; unsigned long long id;
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (pendingCommand.empty()) return;
        command.swap(pendingCommand); value = pendingValue; id = pendingId;
    }
    const bool booleanCommand = command == "menu" || command == "noclip" || command == "fly" ||
        command == "streaming" || command == "squad_aggressive" || command == "objective_hide";
    if (booleanCommand && value != 0 && value != 1)
    {
        std::lock_guard<std::mutex> lock(mutex);
        processedId = id;
        message = "Rejected: boolean controls require 0 or 1";
        return;
    }
    bool valid = true;
    auto& f = Features::Get();
    if (command == "observe") {}
    else if (command == "menu") G::menuOpen = value != 0;
    else if (command == "nora_spawn") Workbench::SpawnNora();
    else if (command == "nora_use") Workbench::UseNora();
    else if (command == "nora_choice" && value >= 1 && value <= 6 && std::floor(value) == value)
        Workbench::ChooseNoraResponse(static_cast<int>(value));
    else if (command == "nora_close") Workbench::CloseNoraDialogue();
    else if (command == "nora_remove") Workbench::RemoveNora();
    else if (command == "prices" && value >= 0 && value <= 2) Workbench::SetPrices(value);
    else if (command == "recipes_unlock" && (value == 0 || value == 1)) Workbench::SetRecipeUnlocks(value != 0);
    else if (command == "skills_unlock") Workbench::UnlockSkills();
    else if (command == "turn") valid = Features::TestTurn(value);
    else if (command == "noclip") f.noclip = value != 0;
    else if (command == "fly") { f.flyHack = value != 0; if (!f.flyHack) f.noclip = false; }
    else if (command == "fly_forward" || command == "fly_up" || command == "fly_down" || command == "fly_backward")
        valid = Features::TestFlyPulse(command == "fly_forward" ? 1 : command == "fly_up" ? 2 : command == "fly_down" ? 3 : 4, value);
    else if (command == "fly_return") Features::ReturnToFlyStart();
    else if (command == "streaming") f.flyStreamingAssist = value != 0;
    else if (command == "squad_spawn") Features::AiSpawnBodyguard();
    else if (command == "squad_recruit") Features::AiRecruitNearby();
    else if (command == "squad_release") Features::AiReleaseSquad();
    else if (command == "squad_aggressive") { f.aiSquadAggressive = value != 0; Features::AiOrderSelected(value != 0 ? 0 : 1); }
    else if (command == "squad_order" && value >= 0 && value <= 2 && std::floor(value) == value)
        Features::AiOrderSelected(static_cast<int>(value));
    else if (command == "weapon_rgb" && (value == 0 || value == 1)) f.weaponRgb = value != 0;
    else if (command == "verify_offsets") Features::DebugVerifyMemberOffsets();
    else if (command == "snapshot") Features::DebugDumpGameSnapshot();
    else if (command == "stage_open") Workbench::ToggleDebugMenu();
    else if (command == "stage_close") Workbench::CloseDebugMenu();
    else if (command == "open_world") Workbench::ContinueOpenWorld();
    else if (command == "objective_hide") Workbench::SetObjectiveHidden(value != 0);
    else valid = false;
    std::lock_guard<std::mutex> lock(mutex);
    processedId = id;
    message = valid ? "Dispatched " + command + "; inspect observations for outcome" : "Rejected command or argument: " + command;
}

void TestHarness::Draw()
{
    bool on = enabled;
    if (ImGui::Checkbox("Enable local agent test harness", &on))
    {
        enabled = on;
        std::lock_guard<std::mutex> lock(mutex);
        if (!on) { pendingCommand.clear(); processedId = acceptedId; }
        message = on ? "Enabled; awaiting observations" : "Disabled; undispatched commands cancelled";
    }
    ImGui::TextWrapped("Commands are restricted to menu features. Each session requires its current token and increasing request IDs. Dispatched does not mean passed.");
    ImGui::Text("Session: %llu", session);
    ImGui::TextWrapped("Files: AtomicHeartMenu.tests beside the DLL");
    if (ImGui::Button("Capture observations")) Observe();
    std::lock_guard<std::mutex> lock(mutex);
    ImGui::TextWrapped("%s", message.c_str());
    if (sampledAt) ImGui::Text("Sample age: %llums", GetTickCount64() - sampledAt);
    ImGui::BeginChild("observations", ImVec2(0, 260), true, ImGuiWindowFlags_HorizontalScrollbar);
    ImGui::TextUnformatted(observation.c_str());
    ImGui::EndChild();
}
