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
#include "dx12_hook.h"
#include "wndproc_hook.h"
#include "../core/globals.h"
#include "../core/log.h"
#include "../menu/menu.h"
#include "../features/features.h"
#include "../sdk/ue4.h"

#include <Windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <MinHook.h>
#include <cstdio>
#include <exception>

#include "imgui.h"
#include "backends/imgui_impl_win32.h"
#include "backends/imgui_impl_dx12.h"

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")

namespace
{
    // ---- hook trampolines --------------------------------------------------
    typedef HRESULT(WINAPI* Present_t)(IDXGISwapChain3*, UINT, UINT);
    typedef HRESULT(WINAPI* ResizeBuffers_t)(IDXGISwapChain3*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
    typedef void   (WINAPI* ExecuteCommandLists_t)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);

    Present_t             oPresent = nullptr;
    ResizeBuffers_t       oResizeBuffers = nullptr;
    ExecuteCommandLists_t oExecuteCommandLists = nullptr;
    void*                 g_presentTarget = nullptr;
    void*                 g_resizeTarget = nullptr;
    void*                 g_executeTarget = nullptr;

    // ---- D3D12 state -------------------------------------------------------
    struct FrameContext
    {
        ID3D12CommandAllocator*     CommandAllocator = nullptr;
        ID3D12Resource*             RenderTarget = nullptr;
        D3D12_CPU_DESCRIPTOR_HANDLE RenderTargetDescriptor{};
        UINT64                      FenceValue = 0;
    };

    constexpr UINT SRV_HEAP_SIZE = 64;

    bool                        g_init = false;
    ID3D12Device*               g_device = nullptr;
    ID3D12DescriptorHeap*       g_rtvHeap = nullptr;
    ID3D12DescriptorHeap*       g_srvHeap = nullptr;
    ID3D12CommandQueue*         g_commandQueue = nullptr;
    ID3D12GraphicsCommandList*  g_commandList = nullptr;
    ID3D12Fence*                g_fence = nullptr;
    HANDLE                      g_fenceEvent = nullptr;
    UINT64                      g_fenceLastValue = 0;
    FrameContext*               g_frames = nullptr;
    UINT                        g_buffersCount = 0;
    HWND                        g_hwnd = nullptr;
    bool                        g_imguiReady = false;

    template <typename T>
    void SafeRelease(T*& ptr)
    {
        if (ptr)
        {
            ptr->Release();
            ptr = nullptr;
        }
    }

    bool WaitForFenceValue(UINT64 value, DWORD timeoutMs)
    {
        if (!g_fence || value == 0) return true;
        if (g_fence->GetCompletedValue() >= value) return true;

        if (!g_fenceEvent) return false;
        HRESULT hr = g_fence->SetEventOnCompletion(value, g_fenceEvent);
        if (FAILED(hr)) return false;

        DWORD wait = WaitForSingleObject(g_fenceEvent, timeoutMs);
        return wait == WAIT_OBJECT_0;
    }

    bool WaitForFrame(FrameContext& fc)
    {
        if (!WaitForFenceValue(fc.FenceValue, 0))
            return false;
        fc.FenceValue = 0;
        return true;
    }

    void WaitForAllFrames()
    {
        if (!g_frames) return;
        for (UINT i = 0; i < g_buffersCount; ++i)
        {
            WaitForFenceValue(g_frames[i].FenceValue, 100);
            g_frames[i].FenceValue = 0;
        }
    }

    void SignalFrame(FrameContext& fc)
    {
        if (!g_commandQueue || !g_fence) return;
        UINT64 value = ++g_fenceLastValue;
        HRESULT hr = g_commandQueue->Signal(g_fence, value);
        if (SUCCEEDED(hr))
        {
            fc.FenceValue = value;
        }
        else
        {
            static bool logged = false;
            if (!logged)
            {
                LOG("SignalFrame: fence signal failed hr=0x%lX", hr);
                logged = true;
            }
        }
    }

