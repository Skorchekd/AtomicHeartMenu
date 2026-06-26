# AI null-target crash -- verification, root cause, and the native-hook fix

This documents (1) the verification of the prior agent's crash analysis against Ghidra
and the captured minidump, (2) the *actionable* root cause it stopped short of, (3) the
new signatures, and (4) the native-hook layer (`src/hooks/native_hooks.*`) + **Hook
Testing** tab that fixes it and lets you pretest AI stability before porting.

Crash artifact: `crash_artifacts/UE4CC-Windows-…_0000` -- `AtomicHeart-Win64-Shipping`,
UE 4.27.2, module base `0x140000000`,
`EXCEPTION_ACCESS_VIOLATION reading address 0x0000000000000738` on the **GameThread**.

---

## 1. Verification of the prior claims -- ALL CONFIRMED

Every byte/address the previous agent gave checks out against both the live Ghidra
program and the minidump's `PCallStack`.

| Claim | Verified? | Evidence |
|---|---|---|
| Module base `0x140000000` | ✅ | `CrashContext` `0x0000000140000000` |
| Faulting helper starts at `0x141BC8FF0` | ✅ | function prologue `48 83 EC 28` at that addr |
| Crash instr `0x141BC8FF4: MOV R9,[RCX+0x738]` | ✅ | bytes `4C 8B 89 38 07 00 00` decode exactly to that |
| Crash because RCX==0, read `0x738` | ✅ | `ErrorMessage … reading address 0x…738`; helper has **no null-check** on RCX |
| Caller around `0x141B988E0` | ✅ | `FUN_141b988e0` contains the hot block |
| Hot callsite block `0x141B9908A` | ✅ | bytes match the agent's quote **exactly** |
| Bad call at `0x141B990A5`, return `0x141B990AA` | ✅ | `E8 46 FF 02 00` → target `0x141B990AA + 0x2FF46 = 0x141BC8FF0`; return addr = frame 1 in `PCallStack` |
| Raw bytes at hot callsite | ✅ | byte-for-byte identical |
| Masked signature | ✅ | matches (and is unique -- see §3) |

`PCallStack` frame 0 = `base+0x1bc8ff4`, frame 1 = `base+0x1b990aa` -- i.e. the crash
instruction and the return address, exactly as claimed.

