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
// Minimal LoadLibrary injector for single-player game modding.
// Usage:  injector.exe [process.exe] [full\path\to\AtomicHeartMenu.dll]
// Defaults: AtomicHeart-Win64-Shipping.exe  +  .\AtomicHeartMenu.dll
#include <Windows.h>
#include <TlHelp32.h>
#include <cstdio>
#include <string>


struct Handle
{
    HANDLE value = nullptr;
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    operator HANDLE() const { return value; }
};

static DWORD FindPid(const wchar_t* exe)
{
    Handle snap{CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)};
    if (snap.value == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe{sizeof(pe)}; DWORD pid = 0;
    if (Process32FirstW(snap, &pe)) do
    {
        if (_wcsicmp(pe.szExeFile, exe) == 0)
        {
            if (pid) { wprintf(L"Multiple matching processes; close the extra instance.\n"); return 0; }
            pid = pe.th32ProcessID;
        }
    } while (Process32NextW(snap, &pe));
    return pid;
}

static uintptr_t ModuleBase(DWORD pid, const wchar_t* name, bool fullPath)
{
    Handle snap{CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid)};
    if (snap.value == INVALID_HANDLE_VALUE) return 0;
    MODULEENTRY32W module{sizeof(module)};
    if (Module32FirstW(snap, &module)) do
    {
        if (_wcsicmp(fullPath ? module.szExePath : module.szModule, name) == 0)
            return reinterpret_cast<uintptr_t>(module.modBaseAddr);
    } while (Module32NextW(snap, &module));
    return 0;
}

int wmain(int argc, wchar_t** argv)
{
    const wchar_t* proc = argc > 1 ? argv[1] : L"AtomicHeart-Win64-Shipping.exe";
    wchar_t dllFull[MAX_PATH]{};
    DWORD length = GetFullPathNameW(argc > 2 ? argv[2] : L"AtomicHeartMenu.dll", MAX_PATH, dllFull, nullptr);
    if (!length || length >= MAX_PATH) { wprintf(L"DLL path invalid or too long.\n"); return 1; }
    DWORD attributes = GetFileAttributesW(dllFull);
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY))
    { wprintf(L"DLL not found: %s\n", dllFull); return 1; }
    DWORD pid = FindPid(proc);
    if (!pid) { wprintf(L"No unique process found: %s\n", proc); return 1; }
    if (ModuleBase(pid, dllFull, true)) { wprintf(L"This DLL is already loaded; no second injection attempted.\n"); return 1; }
    Handle process{OpenProcess(PROCESS_CREATE_THREAD | PROCESS_VM_OPERATION | PROCESS_VM_WRITE |
        PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, pid)};
    if (!process.value) { wprintf(L"OpenProcess failed (%lu).\n", GetLastError()); return 1; }
    BOOL remoteWow = FALSE, localWow = FALSE;
    if (!IsWow64Process(process, &remoteWow) || !IsWow64Process(GetCurrentProcess(), &localWow) || remoteWow != localWow)
    { wprintf(L"Process architecture check failed. Use the x64 build for Atomic Heart.\n"); return 1; }
    auto localLoader = GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW");
    HMODULE owner = nullptr; wchar_t ownerPath[MAX_PATH]{};
    if (!localLoader || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(localLoader), &owner) || !GetModuleFileNameW(owner, ownerPath, MAX_PATH))
    { wprintf(L"Cannot resolve the loader's owning module.\n"); return 1; }
    const wchar_t* name = wcsrchr(ownerPath, L'\\'); name = name ? name + 1 : ownerPath;
    const uintptr_t targetBase = ModuleBase(pid, name, false);
    if (!targetBase) { wprintf(L"Loader module unavailable in target.\n"); return 1; }
    auto loader = reinterpret_cast<LPTHREAD_START_ROUTINE>(targetBase +
        reinterpret_cast<uintptr_t>(localLoader) - reinterpret_cast<uintptr_t>(owner));
    SIZE_T size = (wcslen(dllFull) + 1) * sizeof(wchar_t), written = 0;
    void* remote = VirtualAllocEx(process, nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote) { wprintf(L"Remote allocation failed (%lu).\n", GetLastError()); return 1; }
    if (!WriteProcessMemory(process, remote, dllFull, size, &written) || written != size)
    {
        wprintf(L"Writing DLL path failed (%lu).\n", GetLastError());
        VirtualFreeEx(process, remote, 0, MEM_RELEASE); return 1;
    }
    Handle thread{CreateRemoteThread(process, nullptr, 0, loader, remote, 0, nullptr)};
    if (!thread.value)
    {
        wprintf(L"CreateRemoteThread failed (%lu).\n", GetLastError());
        VirtualFreeEx(process, remote, 0, MEM_RELEASE); return 1;
    }
    DWORD wait = WaitForSingleObject(thread, 15000);
    if (wait != WAIT_OBJECT_0)
    {
        // The loader may still read this buffer. Never free it or kill the thread.
        wprintf(L"Loader has not completed (wait=%lu). Outcome uncertain; inspect the process before retrying.\n", wait);
        return 2;
    }
    VirtualFreeEx(process, remote, 0, MEM_RELEASE);
    uintptr_t loaded = ModuleBase(pid, dllFull, true);
    if (!loaded) { wprintf(L"DLL not present after the loader completed. Check dependencies and game logs.\n"); return 1; }
    wprintf(L"DLL loaded at %p in pid %lu. Feature initialization must be checked in its log.\n", reinterpret_cast<void*>(loaded), pid);
    return 0;
}