    void LogStreamlineStateOnce()
    {
        static bool logged = false;
        if (logged) return;
        logged = true;

        if (GetModuleHandleA("sl.interposer.dll") || GetModuleHandleA("nvngx_dlssg.dll"))
            LOG("NVIDIA Streamline/DLSSG detected; using fenced D3D12 overlay path.");
    }

    bool HookStatusOk(MH_STATUS st)
    {
        return st == MH_OK || st == MH_ERROR_ENABLED || st == MH_ERROR_ALREADY_CREATED;
    }

    ImU32 OverlayColor(const float rgb[3], bool rainbow)
    {
        if (rainbow)
        {
            float hue = (float)((GetTickCount64() % 6000) / 6000.0);
            return ImColor::HSV(hue, 0.85f, 1.0f, 1.0f);
        }
        return ImGui::ColorConvertFloat4ToU32(ImVec4(rgb[0], rgb[1], rgb[2], 1.0f));
    }

    void DrawCornerBox(ImDrawList* dl, const Features::EspEntry& e, ImU32 color)
    {
        float x = e.x, y = e.y, w = e.w, h = e.h;
        float lx = w * 0.32f;
        float ly = h * 0.24f;
        dl->AddLine(ImVec2(x, y), ImVec2(x + lx, y), color, 1.5f);
        dl->AddLine(ImVec2(x, y), ImVec2(x, y + ly), color, 1.5f);
        dl->AddLine(ImVec2(x + w, y), ImVec2(x + w - lx, y), color, 1.5f);
        dl->AddLine(ImVec2(x + w, y), ImVec2(x + w, y + ly), color, 1.5f);
        dl->AddLine(ImVec2(x, y + h), ImVec2(x + lx, y + h), color, 1.5f);
        dl->AddLine(ImVec2(x, y + h), ImVec2(x, y + h - ly), color, 1.5f);
        dl->AddLine(ImVec2(x + w, y + h), ImVec2(x + w - lx, y + h), color, 1.5f);
        dl->AddLine(ImVec2(x + w, y + h), ImVec2(x + w, y + h - ly), color, 1.5f);
    }