The one claim I could **not** independently confirm is the *name* "+0x738 =
`AAHBaseCharacter::AbilitySystemComp`" -- that came from the agent's private offset set.
The offset and the null-`this` mechanism are confirmed; the field label is plausible
(the helper reads `[this+0x738]`, treats it as a `UObject*`, then validates it through
`GObjects` via the chunked `FUObjectArray` index decode -- i.e. it fetches a component and
checks it's still a live object), but treat the exact field name as unverified.

---

## 2. The actionable root cause (the part that was missing)

The prior analysis stopped at the *mechanical* cause ("RCX is null"). The full chain,
traced through the decompiler, is:

`FUN_141b988e0` (the AI **context-target-ally tick**) does, at the hot block:

```c
ally = GetContextTargetAlly(controller);           // FUN_141cf8720: reads controller+0x14a0 weak ptr
if (ally == 0 || !IsA_AHAICharacter(ally))         // FUN_141bcbc70: engine IsA vs AHAICharacter::StaticClass
    ally = 0;                                       //   -> store NULL
weakptr_assign(this+0x800, ally);                   // FUN_14277c040
resolved = weakptr_get(this+0x800);                 // FUN_14277c190 -> NULL when ally was NULL
helper(resolved);                                   // FUN_141bc8ff0(NULL) -> [NULL+0x738] -> CRASH
```

There is **no `IsValid` check before the helper call** (unlike other callsites), so a null
ally flows straight into the un-null-checked helper.

**What makes the ally null in our case** -- found in our own code at
`src/features/features.cpp` (the bodyguard guard-loop):

```cpp
SetAiTargetAlly(guard, player);                     // ally = the PLAYER
if (UObject* ctrl = GetAiController(guard))
    SetControllerTargetAlly(ctrl, player);          // controller context-ally = the PLAYER
```

The **player pawn is an `AHBaseCharacter`, not an `AHAICharacter`**. So the engine's
`IsA_AHAICharacter(player)` returns false → it stores null → resolves null → crashes. The
crash is therefore *reliably triggered by the bodyguard feature assigning the player as an
AI's target-ally*, surfacing a latent engine bug (missing null-check in `FUN_141bc8ff0`).
Assigning the player there never even "worked" -- the engine zeroes it every tick -- it only
ever cost us the crash.

---

## 3. New signatures (all build `4.27-CL-18319896`)

Resolved unique in the live module. The runtime scanner **requires a unique match** and
refuses to hook an absent or ambiguous (≥2-hit) signature -- fail-safe by construction.

| Symbol (Ghidra label) | Addr | RVA | Signature |
|---|---|---|---|
| `AHM_CrashHelper_ContextAllyComp_NoNullCheck` | `141BC8FF0` | `1BC8FF0` | `48 83 EC 28 4C 8B 89 38 07 00 00 4C 8B D1 4D 85 C9 0F 84` |
| `AHM_CastHelper_AHBaseChar_NoNullCheck` | `141D097B0` | `1D097B0` | `48 83 EC 28 4C 8B 89 18 03 00 00 4C 8B D1 4D 85 C9 0F 84` |
| `AHM_IsA_AHAICharacter` | `141BCBC70` | `1BCBC70` | `40 53 48 83 EC 20 48 8B D9 E8 ?? ?? ?? ?? 48 8B 53 10 4C 8D 40 38 48 63` |
| `AHM_GetContextTargetAlly_AsBaseChar` | `141CF8720` | `1CF8720` | `48 83 EC 28 4C 8D 89 A0 14 00 00 49 8B C9 E8 ?? ?? ?? ?? 84 C0 74 14` |
| `AHM_AIContextAllyTick_crashcaller` (hot callsite) | callsite `141B9908A` | -- | `48 8D 8E 00 08 00 00 E8 ?? ?? ?? ?? 48 8D 8E 00 08 00 00 E8 ?? ?? ?? ?? 48 8B C8 E8 ?? ?? ?? ?? 4C 8B C0 48 85 C0 74 55` |

Notes:
- The two crash helpers share a prologue template; the **displacement** (`38 07 00 00`
  = +0x738 vs `18 03 00 00` = +0x318) is what makes each sig unique -- do **not** wildcard it.
- The two `StaticClass` thunks (`AHAICharacter` `141B863C0`, `AHBaseCharacter` `141B85FE0`)
  are byte-identical except their RIP-relative displacement, so they are *not* sig-safe to
  wildcard; we resolve `IsA_AHAICharacter` directly instead (it internally fetches the class).
- The callsite sig resolves mid-function; the menu cross-checks that its 3rd `CALL`
  (at +27) targets the primary helper -- a free integrity check logged on the `[HOOK]` channel.

These addresses are also labelled/commented in the Ghidra database (`AHM_*`).

---

## 4. The fix -- `src/hooks/native_hooks.*` + the **Hook Testing** tab

### Native-hook layer (separate from the SDK)
A new module resolves the signatures above over the live module and installs MinHook
detours, **fully independent of the GObjects/SDK layer**. Logging is split by channel:
`[SDK]` for reflection (GObjects/GNames/GWorld, FindObject, UFunctions) and `[HOOK]` for
native code-byte scanning + detours.

**The crash guard (centerpiece):** a detour on `FUN_141bc8ff0` (and, defence-in-depth, the
`+0x318` sibling `FUN_141d097b0`):

```cpp
void* hkPrimary(void* self) {
    if (enabled && (uintptr_t)self < 0x10000) { ++guarded; return nullptr; }
    return original(self);
}
```

Returning `0` is exactly the helper's own "no result" path (every caller already branches
`if (result != 0)`), so it is **behaviour-preserving for every valid object** and merely
refuses to fault on a null one. One tiny compare neutralises the bug for **all** call paths
(direct + vtable) at once. It is wired in `dllmain` right after the DX12 hook -- live before
the SDK even resolves -- and removed on eject.

**Fail-safe guarantees**
- Missing sig → entry shows `missing`, no detour, SDK path unaffected.
- Ambiguous sig (≥2 hits) → `ambiguous`, **refused** (never patch a non-unique site).
- `MH_CreateHook`/`EnableHook` failure → `install-fail`, no detour.
- Validation helpers (`IsAHAICharacter`, `ContextTargetAlly`) guard every input with
  `Mem::IsReadable` and run under the `/EHa` `try/catch` firewall → return `false`/`null`,
  never crash.

### Hook Testing tab
A new menu tab (`Menu` → **Hook Testing**) to pretest before porting:
- **Crash-guard headline**: ACTIVE/INACTIVE + running count of null-target derefs caught.
- **Signature table**: per-hook state (`detour`/`resolved`/`missing`/`ambiguous`/`install-fail`),
  RVA, call hits, guarded count, and a per-detour **Guard** toggle.
