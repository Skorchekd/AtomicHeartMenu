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
#include "log.h"
#include <Windows.h>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <mutex>

namespace
{
    FILE*       g_file = nullptr;
    bool        g_console = false;
    bool        g_allocatedConsole = false;
    HANDLE      g_consoleOut = nullptr;
    char        g_path[MAX_PATH]{};
    std::mutex  g_mtx;

    void DisableConsoleCloseButton()
    {
        HWND hwnd = GetConsoleWindow();
        if (!hwnd) return;

        HMENU menu = GetSystemMenu(hwnd, FALSE);
        if (!menu) return;

        DeleteMenu(menu, SC_CLOSE, MF_BYCOMMAND);
        DrawMenuBar(hwnd);
    }
}

void Log::Init(bool allocConsole)
{
    char logPath[MAX_PATH]{};
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        g_console = allocConsole;
        if (allocConsole)
        {
            BOOL allocated = AllocConsole();
            if (allocated || GetLastError() == ERROR_ACCESS_DENIED)
            {
                g_allocatedConsole = allocated != FALSE;
                g_console = true;
                g_consoleOut = GetStdHandle(STD_OUTPUT_HANDLE);

                FILE* dummy = nullptr;
                freopen_s(&dummy, "CONOUT$", "w", stdout);
                freopen_s(&dummy, "CONOUT$", "w", stderr);
                setvbuf(stdout, nullptr, _IONBF, 0);
                setvbuf(stderr, nullptr, _IONBF, 0);

                SetConsoleOutputCP(CP_UTF8);
                SetConsoleTitleA("AtomicHeartMenu live log - copy from here");
                DisableConsoleCloseButton();
            }
            else
            {
                g_console = false;
                g_consoleOut = nullptr;
            }
        }

        char exePath[MAX_PATH]{};
        DWORD len = GetModuleFileNameA(nullptr, exePath, MAX_PATH); // path to the game exe
        if (len > 0 && len < MAX_PATH)
        {
            strcpy_s(logPath, exePath);
            char* slash = strrchr(logPath, '\\');
            char* slash2 = strrchr(logPath, '/');
            if (slash2 && (!slash || slash2 > slash)) slash = slash2;
            if (slash)
            {
                slash[1] = '\0';
                strcat_s(logPath, "AtomicHeartMenu.log");
                fopen_s(&g_file, logPath, "w");
            }
        }

        if (!g_file)
        {
            char tempPath[MAX_PATH]{};
            if (GetTempPathA(MAX_PATH, tempPath) > 0)
            {
                strcpy_s(logPath, tempPath);
                strcat_s(logPath, "AtomicHeartMenu.log");
                fopen_s(&g_file, logPath, "w");
            }
        }
    }

    if (logPath[0]) strcpy_s(g_path, logPath);
    Write("==== AtomicHeartMenu log start ====");
    if (g_file) Write("Log path: %s", logPath);
}

void Log::Shutdown()
{
    std::lock_guard<std::mutex> lk(g_mtx);
    if (g_file) { fclose(g_file); g_file = nullptr; }
    if (g_allocatedConsole) { FreeConsole(); g_allocatedConsole = false; }
    g_console = false;
    g_consoleOut = nullptr;
}

void Log::Clear()
{
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        if (g_file) { fclose(g_file); g_file = nullptr; }
        if (g_path[0]) fopen_s(&g_file, g_path, "w");
    }
    Write("==== AtomicHeartMenu log cleared from Hook Diagnostics ====");
}

void Log::Write(const char* fmt, ...)
{
    std::lock_guard<std::mutex> lk(g_mtx);

    char buf[2048];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    SYSTEMTIME st{};
    GetLocalTime(&st);

    char line[2300];
    sprintf_s(line, "%02u:%02u:%02u.%03u [T%lu] %s",
        st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
        GetCurrentThreadId(), buf);

    if (g_console && g_consoleOut && g_consoleOut != INVALID_HANDLE_VALUE)
    {
        char con[2400];
        sprintf_s(con, "[AHM] %s\r\n", line);
        DWORD written = 0;
        WriteConsoleA(g_consoleOut, con, (DWORD)strlen(con), &written, nullptr);
    }
    if (g_file) { fprintf(g_file, "%s\n", line); fflush(g_file); }

    char dbg[2300];
    sprintf_s(dbg, "[AHM] %s\n", line);
    OutputDebugStringA(dbg);
}
