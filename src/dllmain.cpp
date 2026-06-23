#include <Windows.h>
#include <Psapi.h>
#include "core/globals.h"
#include "core/exception_guard.h"
#include "core/log.h"
#include "sdk/ue4.h"
#include "hooks/dx12_hook.h"
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
        try { DX12Hook::Remove(); }
        catch (...) { LOG("DX12Hook::Remove threw during shutdown."); }
    }

    void RunMainThread(LPVOID param)
    {
        Log::Init(true);
        ExceptionGuard::Install();
        LOG("Injected. Module=%p", param);
        LOG("Live console logging enabled. Use the console text for real-time copy/paste.");

        PopulateModuleInfo();
        LOG("Game module base=%p size=0x%zX", (void*)G::moduleBase, G::moduleSize);

        if (!DX12Hook::Install())
            LOG("WARNING: DX12 hook install failed - menu will not draw.");

        bool sdkPrewarmed = false;
        bool sdkWarningLogged = false;
        ULONGLONG firstSdkAttemptMs = GetTickCount64();
        ULONGLONG lastSdkAttemptMs = 0;
        auto tryResolveSdk = [&]() -> bool
        {
            lastSdkAttemptMs = GetTickCount64();
            if (!UE::ResolveGlobals())
                return false;

            LOG("SDK ready.");
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
            if (!sdkPrewarmed && nowMs - lastSdkAttemptMs > 1000)
            {
                tryResolveSdk();
                if (!sdkPrewarmed && !sdkWarningLogged && nowMs - firstSdkAttemptMs > 10000)
                {
                    LOG("WARNING: SDK not resolved yet - fix signatures/offsets in offsets.h if this persists.");
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
