#pragma once

// Installs the DirectX 12 render hook (Present / ResizeBuffers /
// ExecuteCommandLists) via MinHook and drives the ImGui overlay.
namespace DX12Hook
{
    bool Install();   // grab vtables, create MinHook hooks, enable
    void Remove();    // disable hooks, tear down ImGui + D3D objects
}
