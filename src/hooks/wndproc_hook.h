#pragma once
#include <Windows.h>

// Subclasses the game window so ImGui sees raw input and the INSERT key toggles
// the menu. Installed once we know the swapchain's HWND.
namespace WndProcHook
{
    void Install(HWND hwnd);
    void Tick();
    void Remove();
}
