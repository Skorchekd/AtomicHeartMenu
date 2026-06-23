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
#include "exception_guard.h"
#include "globals.h"
#include "log.h"
#include <Windows.h>
#include <Psapi.h>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include "../../deps/minhook/src/hde/hde64.h"

#pragma comment(lib, "Psapi.lib")

namespace
{
    void* g_handler = nullptr;
    uintptr_t g_selfBase = 0;
    uintptr_t g_selfEnd = 0;
    std::atomic<unsigned> g_loggedExceptions{ 0 };

#if defined(_M_X64)
    std::atomic<bool> g_skipEnabled{ false };
    std::atomic<bool> g_skipAvOnly{ true };
    std::atomic<bool> g_skipExternal{ false };
#endif

    const char* ExceptionName(DWORD code)
    {
        switch (code)
        {
        case EXCEPTION_ACCESS_VIOLATION: return "ACCESS_VIOLATION";
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "ARRAY_BOUNDS_EXCEEDED";
        case EXCEPTION_BREAKPOINT: return "BREAKPOINT";
        case EXCEPTION_DATATYPE_MISALIGNMENT: return "DATATYPE_MISALIGNMENT";
        case EXCEPTION_FLT_DIVIDE_BY_ZERO: return "FLT_DIVIDE_BY_ZERO";
        case EXCEPTION_ILLEGAL_INSTRUCTION: return "ILLEGAL_INSTRUCTION";
        case EXCEPTION_IN_PAGE_ERROR: return "IN_PAGE_ERROR";
        case EXCEPTION_INT_DIVIDE_BY_ZERO: return "INT_DIVIDE_BY_ZERO";
        case EXCEPTION_PRIV_INSTRUCTION: return "PRIV_INSTRUCTION";
        case EXCEPTION_STACK_OVERFLOW: return "STACK_OVERFLOW";
        default: return "UNKNOWN";
        }
    }

    bool ShouldLogCode(DWORD code)
    {
        switch (code)
        {
        case EXCEPTION_ACCESS_VIOLATION:
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
        case EXCEPTION_DATATYPE_MISALIGNMENT:
        case EXCEPTION_FLT_DIVIDE_BY_ZERO:
        case EXCEPTION_ILLEGAL_INSTRUCTION:
        case EXCEPTION_IN_PAGE_ERROR:
        case EXCEPTION_INT_DIVIDE_BY_ZERO:
        case EXCEPTION_PRIV_INSTRUCTION:
        case EXCEPTION_STACK_OVERFLOW:
            return true;
        default:
            return false;
        }
    }

    bool IsExecutableProtect(DWORD protect)
    {
        protect &= 0xFF;
        return protect == PAGE_EXECUTE ||
               protect == PAGE_EXECUTE_READ ||
               protect == PAGE_EXECUTE_READWRITE ||
               protect == PAGE_EXECUTE_WRITECOPY;
    }

    bool AddressInSelf(uintptr_t address)
    {
        return g_selfBase != 0 && address >= g_selfBase && address < g_selfEnd;
    }

    void DescribeAddress(uintptr_t address, char* out, size_t outSize)
    {
        if (!out || outSize == 0)
            return;
        out[0] = '\0';

        HMODULE module = nullptr;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCSTR>(address), &module) && module)
        {
            char path[MAX_PATH]{};
            GetModuleFileNameA(module, path, MAX_PATH);
            const char* name = path;
            for (const char* p = path; *p; ++p)
                if (*p == '\\' || *p == '/')
                    name = p + 1;

            MODULEINFO mi{};
            uintptr_t base = reinterpret_cast<uintptr_t>(module);
            if (GetModuleInformation(GetCurrentProcess(), module, &mi, sizeof(mi)) && mi.lpBaseOfDll)
                base = reinterpret_cast<uintptr_t>(mi.lpBaseOfDll);

            sprintf_s(out, outSize, "%s+0x%llX", name,
                      static_cast<unsigned long long>(address - base));
            return;
        }

        sprintf_s(out, outSize, "0x%llX", static_cast<unsigned long long>(address));
    }

