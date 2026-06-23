// Minimal LoadLibrary injector for single-player game modding.
// Usage:  injector.exe [process.exe] [full\path\to\AtomicHeartMenu.dll]
// Defaults: AtomicHeart-Win64-Shipping.exe  +  .\AtomicHeartMenu.dll
#include <Windows.h>
#include <TlHelp32.h>
#include <cstdio>
#include <string>

static DWORD FindPid(const wchar_t* exe)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe{ sizeof(pe) };
    DWORD pid = 0;
    if (Process32FirstW(snap, &pe))
        do {
            if (_wcsicmp(pe.szExeFile, exe) == 0) { pid = pe.th32ProcessID; break; }
        } while (Process32NextW(snap, &pe));
    CloseHandle(snap);
    return pid;
}

int wmain(int argc, wchar_t** argv)
{
    const wchar_t* proc = (argc > 1) ? argv[1] : L"AtomicHeart-Win64-Shipping.exe";

    wchar_t dllFull[MAX_PATH];
    if (argc > 2) GetFullPathNameW(argv[2], MAX_PATH, dllFull, nullptr);
    else          GetFullPathNameW(L"AtomicHeartMenu.dll", MAX_PATH, dllFull, nullptr);

    if (GetFileAttributesW(dllFull) == INVALID_FILE_ATTRIBUTES)
    { wprintf(L"DLL not found: %s\n", dllFull); return 1; }

    DWORD pid = FindPid(proc);
    if (!pid) { wprintf(L"Process not found: %s\n", proc); return 1; }
    wprintf(L"Target %s pid=%lu\nDLL %s\n", proc, pid, dllFull);

    HANDLE hProc = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_VM_OPERATION |
                               PROCESS_VM_WRITE | PROCESS_VM_READ | PROCESS_QUERY_INFORMATION,
                               FALSE, pid);
    if (!hProc) { wprintf(L"OpenProcess failed (%lu) - run as admin.\n", GetLastError()); return 1; }

    SIZE_T size = (wcslen(dllFull) + 1) * sizeof(wchar_t);
    LPVOID remote = VirtualAllocEx(hProc, nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    WriteProcessMemory(hProc, remote, dllFull, size, nullptr);

    // LoadLibraryW lives at the same address in every process (kernel32 ASLR is per-boot, shared).
    auto loadLib = (LPTHREAD_START_ROUTINE)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW");
    HANDLE hThread = CreateRemoteThread(hProc, nullptr, 0, loadLib, remote, 0, nullptr);
    if (!hThread) { wprintf(L"CreateRemoteThread failed (%lu)\n", GetLastError()); return 1; }

    WaitForSingleObject(hThread, INFINITE);
    DWORD exitCode = 0; GetExitCodeThread(hThread, &exitCode);
    wprintf(L"Injected. LoadLibrary returned module handle 0x%lX\n", exitCode);

    VirtualFreeEx(hProc, remote, 0, MEM_RELEASE);
    CloseHandle(hThread);
    CloseHandle(hProc);
    return 0;
}
