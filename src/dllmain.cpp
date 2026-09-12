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
#include <Windows.h>
#include <Psapi.h>
#include "core/globals.h"
#include "core/exception_guard.h"
#include "core/log.h"
#include "sdk/ue4.h"
#include "hooks/dx12_hook.h"
#include "hooks/native_hooks.h"
#include "hooks/ai_movement_hooks.h"
#include "features/features.h"
#include <exception>

#pragma comment(lib, "Psapi.lib")

namespace
{
    void PopulateModuleInfo()
    {
        HMODULE hExe = GetModuleHandleA(nullptr);
        MODULEINFO mi{};
        GetModuleInformation(GetCurrentProcess(), hExe, &mi, sizeof(mi));
        G::moduleBase = (uint8_t*)mi.lpBaseOfDll;
        G::moduleSize = mi.SizeOfImage;
    }

    void SafeRemoveHooks()
    {
        try { AiMovementHooks::Shutdown(); }
        catch (...) { LOG("AiMovementHooks::Shutdown threw during shutdown."); }
        try { NativeHooks::Shutdown(); }
        catch (...) { LOG("NativeHooks::Shutdown threw during shutdown."); }
        try { DX12Hook::Remove(); }
        catch (...) { LOG("DX12Hook::Remove threw during shutdown."); }
    }

    void RunMainThread(LPVOID param)
    {
        Log::Init(false);
        ExceptionGuard::Install();
        LOG("Injected. Module=%p", param);
        LOG("File logging enabled; injection does not steal game focus.");

        PopulateModuleInfo();
        LOG("Game module base=%p size=0x%zX", (void*)G::moduleBase, G::moduleSize);

        if (Offsets::ExpectedImageSize && G::moduleSize != Offsets::ExpectedImageSize)
            LOG("Game image 0x%zX; offsets.h was captured from 0x%zX (different game build).",
                G::moduleSize, Offsets::ExpectedImageSize);

        if (!DX12Hook::Install())
            LOG("WARNING: DX12 hook install failed - menu will not draw.");

        // Native code-byte hooks (separate from the SDK reflection layer below).
        // Pure module signature scan + MinHook detours; does NOT need the SDK, so
        // we bring the crash guard live as early as possible. Fails safe per-hook.
        try { NativeHooks::Init(); }
        catch (...) { LOG_HOOK("NativeHooks::Init threw -- continuing without native detours."); }

        bool sdkPrewarmed = false;
        bool sdkWarningLogged = false;
        ULONGLONG firstSdkAttemptMs = GetTickCount64();
        ULONGLONG lastSdkAttemptMs = 0;
        // Retrying only helps while the engine is still coming up; past that the
        // addresses are wrong for this build and the spam buries the diagnosis.
        ULONGLONG sdkRetryDelayMs = 1000;
        constexpr ULONGLONG kSdkRetryDelayCapMs = 30000;
        auto tryResolveSdk = [&]() -> bool
        {
            lastSdkAttemptMs = GetTickCount64();
            if (!UE::ResolveGlobals())
                return false;

            LOG_SDK("ready (GObjects/GNames/GWorld resolved).");
            Features::Prewarm();
            sdkPrewarmed = true;
            return true;
        };

        // Resolve once immediately; if injected too early, retry from the idle
        // worker below instead of scanning/logging in a tight burst.
        tryResolveSdk();

        // Idle until eject (DELETE/END) or until the host clears running.
        while (G::running.load())
        {
            ULONGLONG nowMs = GetTickCount64();
            if (!sdkPrewarmed && nowMs - lastSdkAttemptMs > sdkRetryDelayMs)
            {
                if (!tryResolveSdk())
                    sdkRetryDelayMs = (sdkRetryDelayMs * 2 > kSdkRetryDelayCapMs)
                                        ? kSdkRetryDelayCapMs : sdkRetryDelayMs * 2;

                if (!sdkPrewarmed && !sdkWarningLogged && nowMs - firstSdkAttemptMs > 10000)
                {
                    LOG_SDK("WARNING: not resolved yet - run tools/find_globals.py against this "
                            "game build and update offsets.h. Backing off toward a retry every "
                            "%llums.", kSdkRetryDelayCapMs);
                    sdkWarningLogged = true;
                }
            }

            // Heavy puzzle/minigame discovery (GObjects scans) runs here, off
            // the DX12 Present hook, so the render thread never stalls.
            if (sdkPrewarmed)
                Features::WorkerTick();

            if (GetAsyncKeyState(VK_END) & 1)
            {
                LOG("Eject key pressed.");
                if (!Features::PrepareUnload()) continue;
                G::running = false;
                break;
            }
            Sleep(50);
        }

        LOG("Unloading.");
    }

    DWORD WINAPI MainThread(LPVOID param)
    {
        DWORD exitCode = 0;

        try
        {
            RunMainThread(param);
        }
        catch (const std::exception& e)
        {
            LOG("MainThread: unhandled std::exception: %s", e.what());
            exitCode = 1;
        }
        catch (...)
        {
            LOG("MainThread: unhandled exception.");
            exitCode = 1;
        }

        G::running = false;
        SafeRemoveHooks();
        if (!UE::WaitForObjectNameIndex())
        {
            LOG("Name-index worker is still exiting; retaining the DLL to avoid unloading running code.");
            return exitCode;
        }
        ExceptionGuard::Remove();
        Log::Shutdown();
        Sleep(100);
        FreeLibraryAndExitThread(reinterpret_cast<HMODULE>(G::hModule), exitCode);
        return exitCode;
    }
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(hModule);
        G::hModule = hModule;
        HANDLE thread = CreateThread(nullptr, 0, MainThread, hModule, 0, nullptr);
        if (thread) CloseHandle(thread);
    }
    return TRUE;
}