#if defined(_M_X64)
    uintptr_t ExceptionIp(PCONTEXT ctx)
    {
        return ctx ? static_cast<uintptr_t>(ctx->Rip) : 0;
    }
#else
    uintptr_t ExceptionIp(PCONTEXT)
    {
        return 0;
    }
#endif

#if defined(_M_X64)
    bool EnvEnabled(const char* name)
    {
        char value[16]{};
        DWORD n = GetEnvironmentVariableA(name, value, (DWORD)sizeof(value));
        return n > 0 && value[0] != '\0' && value[0] != '0';
    }

    bool TrySkipFaultingInstruction(PEXCEPTION_POINTERS p, char* reason, size_t reasonSize)
    {
        reason[0] = '\0';
        if (!g_skipEnabled.load(std::memory_order_relaxed))
        {
            sprintf_s(reason, reasonSize, "disabled");
            return false;
        }
        if (!p || !p->ExceptionRecord || !p->ContextRecord)
        {
            sprintf_s(reason, reasonSize, "missing context");
            return false;
        }
        DWORD code = p->ExceptionRecord->ExceptionCode;
        if (g_skipAvOnly.load(std::memory_order_relaxed) && code != EXCEPTION_ACCESS_VIOLATION)
        {
            sprintf_s(reason, reasonSize, "non-AV");
            return false;
        }

        uintptr_t ip = ExceptionIp(p->ContextRecord);
        if (!AddressInSelf(ip) && !g_skipExternal.load(std::memory_order_relaxed))
        {
            sprintf_s(reason, reasonSize, "outside AtomicHeartMenu.dll");
            return false;
        }

        thread_local int skipsThisThread = 0;
        if (skipsThisThread >= 8)
        {
            sprintf_s(reason, reasonSize, "thread skip budget exhausted");
            return false;
        }

        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(reinterpret_cast<void*>(ip), &mbi, sizeof(mbi)) ||
            mbi.State != MEM_COMMIT || !IsExecutableProtect(mbi.Protect))
        {
            sprintf_s(reason, reasonSize, "ip not executable");
            return false;
        }

        hde64s hs{};
        unsigned len = hde64_disasm(reinterpret_cast<void*>(ip), &hs);
        if (len == 0 || len > 15 || (hs.flags & F_ERROR))
        {
            sprintf_s(reason, reasonSize, "disasm failed len=%u flags=0x%X", len, hs.flags);
            return false;
        }

        p->ContextRecord->Rip += len;
        ++skipsThisThread;
        sprintf_s(reason, reasonSize, "skipped %u byte(s)", len);
        return true;
    }
#endif

    LONG CALLBACK VectoredHandler(PEXCEPTION_POINTERS p)
    {
        thread_local bool inHandler = false;
        if (inHandler || !p || !p->ExceptionRecord)
            return EXCEPTION_CONTINUE_SEARCH;

        inHandler = true;

        DWORD code = p->ExceptionRecord->ExceptionCode;
        uintptr_t ip = ExceptionIp(p->ContextRecord);
        uintptr_t faultAddr = 0;
        ULONG_PTR accessKind = 0;
        if ((code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR) &&
            p->ExceptionRecord->NumberParameters >= 2)
        {
            accessKind = p->ExceptionRecord->ExceptionInformation[0];
            faultAddr = static_cast<uintptr_t>(p->ExceptionRecord->ExceptionInformation[1]);
        }

        char ipDesc[512]{};
        char faultDesc[512]{};
        DescribeAddress(ip, ipDesc, sizeof(ipDesc));
        if (faultAddr)
            DescribeAddress(faultAddr, faultDesc, sizeof(faultDesc));
        else
            sprintf_s(faultDesc, sizeof(faultDesc), "n/a");

        bool logged = false;
        if (ShouldLogCode(code))
        {
            unsigned n = g_loggedExceptions.fetch_add(1);
            if (n < 64 || AddressInSelf(ip))
            {
                LOG("ExceptionGuard: %s code=0x%08lX ip=%s fault=%s access=%llu flags=0x%08lX",
                    ExceptionName(code), code, ipDesc, faultDesc,
                    static_cast<unsigned long long>(accessKind),
                    p->ExceptionRecord->ExceptionFlags);
                logged = true;
            }
        }

#if defined(_M_X64)
        char skipReason[128]{};
        if (TrySkipFaultingInstruction(p, skipReason, sizeof(skipReason)))
        {
            LOG("ExceptionGuard: instruction skipper continued at 0x%llX (%s)",
                static_cast<unsigned long long>(p->ContextRecord->Rip), skipReason);
            inHandler = false;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        if (logged && g_skipEnabled.load(std::memory_order_relaxed))
            LOG("ExceptionGuard: instruction skipper not used (%s)", skipReason);
#endif

        inHandler = false;
        return EXCEPTION_CONTINUE_SEARCH;
    }
}