- **Rescan signatures** button (re-resolves + installs anything not yet live).
- **Live validation**: shows `Player IsA AHAICharacter` (→ `NO`, demonstrating the poison).
- **Stress test**: buttons that drive the real bodyguard/follow/attack features so you can
  hammer the exact crash paths. With the guard **ON** the *Guarded* counter climbs instead
  of the game dying; toggle it **OFF** to confirm the sig truly catches the fault (it will
  likely crash -- which proves it).

---

## 5. Porting to the main features (next step)

The crash guard is global the moment it installs, so the existing bodyguard/follow features
are already protected without touching `features.cpp`. When promoting further:

1. Before assigning any target-ally, gate on `NativeHooks::IsAHAICharacter(ally)` -- this is
   the *exact* engine check, so a `false` means "do not assign; it will be zeroed anyway".
   In practice this means: stop assigning the **player** as an AI target-ally entirely
   (use the follow/blackboard-location path for protect-the-player behaviour instead).
2. Keep the detour as the backstop even after (1) -- it costs one compare and covers every
   other null-ally source (dead/despawned allies whose weak ptr resolves null).
3. Re-verify the sigs after any game patch via the tab's **Rescan** (a moved/ambiguous sig
   fails safe to the SDK path and is logged).

---

## 6. AI ownership lock -- making "ours" permanent (native, ProcessEvent-level)

**Goal:** a squad unit must stay friendly/subservient forever -- never attack the player,
fight real enemies, follow -- without the game flipping it back. The previous SDK approach
*set* the team and re-asserted it; the game's own AI re-evaluates attitude and flips it
back, which is the "fragile" behaviour.

**The mechanism that's actually robust:** stop the game from changing it, rather than
fighting it. The menu already runs a real MinHook detour on `UObject::ProcessEvent`, and
the engine performs the un-allying through ordinary UFunctions dispatched *through that
detour*. So we **swallow** exactly those calls for our own units -- the identical, proven
pattern the Horde no-save guard already uses. Implemented in `features.cpp`
(`hkProcessEvent` + `OwnershipShouldSwallow`), armed by **Hook Testing -> Ownership lock**:

A dispatch is swallowed only when **all** hold (otherwise it passes through untouched):
1. `fn` is one of three cached un-ally UFunctions:
   `AHAICharacter.SwitchTeamToMatchCharacterAttitude`, `AHAICharacter.SetTargetEnemy`,
   `AHAIController.SetBlackboardTargetEnemy`;
2. `obj` is a squad member (or a squad member's controller, via `Pawn` @0x270);
3. the direction is *against us*:
   - `SwitchTeamToMatchCharacterAttitude(target, attitude)` where `target` is the player /
     a squad member and `attitude != Friendly(0)`  (our own `(player, Friendly)` call passes);
   - `SetTargetEnemy` / `SetBlackboardTargetEnemy` whose target is the player / a squad
     member  (our "attack that real enemy" commands target non-owned actors, so they pass).

This is **class-agnostic** -- it filters UFunctions, not character types -- so it covers
the Mercuna-navigated **Twins** exactly like any robot. It is OFF by default, logs every
block (throttled), counts them in the tab, and is wrapped so a bad param read degrades to
"don't swallow" rather than ever crashing. Recruit units first (that makes them friendly via
the existing path), then arm the lock so they **stay** ours.

**Workflow in the tab:** Recruit nearby -> squad  ->  enable *Ownership lock*  ->  the
"Un-ally calls blocked" counter climbs whenever the game would have turned a unit against
you. Watch behaviour: units should now never retaliate against you and never drop to the
enemy faction.

### Why this and not a `GetTeamAttitudeTowards` C++ detour (the honest limitation)
The "purest" hook is a detour on the engine's C++ `IGenericTeamAgentInterface::GetTeamAttitudeTowards`
virtual (force Friendly for player/owned pairs). Locating it **verifiably** needs the
attitude function's address. On this 436 MB shipping binary the static-analysis tooling for
that (full string/symbol search, and large-function decompilation) was unreliable
(repeated timeouts), and shipping an *unverified* deep AI detour would violate the "no
risk, game-verified" bar. What was pinned: the AHAICharacter primary vtable
(`0x145cdcfe8`, `FUN_141b988e0` at slot 58); the `IGenericTeamAgentInterface` slice lives
in a secondary vtable that the stripped binary doesn't cleanly cross-reference. The
ProcessEvent-swallow above achieves the same *user-visible* guarantee (the game can never
turn our unit against us) through a fully-verified, already-proven mechanism, so it is the
shipped default. To complete the C++-virtual detour later, capture the exact address from
the in-game **Debug -> Dump targeted actor (full reflection)** output (it prints every
component vtable + module/RVA) and feed it to a new `NativeHooks` detour entry.
