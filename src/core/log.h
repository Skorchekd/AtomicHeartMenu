#pragma once
#include <string>

// Lightweight logging. Writes to a file next to the game and (optionally) a
// debug console. Safe to call from any thread.
namespace Log
{
    void Init(bool allocConsole);
    void Shutdown();
    void Write(const char* fmt, ...);
}

#define LOG(...)  ::Log::Write(__VA_ARGS__)
