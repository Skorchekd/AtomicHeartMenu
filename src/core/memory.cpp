#include "memory.h"
#include <Windows.h>

bool Mem::IsReadable(const void* p, size_t size)
{
    if (!p || size == 0) return false;

    auto addr = reinterpret_cast<uintptr_t>(p);
    // reject the null page and non-canonical / kernel-space addresses
    if (addr < 0x10000 || addr > 0x7FFFFFFFFFFFull) return false;

    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(p, &mbi, sizeof(mbi)) == 0) return false;
    if (mbi.State != MEM_COMMIT) return false;

    constexpr DWORD readable =
        PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
        PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    if (!(mbi.Protect & readable)) return false;
    if (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) return false;

    // make sure the whole requested range sits inside this committed region
    auto regionStart = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
    auto regionEnd   = regionStart + mbi.RegionSize;
    return (addr + size) <= regionEnd;
}

bool Mem::LooksLikePtr(const void* p)
{
    auto addr = reinterpret_cast<uintptr_t>(p);
    if (addr & 0x7) return false;            // UObjects are 8/16-byte aligned
    return Mem::IsReadable(p, sizeof(void*));
}