    void DrawEspOverlay()
    {
        auto& f = Features::Get();
        ImDrawList* dl = ImGui::GetForegroundDrawList();
        ImGuiIO& io = ImGui::GetIO();
        const float sw = io.DisplaySize.x;
        const float sh = io.DisplaySize.y;

        if (f.crosshair)
        {
            ImU32 color = OverlayColor(f.crosshairColor, f.espRainbow);
            ImVec2 c(sw * 0.5f, sh * 0.5f);
            dl->AddLine(ImVec2(c.x - 7.0f, c.y), ImVec2(c.x + 7.0f, c.y), color, 1.5f);
            dl->AddLine(ImVec2(c.x, c.y - 7.0f), ImVec2(c.x, c.y + 7.0f), color, 1.5f);
        }

        if (!f.espEnabled)
            return;

        ImU32 color = OverlayColor(f.espColor, f.espRainbow);
        ImU32 shadow = IM_COL32(0, 0, 0, 170);
        ImU32 healthBg = IM_COL32(12, 12, 12, 190);
        ImU32 healthFg = IM_COL32(60, 235, 95, 230);
        const auto& frame = Features::BuildEspFrame(sw, sh);
        char text[32]{};

        const ImU32 squadCol = IM_COL32(70, 235, 120, 255);  // green = under our control
        const ImU32 selCol   = IM_COL32(60, 200, 255, 255);  // cyan  = selected

        for (const Features::EspEntry& e : frame)
        {
            // --- controlled / selected highlight: glow box + bobbing arrow overhead --
            if (e.inSquad || e.selected)
            {
                ImU32 hi = e.selected ? selCol : squadCol;
                for (int g = 1; g <= 3; ++g)
                {
                    ImU32 glow = (hi & 0x00FFFFFFu) | ((ImU32)(70 / g) << 24);
                    float pad = (float)(g * 2);
                    dl->AddRect(ImVec2(e.x - pad, e.y - pad), ImVec2(e.x + e.w + pad, e.y + e.h + pad), glow, 3.0f, 0, 2.0f);
                }
                dl->AddRect(ImVec2(e.x, e.y), ImVec2(e.x + e.w, e.y + e.h), hi, 2.0f, 0, 2.2f);
                float bob = (float)((GetTickCount64() % 1000) / 1000.0) * 6.0f;
                float ax = e.headX;
                float ay = e.headY - 16.0f - bob;
                dl->AddTriangleFilled(ImVec2(ax - 7, ay - 10), ImVec2(ax + 7, ay - 10), ImVec2(ax, ay), hi);
                dl->AddTriangle(ImVec2(ax - 7, ay - 10), ImVec2(ax + 7, ay - 10), ImVec2(ax, ay), IM_COL32(0, 0, 0, 200), 1.5f);
            }

            if (f.espFilled)
            {
                int a = (int)(f.espFillAlpha * 255.0f);
                if (a < 0) a = 0; if (a > 255) a = 255;
                ImU32 fill = (color & 0x00FFFFFFu) | ((ImU32)a << 24); // chams-style solid fill (drawn through walls)
                dl->AddRectFilled(ImVec2(e.x, e.y), ImVec2(e.x + e.w, e.y + e.h), fill);
            }
            if (f.espBox)
            {
                dl->AddRect(ImVec2(e.x - 1.0f, e.y - 1.0f), ImVec2(e.x + e.w + 1.0f, e.y + e.h + 1.0f), shadow);
                dl->AddRect(ImVec2(e.x, e.y), ImVec2(e.x + e.w, e.y + e.h), color, 0.0f, 0, 1.4f);
            }
            if (f.espCornerBox)
                DrawCornerBox(dl, e, color);
            if (f.espSnapline)
                dl->AddLine(ImVec2(sw * 0.5f, sh - 2.0f), ImVec2(e.feetX, e.feetY), color, 1.0f);
            if (f.espHealthbar && e.healthFrac >= 0.0f)
            {
                float barX = e.x - 5.0f;
                float fillH = e.h * e.healthFrac;
                dl->AddRectFilled(ImVec2(barX, e.y), ImVec2(barX + 3.0f, e.y + e.h), healthBg);
                dl->AddRectFilled(ImVec2(barX, e.y + e.h - fillH), ImVec2(barX + 3.0f, e.y + e.h), healthFg);
            }
            if (f.espDistance)
            {
                std::snprintf(text, sizeof(text), "%.0fm", e.distance);
                ImVec2 pos(e.x + e.w * 0.5f, e.y + e.h + 2.0f);
                ImVec2 sz = ImGui::CalcTextSize(text);
                dl->AddText(ImVec2(pos.x - sz.x * 0.5f + 1.0f, pos.y + 1.0f), shadow, text);
                dl->AddText(ImVec2(pos.x - sz.x * 0.5f, pos.y), color, text);
            }
        }
    }

    void ResetHookTargets()
    {
        g_presentTarget = nullptr;
        g_resizeTarget = nullptr;
        g_executeTarget = nullptr;
    }

    void RemoveCreatedHooks()
    {
        if (g_presentTarget)
            MH_RemoveHook(g_presentTarget);
        if (g_resizeTarget)
            MH_RemoveHook(g_resizeTarget);
        if (g_executeTarget)
            MH_RemoveHook(g_executeTarget);
        ResetHookTargets();
    }

    // ---- shader-visible SRV heap free-list allocator (ImGui 1.92 needs it) --
    struct SrvHeapAllocator
    {
        ID3D12DescriptorHeap*       Heap = nullptr;
        D3D12_CPU_DESCRIPTOR_HANDLE CpuStart{};
        D3D12_GPU_DESCRIPTOR_HANDLE GpuStart{};
        UINT                        Increment = 0;
        ImVector<int>               Free;

