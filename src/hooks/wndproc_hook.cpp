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
#include "wndproc_hook.h"
#include "../core/globals.h"
#include "../core/log.h"
#include "../features/features.h"
#include "imgui.h"
#include "backends/imgui_impl_win32.h"
#include <exception>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace
{
    WNDPROC g_originalWndProc = nullptr;
    HWND    g_hwnd = nullptr;
    std::atomic<bool> g_installed{ false }; // read by the eject poll on the main thread
    bool    g_wasMenuOpen = false;
    std::recursive_mutex g_inputMutex;
    ULONGLONG g_lastInsertToggleMs = 0;
    const LPCSTR kArrowCursor = MAKEINTRESOURCEA(32512);
    constexpr ULONGLONG kOverlayInputGraceMs = 750;
    constexpr ULONGLONG kStaleMenuCloseMs = 2000;

    // True while a menu text field has keyboard focus; End then moves the caret
    // instead of ejecting.
    bool MenuTypingText()
    {
        if (!G::menuOpen.load())
            return false;
        std::lock_guard<std::recursive_mutex> inputLock(g_inputMutex);
        return ImGui::GetCurrentContext() && ImGui::GetIO().WantTextInput;
    }

    bool OverlayRecentlyDrawn(ULONGLONG now = GetTickCount64())
    {
        ULONGLONG lastDrawMs = G::overlayLastDrawMs.load();
        return lastDrawMs != 0 && now - lastDrawMs <= kOverlayInputGraceMs;
    }

    bool MenuCanCaptureInput()
    {
        if (!G::menuOpen.load())
            return false;

        ULONGLONG now = GetTickCount64();
        ULONGLONG lastDrawMs = G::overlayLastDrawMs.load();
        if (lastDrawMs != 0 && now - lastDrawMs > kStaleMenuCloseMs)
        {
            G::menuOpen = false;
            LOG("Menu auto-closed after stale overlay draw state (%llums).", now - lastDrawMs);
            return false;
        }

        return OverlayRecentlyDrawn(now);
    }

    // ONLY cooked mouse messages are ever swallowed. Keyboard (cooked AND raw) is
    // NEVER swallowed -- the game must always receive Space/WASD/etc. so a stale
    // menu-capture state (e.g. the overlay still drawing over a death screen)
    // can never lock the player out of the "press Space to respawn" prompt. That
    // lockout (had to restart the game) is exactly what this guards against.
    bool IsBlockedMouseMessage(UINT msg)
    {
        switch (msg)
        {
        case WM_MOUSEMOVE:
        case WM_MOUSEWHEEL:
        case WM_MOUSEHWHEEL:
        case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
        case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK:
        case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MBUTTONDBLCLK:
        case WM_XBUTTONDOWN: case WM_XBUTTONUP: case WM_XBUTTONDBLCLK:
            return true;
        default:
            return false;
        }
    }

    // WM_INPUT carries either raw mouse OR raw keyboard. We only want to swallow
    // raw MOUSE (to stop the camera spinning while the menu is open); raw KEYBOARD
    // must pass through so gameplay/menu key actions (Space respawn) still work.
    bool IsRawKeyboard(LPARAM lParam)
    {
        RAWINPUTHEADER hdr{};
        UINT size = sizeof(hdr);
        if (GetRawInputData((HRAWINPUT)lParam, RID_HEADER, &hdr, &size, sizeof(RAWINPUTHEADER)) == (UINT)-1)
            return false; // unknown -> treat as non-keyboard (safe to swallow as mouse)
        return hdr.dwType == RIM_TYPEKEYBOARD;
    }

    // How many net ShowCursor(TRUE) calls WE applied. We MUST undo exactly this many
    // on close, or the OS cursor display counter (and the game's input/cursor state
    // that rides on it) stays desynced -> fire/abilities/weapon-switch dead even
    // after the menu is closed. That leak was the "open/close breaks left click" bug.
    int g_cursorShows = 0;

    void RaiseCursor()
    {
        // Bring the cursor to visible, counting every TRUE so we can undo it exactly.
        for (int guard = 0; guard < 16; ++guard)
        {
            int c = ShowCursor(TRUE);
            ++g_cursorShows;
            if (c >= 0) break;
        }
    }

    void RestoreCursor()
    {
        // Undo exactly our shows so the game's hidden-cursor/gameplay-input state returns.
        while (g_cursorShows > 0) { ShowCursor(FALSE); --g_cursorShows; }
        if (GetCapture() == g_hwnd)
            ReleaseCapture();
        // Don't leave the cursor unclipped; the game re-clips during gameplay, but
        // clearing here avoids a stale clip across the close.
        ClipCursor(nullptr);
    }

    void ApplyMenuCursorState()
    {
        bool open = MenuCanCaptureInput();
        if (open)
        {
            ClipCursor(nullptr);
            if (GetCapture() == g_hwnd)
                ReleaseCapture();
            SetCursor(LoadCursorA(nullptr, kArrowCursor));

            if (!g_wasMenuOpen)
                RaiseCursor(); // open edge: show the cursor (balanced)
        }
        else if (g_wasMenuOpen)
        {
            RestoreCursor(); // close edge: FULLY restore OS cursor/capture state
        }

        g_wasMenuOpen = open;
    }

    LRESULT CALLBACK Hooked(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        G::HookScope hookScope;
        WNDPROC original = g_originalWndProc;
        // Eject in progress: forward everything untouched. ImGui may already be gone.
        if (!G::running.load())
        {
            return original ? CallWindowProc(original, hWnd, msg, wParam, lParam)
                            : DefWindowProc(hWnd, msg, wParam, lParam);
        }

        // UE4 pumps window messages on the game thread, so THIS is the game thread.
        // Pin it so the ProcessEvent pump never runs a spawn on a loading/audio worker.
        Features::NoteGameThread();
        // Feature hotkeys (numpad by default). Keyboard is only observed, never
        // swallowed, so the game still receives every key.
        if (msg == WM_KEYDOWN && (lParam & 0x40000000) == 0)
            Features::NoteHotkey(static_cast<int>(wParam));
        // Eject on the dedicated End key only. With NumLock off, numpad 1 (one of the
        // hotkeys above) also arrives as VK_END, but without the extended-key bit.
        if (msg == WM_KEYDOWN && wParam == VK_END && (lParam & 0x01000000) != 0 && (lParam & 0x40000000) == 0 &&
            !MenuTypingText())
            G::ejectRequested = true;
        try
        {
            // Toggle on key-down of INSERT or F7.
            if (msg == WM_KEYDOWN && (wParam == VK_INSERT || wParam == VK_F7))
            {
                if ((lParam & 0x40000000) == 0)
                {
                    ULONGLONG nowMs = GetTickCount64();
                    if (nowMs - g_lastInsertToggleMs < 180)
                        return 0;
                    g_lastInsertToggleMs = nowMs;

                    bool now = !G::menuOpen.load();
                    G::menuOpen = now;
                    if (now)
                        G::overlayLastDrawMs = nowMs; // opening should not be killed by an old ESP/menu draw timestamp
                    ApplyMenuCursorState();
                    LOG("Menu %s", G::menuOpen.load() ? "opened" : "closed");
                }
                return 0;
            }
            if ((msg == WM_KEYUP || msg == WM_SYSKEYUP) && (wParam == VK_INSERT || wParam == VK_F7))
                return 0;

            if (G::menuOpen.load())
            {
                bool captureInput = MenuCanCaptureInput();
                ApplyMenuCursorState();
                if (captureInput)
                {
                    {
                        // The render thread consumes this queue inside NewFrame under
                        // the same lock; unsynchronised, the two corrupt ImGui's
                        // event vector while the menu is open.
                        std::lock_guard<std::recursive_mutex> inputLock(g_inputMutex);
                        if (ImGui::GetCurrentContext())
                            ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam);
                    }
                    if (msg == WM_SETCURSOR)
                    {
                        SetCursor(LoadCursorA(nullptr, kArrowCursor));
                        return TRUE;
                    }
                    // Swallow MOUSE only -- never keyboard. Raw input is split:
                    // raw mouse is swallowed (no camera spin), raw keyboard passes
                    // through so the game always gets keys.
                    if (msg == WM_INPUT)
                    {
                        if (!IsRawKeyboard(lParam))
                            // Swallow raw mouse from the GAME, but STILL call
                            // DefWindowProc so the system performs the required WM_INPUT
                            // cleanup. Returning 0 here (the old code) skipped that
                            // cleanup -> the raw-input buffer leaked -> after a bit of
                            // mouse movement, raw-input delivery STALLED for the whole
                            // process, so the game stopped seeing keyboard (V / shock /
                            // weapon-switch) and clicks even after the menu was closed.
                            // That was the "open/close breaks left-click + V" bug.
                            return DefWindowProc(hWnd, msg, wParam, lParam);
                        // raw keyboard -> fall through to the game
                    }
                    else if (IsBlockedMouseMessage(msg))
                    {
                        return 0;
                    }
                }
            }
        }
        catch (const std::exception& e) { LOG("WndProc: std::exception ignored: %s", e.what()); }
        catch (...) { LOG("WndProc: exception ignored."); }

        return original
            ? CallWindowProc(original, hWnd, msg, wParam, lParam)
            : DefWindowProc(hWnd, msg, wParam, lParam);
    }
}

