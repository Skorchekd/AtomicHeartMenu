#pragma once
#include <cstdint>
#include <atomic>

// Process-wide state shared between the injector thread, the render hook and the
// feature loop.
namespace G
{
    inline std::atomic<bool> running{ true };   // cleared on eject to unwind hooks
    inline std::atomic<bool> menuOpen{ false }; // toggled with INSERT
    inline std::atomic<bool> sdkReady{ false }; // engine globals resolved
    inline std::atomic<uint64_t> overlayLastDrawMs{ 0 }; // menu input block only while recently visible

    inline void*    hModule   = nullptr;        // our injected DLL base
    inline uint8_t* moduleBase = nullptr;       // game exe base (0x140000000)
    inline size_t   moduleSize = 0;
    inline void*    hGameWindow = nullptr;       // HWND of the game
}