        void Create(ID3D12Device* dev, ID3D12DescriptorHeap* heap)
        {
            Heap = heap;
            auto d = heap->GetDesc();
            CpuStart = heap->GetCPUDescriptorHandleForHeapStart();
            GpuStart = heap->GetGPUDescriptorHandleForHeapStart();
            Increment = dev->GetDescriptorHandleIncrementSize(d.Type);
            Free.reserve((int)d.NumDescriptors);
            for (int n = (int)d.NumDescriptors; n > 0; --n) Free.push_back(n - 1);
        }
        void Destroy() { Heap = nullptr; Free.clear(); }
        void Alloc(D3D12_CPU_DESCRIPTOR_HANDLE* cpu, D3D12_GPU_DESCRIPTOR_HANDLE* gpu)
        {
            IM_ASSERT(Free.Size > 0);
            int idx = Free.back(); Free.pop_back();
            cpu->ptr = CpuStart.ptr + (UINT64)idx * Increment;
            gpu->ptr = GpuStart.ptr + (UINT64)idx * Increment;
        }
        void Dealloc(D3D12_CPU_DESCRIPTOR_HANDLE cpu, D3D12_GPU_DESCRIPTOR_HANDLE)
        {
            int idx = (int)((cpu.ptr - CpuStart.ptr) / Increment);
            Free.push_back(idx);
        }
    } g_srvAlloc;

    void CleanupRenderTargets()
    {
        if (!g_frames) return;
        for (UINT i = 0; i < g_buffersCount; ++i)
        {
            if (g_frames[i].RenderTarget) { g_frames[i].RenderTarget->Release(); g_frames[i].RenderTarget = nullptr; }
        }
    }

    bool CreateRenderTargets(IDXGISwapChain3* sc)
    {
        UINT rtvSize = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        D3D12_CPU_DESCRIPTOR_HANDLE handle = g_rtvHeap->GetCPUDescriptorHandleForHeapStart();
        for (UINT i = 0; i < g_buffersCount; ++i)
        {
            g_frames[i].RenderTargetDescriptor = handle;
            if (FAILED(sc->GetBuffer(i, IID_PPV_ARGS(&g_frames[i].RenderTarget)))) return false;
            g_device->CreateRenderTargetView(g_frames[i].RenderTarget, nullptr, handle);
            handle.ptr += rtvSize;
        }
        return true;
    }

    bool InitImGui(IDXGISwapChain3* sc)
    {
        LogStreamlineStateOnce();

        if (FAILED(sc->GetDevice(IID_PPV_ARGS(&g_device)))) { LOG("InitImGui: GetDevice failed"); return false; }

        DXGI_SWAP_CHAIN_DESC desc{};
        sc->GetDesc(&desc);
        g_buffersCount = desc.BufferCount;
        g_hwnd = desc.OutputWindow;
        G::hGameWindow = g_hwnd;

        // SRV heap (shader visible) for ImGui textures.
        D3D12_DESCRIPTOR_HEAP_DESC srvDesc{};
        srvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        srvDesc.NumDescriptors = SRV_HEAP_SIZE;
        srvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(g_device->CreateDescriptorHeap(&srvDesc, IID_PPV_ARGS(&g_srvHeap)))) return false;
        g_srvAlloc.Create(g_device, g_srvHeap);