std::recursive_mutex& WndProcHook::InputMutex()
{
    return g_inputMutex;
}

bool WndProcHook::IsTopLevel()
{
    if (!g_hwnd || !g_originalWndProc)
        return false;
    return reinterpret_cast<WNDPROC>(GetWindowLongPtr(g_hwnd, GWLP_WNDPROC)) == Hooked;
}

void WndProcHook::Install(HWND hwnd)
{
    if (g_originalWndProc || !hwnd) return;
    g_hwnd = hwnd;
    g_originalWndProc = (WNDPROC)SetWindowLongPtr(hwnd, GWLP_WNDPROC, (LONG_PTR)Hooked);
    g_installed = g_originalWndProc != nullptr;
    LOG("WndProc hooked (hwnd=%p, orig=%p)", hwnd, g_originalWndProc);
}

bool WndProcHook::IsInstalled()
{
    return g_installed.load();
}

void WndProcHook::Tick()
{
    // Intentionally does NOT manage the cursor. The cursor/ShowCursor counter is
    // per-thread and tied to the WINDOW thread that created the HWND; driving it
    // from the render thread (where this Tick runs) was ineffective AND unbalanced
    // the counter -> the cursor/input state never restored on close, which is the
    // "open/close breaks left-click + V + weapon-switch" bug. Cursor state is now
    // managed exclusively from the WndProc (window thread) on input messages.
}

bool WndProcHook::Remove()
{
    if (g_originalWndProc && g_hwnd)
    {
        if (GetCapture() == g_hwnd)
            ReleaseCapture();
        if (!IsTopLevel())
        {
            // Another hook subclassed after ours and still calls into it. Restoring
            // our saved original would cut that hook out of the chain, so leave ours
            // in place: it forwards untouched while ImGui is gone.
            LOG("WndProc: another hook subclassed after ours; leaving the chain intact.");
            return false;
        }
        SetWindowLongPtr(g_hwnd, GWLP_WNDPROC, (LONG_PTR)g_originalWndProc);
        g_installed = false;
        g_originalWndProc = nullptr;
        g_wasMenuOpen = false;
        LOG("WndProc restored");
    }
    return true;
}