void ExceptionGuard::Install()
{
    if (g_handler)
        return;

    if (G::hModule)
    {
        MODULEINFO mi{};
        if (GetModuleInformation(GetCurrentProcess(), reinterpret_cast<HMODULE>(G::hModule), &mi, sizeof(mi)) && mi.lpBaseOfDll)
        {
            g_selfBase = reinterpret_cast<uintptr_t>(mi.lpBaseOfDll);
            g_selfEnd = g_selfBase + mi.SizeOfImage;
        }
    }

#if defined(_M_X64)
    g_skipEnabled.store(EnvEnabled("AHM_SKIP_FAULTING_INSTRUCTION"), std::memory_order_relaxed);
    g_skipAvOnly.store(!EnvEnabled("AHM_SKIP_ALL_EXCEPTIONS"), std::memory_order_relaxed);
    g_skipExternal.store(EnvEnabled("AHM_SKIP_EXTERNAL_INSTRUCTIONS"), std::memory_order_relaxed);
#endif

    g_handler = AddVectoredExceptionHandler(1, VectoredHandler);
    LOG("ExceptionGuard: installed handler=%p self=[0x%llX..0x%llX)",
        g_handler,
        static_cast<unsigned long long>(g_selfBase),
        static_cast<unsigned long long>(g_selfEnd));

#if defined(_M_X64)
    LOG("ExceptionGuard: instruction skipper %s (%s, %s; set AHM_SKIP_FAULTING_INSTRUCTION=1 to enable)",
        g_skipEnabled.load(std::memory_order_relaxed) ? "enabled" : "disabled",
        g_skipAvOnly.load(std::memory_order_relaxed) ? "AV only" : "all logged exceptions",
        g_skipExternal.load(std::memory_order_relaxed) ? "external code allowed" : "self DLL only");
#else
    LOG("ExceptionGuard: instruction skipper unavailable in this build (x64 only)");
#endif
}

void ExceptionGuard::Remove()
{
    if (!g_handler)
        return;

    void* handler = g_handler;
    g_handler = nullptr;
    RemoveVectoredExceptionHandler(handler);
    LOG("ExceptionGuard: removed");
}

ExceptionGuard::InstructionSkipperOptions ExceptionGuard::GetInstructionSkipperOptions()
{
#if defined(_M_X64)
    return {
        g_skipEnabled.load(std::memory_order_relaxed),
        !g_skipAvOnly.load(std::memory_order_relaxed),
        g_skipExternal.load(std::memory_order_relaxed)
    };
#else
    return { false, false, false };
#endif
}

void ExceptionGuard::SetInstructionSkipperOptions(const InstructionSkipperOptions& options)
{
#if defined(_M_X64)
    g_skipEnabled.store(options.enabled, std::memory_order_relaxed);
    g_skipAvOnly.store(!options.skipAllExceptions, std::memory_order_relaxed);
    g_skipExternal.store(options.allowExternalInstructions, std::memory_order_relaxed);

    LOG("ExceptionGuard: instruction skipper runtime -> %s (%s, %s)",
        options.enabled ? "enabled" : "disabled",
        options.skipAllExceptions ? "all logged exceptions" : "AV only",
        options.allowExternalInstructions ? "external code allowed" : "self DLL only");
#else
    (void)options;
    LOG("ExceptionGuard: instruction skipper runtime request ignored (x64 only)");
#endif
}