        // RTV heap (one per back buffer).
        D3D12_DESCRIPTOR_HEAP_DESC rtvDesc{};
        rtvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        rtvDesc.NumDescriptors = g_buffersCount;
        rtvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        if (FAILED(g_device->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&g_rtvHeap)))) return false;

        g_frames = new FrameContext[g_buffersCount];
        for (UINT i = 0; i < g_buffersCount; ++i)
        {
            HRESULT hr = g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_frames[i].CommandAllocator));
            if (FAILED(hr) || !g_frames[i].CommandAllocator)
            {
                LOG("InitImGui: CreateCommandAllocator[%u] failed hr=0x%lX", i, hr);
                return false;
            }
        }

        if (!CreateRenderTargets(sc)) return false;

        HRESULT clHr = g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_frames[0].CommandAllocator, nullptr, IID_PPV_ARGS(&g_commandList));
        if (FAILED(clHr) || !g_commandList)
        {
            LOG("InitImGui: CreateCommandList failed hr=0x%lX", clHr);
            return false;
        }
        g_commandList->Close();

        HRESULT fenceHr = g_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence));
        if (FAILED(fenceHr) || !g_fence)
        {
            LOG("InitImGui: CreateFence failed hr=0x%lX", fenceHr);
            return false;
        }
        g_fenceEvent = CreateEventA(nullptr, FALSE, FALSE, nullptr);
        if (!g_fenceEvent)
        {
            LOG("InitImGui: CreateEvent failed err=%lu", GetLastError());
            return false;
        }

        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        ImGui::StyleColorsDark();
        ImGui_ImplWin32_Init(g_hwnd);

        ImGui_ImplDX12_InitInfo init{};
        init.Device = g_device;
        init.CommandQueue = g_commandQueue;
        init.NumFramesInFlight = (int)g_buffersCount;
        init.RTVFormat = desc.BufferDesc.Format;
        init.SrvDescriptorHeap = g_srvHeap;
        init.SrvDescriptorAllocFn = [](ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE* c, D3D12_GPU_DESCRIPTOR_HANDLE* g) { g_srvAlloc.Alloc(c, g); };
        init.SrvDescriptorFreeFn  = [](ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE c, D3D12_GPU_DESCRIPTOR_HANDLE g) { g_srvAlloc.Dealloc(c, g); };
        if (!ImGui_ImplDX12_Init(&init)) { LOG("ImGui_ImplDX12_Init failed"); return false; }

        WndProcHook::Install(g_hwnd);
        g_imguiReady = true;
        LOG("ImGui DX12 initialised (%u buffers, fmt=%d, hwnd=%p)", g_buffersCount, desc.BufferDesc.Format, g_hwnd);
        return true;
    }

    void ReleaseOverlayResources()
    {
        WaitForAllFrames();
        WndProcHook::Remove();

        if (g_imguiReady)
        {
            ImGui_ImplDX12_Shutdown();
            ImGui_ImplWin32_Shutdown();
            ImGui::DestroyContext();
            g_imguiReady = false;
        }

        CleanupRenderTargets();
        if (g_frames)
        {
            for (UINT i = 0; i < g_buffersCount; ++i)
                SafeRelease(g_frames[i].CommandAllocator);
            delete[] g_frames;
            g_frames = nullptr;
        }

        SafeRelease(g_commandList);
        SafeRelease(g_fence);
        if (g_fenceEvent)
        {
            CloseHandle(g_fenceEvent);
            g_fenceEvent = nullptr;
        }
        g_fenceLastValue = 0;

        SafeRelease(g_rtvHeap);
        if (g_srvHeap)
        {
            g_srvAlloc.Destroy();
            SafeRelease(g_srvHeap);
        }
        SafeRelease(g_device);

        g_buffersCount = 0;
        g_hwnd = nullptr;
        G::hGameWindow = nullptr;
        g_init = false;
    }

    // ---- hooked Present ----------------------------------------------------
    HRESULT WINAPI hkPresent(IDXGISwapChain3* sc, UINT sync, UINT flags)
    {
        if (!oPresent) return DXGI_ERROR_INVALID_CALL;
        if (!G::running.load()) return oPresent(sc, sync, flags);

        // Everything below touches D3D + game state. /EHa means catch(...)
        // also traps access violations, so injecting during a loading screen or
        // menu (renderer mid-init) degrades to "skip our frame", never a crash.
        try
        {
            if (!g_init)
            {
                if (!g_commandQueue) return oPresent(sc, sync, flags); // wait for queue capture
                if (InitImGui(sc)) g_init = true;
                else
                {
                    ReleaseOverlayResources();
                    return oPresent(sc, sync, flags);
                }
            }

            Features::Tick();   // apply cheats for this frame

            const auto& featureState = Features::Get();
            bool wantsOverlay = G::menuOpen.load() ||
                (G::sdkReady.load() &&
                    (featureState.showCoords || featureState.espEnabled || featureState.crosshair));
            if (!wantsOverlay)
                return oPresent(sc, sync, flags);

            ImGui_ImplDX12_NewFrame();
            ImGui_ImplWin32_NewFrame();
            ImGui::NewFrame();
            ImGui::GetIO().MouseDrawCursor = G::menuOpen.load();

            Menu::Render();
            DrawEspOverlay();

            ImGui::Render();
            G::overlayLastDrawMs = GetTickCount64();
            WndProcHook::Tick();

            UINT idx = sc->GetCurrentBackBufferIndex();
            if (idx >= g_buffersCount) return oPresent(sc, sync, flags);
            FrameContext& fc = g_frames[idx];
            if (!fc.CommandAllocator || !fc.RenderTarget || !g_commandList || !g_commandQueue)
                return oPresent(sc, sync, flags);
            if (!WaitForFrame(fc))
            {
                static bool loggedWait = false;
                if (!loggedWait)
                {
                    LOG("hkPresent: timed out waiting for previous overlay frame; skipping draw.");
                    loggedWait = true;
                }
                return oPresent(sc, sync, flags);
            }
            fc.CommandAllocator->Reset();

            D3D12_RESOURCE_BARRIER barrier{};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
            barrier.Transition.pResource = fc.RenderTarget;
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
            barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;

            g_commandList->Reset(fc.CommandAllocator, nullptr);
            g_commandList->ResourceBarrier(1, &barrier);
            g_commandList->OMSetRenderTargets(1, &fc.RenderTargetDescriptor, FALSE, nullptr);
            g_commandList->SetDescriptorHeaps(1, &g_srvHeap);

            ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), g_commandList);

            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
            barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_PRESENT;
            g_commandList->ResourceBarrier(1, &barrier);
            g_commandList->Close();

            ID3D12CommandList* lists[] = { g_commandList };
            g_commandQueue->ExecuteCommandLists(1, lists);
            SignalFrame(fc);
        }
        catch (const std::exception& e)
        {
            static bool logged = false;
            if (!logged)
            {
                LOG("hkPresent: std::exception ignored: %s", e.what());
                logged = true;
            }
        }
        catch (...)
        {
            static bool logged = false;
            if (!logged)
            {
                LOG("hkPresent: exception ignored.");
                logged = true;
            }
        }

        return oPresent(sc, sync, flags);
    }

    // ---- hooked ResizeBuffers ---------------------------------------------
    HRESULT WINAPI hkResizeBuffers(IDXGISwapChain3* sc, UINT bufferCount, UINT w, UINT h, DXGI_FORMAT fmt, UINT flags)
    {
        try
        {
            if (g_init)
            {
                WaitForAllFrames();
                CleanupRenderTargets();
            }
        }
        catch (...) { LOG("hkResizeBuffers: pre-resize cleanup exception ignored."); }

        HRESULT hr = oResizeBuffers ? oResizeBuffers(sc, bufferCount, w, h, fmt, flags) : DXGI_ERROR_INVALID_CALL;

        try
        {
            if (g_init && SUCCEEDED(hr))
            {
                DXGI_SWAP_CHAIN_DESC desc{};
                if (SUCCEEDED(sc->GetDesc(&desc)) && desc.BufferCount != g_buffersCount)
                {
                    LOG("hkResizeBuffers: buffer count changed %u -> %u; rebuilding overlay state.", g_buffersCount, desc.BufferCount);
                    ReleaseOverlayResources();
                }
                else if (!CreateRenderTargets(sc))
                {
                    LOG("hkResizeBuffers: failed to recreate render targets.");
                }
            }
        }
        catch (...) { LOG("hkResizeBuffers: post-resize exception ignored."); }
        return hr;
    }

    // ---- hooked ExecuteCommandLists (capture the command queue) ------------
    void WINAPI hkExecuteCommandLists(ID3D12CommandQueue* queue, UINT num, ID3D12CommandList* const* lists)
    {
        try
        {
            if (!g_commandQueue && queue && queue->GetDesc().Type == D3D12_COMMAND_LIST_TYPE_DIRECT)
            {
                g_commandQueue = queue;
                g_commandQueue->AddRef();
                LOG("Captured command queue %p", queue);
            }
        }
        catch (...) { LOG("hkExecuteCommandLists: queue capture exception ignored."); }

        if (oExecuteCommandLists)
            oExecuteCommandLists(queue, num, lists);
    }

    // ---- grab vtable method pointers via a throwaway device/swapchain -------
    bool GetVTables(void**& swapVT, void**& queueVT)
    {
        swapVT = nullptr;
        queueVT = nullptr;

        ID3D12Device* device = nullptr;
        ID3D12CommandQueue* queue = nullptr;
        IDXGIFactory4* factory = nullptr;
        IDXGISwapChain1* swap1 = nullptr;
        HWND hwnd = nullptr;
        bool registered = false;

        auto cleanup = [&]()
        {
            SafeRelease(swap1);
            SafeRelease(factory);
            SafeRelease(queue);
            SafeRelease(device);
            if (hwnd) { DestroyWindow(hwnd); hwnd = nullptr; }
        };

        char className[64]{};
        sprintf_s(className, "AHM_Dummy_%lu", GetCurrentProcessId());

        WNDCLASSEXA wc{ sizeof(wc) };
        wc.lpfnWndProc = DefWindowProcA;
        wc.hInstance = GetModuleHandle(nullptr);
        wc.lpszClassName = className;
        registered = RegisterClassExA(&wc) != 0;
        if (!registered)
        {
            DWORD err = GetLastError();
            LOG("dummy RegisterClassEx failed err=%lu", err);
            return false;
        }

        hwnd = CreateWindowA(wc.lpszClassName, "AHM", WS_OVERLAPPEDWINDOW, 0, 0, 100, 100, nullptr, nullptr, wc.hInstance, nullptr);
        if (!hwnd)
        {
            LOG("dummy CreateWindow failed err=%lu", GetLastError());
            UnregisterClassA(wc.lpszClassName, wc.hInstance);
            return false;
        }

        HRESULT hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
        if (FAILED(hr) || !device)
        {
            LOG("dummy CreateDevice failed hr=0x%lX", hr);
            cleanup();
            UnregisterClassA(wc.lpszClassName, wc.hInstance);
            return false;
        }

        D3D12_COMMAND_QUEUE_DESC qd{};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        hr = device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue));
        if (FAILED(hr) || !queue)
        {
            LOG("dummy CreateCommandQueue failed hr=0x%lX", hr);
            cleanup();
            UnregisterClassA(wc.lpszClassName, wc.hInstance);
            return false;
        }

        hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
        if (FAILED(hr) || !factory)
        {
            LOG("dummy CreateDXGIFactory1 failed hr=0x%lX", hr);
            cleanup();
            UnregisterClassA(wc.lpszClassName, wc.hInstance);
            return false;
        }

        DXGI_SWAP_CHAIN_DESC1 scd{};
        scd.BufferCount = 2;
        scd.Width = 100; scd.Height = 100;
        scd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        scd.SampleDesc.Count = 1;

        hr = factory->CreateSwapChainForHwnd(queue, hwnd, &scd, nullptr, nullptr, &swap1);
        if (FAILED(hr) || !swap1)
        {
            LOG("dummy swapchain failed hr=0x%lX", hr);
            cleanup();
            UnregisterClassA(wc.lpszClassName, wc.hInstance);
            return false;
        }

        swapVT  = *reinterpret_cast<void***>(swap1);
        queueVT = *reinterpret_cast<void***>(queue);

        // tear the dummies down; vtable addresses stay valid (they live in dxgi/d3d12)
        cleanup();
        UnregisterClassA(wc.lpszClassName, wc.hInstance);
        return swapVT && queueVT;
    }
}

