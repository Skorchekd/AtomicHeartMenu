#pragma once
#include <cstdint>

// Signature/AOB scanning over the main module. Patterns use IDA-style strings,
// e.g. "48 8B 05 ?? ?? ?? ?? 48 85 C0" where ?? is a wildcard byte.
namespace Scanner
{
    // Find the first match of `pattern` in the main module. Returns nullptr if
    // not found.
    uint8_t* Find(const char* pattern);

    // Find a match, then resolve a 32-bit RIP-relative reference located at
    // (match + offset). `instructionLength` is the full length of the
    // instruction that contains the rel32 (so we can compute the next-IP base).
    // Typical UE4 usage for `mov rax, [rip+disp]` (48 8B 05 xx xx xx xx) is
    // ResolveRipRel(match, 3, 7).
    uint8_t* ResolveRipRel(uint8_t* match, int offsetToRel32, int instructionLength);

    // Convenience: find pattern then resolve the rel32 in one call.
    uint8_t* FindRipRel(const char* pattern, int offsetToRel32, int instructionLength);
}
