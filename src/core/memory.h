#pragma once
#include <cstddef>
#include <cstdint>

// Pointer-safety helpers. The whole point: with wrong offsets we must DEGRADE
// (show "SDK not resolved") instead of dereferencing garbage and crashing the
// game. Every read of game memory should be gated on these.
namespace Mem
{
    // True if [p, p+size) is committed and readable in this process.
    bool IsReadable(const void* p, size_t size = sizeof(void*));

    // Heuristic: looks like a usable heap/image pointer (aligned, readable,
    // not in the null page or obviously bogus high range).
    bool LooksLikePtr(const void* p);
}