bool DX12Hook::Install()
{
    try
    {
        ULONGLONG startMs = GetTickCount64();
        void** swapVT = nullptr; void** queueVT = nullptr;
        if (!GetVTables(swapVT, queueVT)) return false;
        ULONGLONG vtablesMs = GetTickCount64();

        MH_STATUS init = MH_Initialize();
        if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED)
        {
            LOG("MH_Initialize failed status=%d", init);
            return false;
        }

        auto createHook = [](void* target, void* detour, void** original, const char* name) -> bool
        {
            MH_STATUS st = MH_CreateHook(target, detour, original);
            if (st == MH_OK || st == MH_ERROR_ALREADY_CREATED) return true;
            LOG("MH_CreateHook(%s) failed status=%d target=%p", name, st, target);
            return false;
        };

        // IDXGISwapChain vtable: Present=8, ResizeBuffers=13
        // ID3D12CommandQueue vtable: ExecuteCommandLists=10
        g_presentTarget = swapVT[8];
        g_resizeTarget = swapVT[13];
        g_executeTarget = queueVT[10];
        if (!createHook(swapVT[8],   &hkPresent,             reinterpret_cast<void**>(&oPresent), "Present") ||
            !createHook(swapVT[13],  &hkResizeBuffers,       reinterpret_cast<void**>(&oResizeBuffers), "ResizeBuffers") ||
            !createHook(queueVT[10], &hkExecuteCommandLists, reinterpret_cast<void**>(&oExecuteCommandLists), "ExecuteCommandLists"))
        {
            RemoveCreatedHooks();
            MH_Uninitialize();
            return false;
        }
        ULONGLONG createMs = GetTickCount64();

        MH_STATUS qPresent = MH_QueueEnableHook(g_presentTarget);
        MH_STATUS qResize = MH_QueueEnableHook(g_resizeTarget);
        MH_STATUS qExec = MH_QueueEnableHook(g_executeTarget);
        if (!HookStatusOk(qPresent) || !HookStatusOk(qResize) || !HookStatusOk(qExec))
        {
            LOG("MH_QueueEnableHook failed statuses Present=%d Resize=%d Exec=%d", qPresent, qResize, qExec);
            RemoveCreatedHooks();
            MH_Uninitialize();
            return false;
        }

        MH_STATUS enable = MH_ApplyQueued();
        if (enable != MH_OK)
        {
            LOG("MH_ApplyQueued enable failed status=%d", enable);
            RemoveCreatedHooks();
            MH_Uninitialize();
            return false;
        }
        ULONGLONG enableMs = GetTickCount64();

        LOG("DX12 hooks installed (Present=%p ResizeBuffers=%p Exec=%p; timing vtables=%llums create=%llums enable=%llums total=%llums)",
            swapVT[8],
            swapVT[13],
            queueVT[10],
            vtablesMs - startMs,
            createMs - vtablesMs,
            enableMs - createMs,
            enableMs - startMs);
        return true;
    }
    catch (...)
    {
        LOG("DX12Hook::Install: exception.");
        return false;
    }
}

void DX12Hook::Remove()
{
    if (g_presentTarget)
        MH_QueueDisableHook(g_presentTarget);
    if (g_resizeTarget)
        MH_QueueDisableHook(g_resizeTarget);
    if (g_executeTarget)
        MH_QueueDisableHook(g_executeTarget);
    MH_ApplyQueued();
    RemoveCreatedHooks();
    MH_Uninitialize();

    ReleaseOverlayResources();
    SafeRelease(g_commandQueue);

    g_init = false;
    LOG("DX12 hooks removed");
}
