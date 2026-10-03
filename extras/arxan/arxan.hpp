// arxan.hpp: neutralise the Arxan code-integrity checks of the retail Black Ops III exe, so that code patches survive.
//
// WHAT IT IS FOR
//   The retail BlackOps3.exe (Steam CL 13892626, PE SizeOfImage 0x1D74B000, TimeDateStamp 0x693D731E) is protected by Arxan.
//   Its .text is encrypted on disk and decrypted about 4 seconds after launch; from then on 1,069 integrity checks run on
//   other threads all the time. Each one checksums a range of code and compares it with the stored original. If you patch
//   any code those checks cover (a hook, a byte patch), the game jumps into unmapped memory about 20 s later and dies.
//   The BO3 Enhanced exe (CL 20659811) has no such checks, so there this does nothing and is not needed.
//
// WHAT IT DOES
//   Every check ends by storing the checksum it computed and counting down a loop counter. This code finds all those
//   store sites in the executable sections of the exe and redirects each to a small stub. The stub finds the check's
//   {computed, original} checksum pair in the caller's stack frame and stores the ORIGINAL checksum in place of the
//   computed one, then runs the displaced instructions, so every check passes no matter what was patched. Sites:
//     * 1,000 "intact" checks:  89 04 8A (mov [rdx+rcx*4], eax) directly followed by 83 45 xx FF (add dword [rbp+xx], -1).
//       Those 7 bytes become `push xx ; call stub`.
//     * 69 "obfuscated-store" checks: the same store, but followed by an obfuscated jump chain instead of the add.
//       Pattern 8B 04 82 | 48 8D 15 rel32 | 89 04 8A. The `lea` + store (10 bytes) become `jmp <own stub> ; nop x5`. A stub
//       per site keeps 128 bytes below rsp untouched (the chain writes there), preserves flags and xmm0-5, asks for the
//       value to store, redoes the lea and the store with it, and jumps back into the chain. Skipping these is NOT safe:
//       left armed they fail ~20 s after the intact ones are patched.
//     Total 1,000 + 69 = 1,069 on that build. (A third "split" form, 89 04 8A E9, is only counted: the retail build has
//     none.) The count is not hard-coded: Neutralise() patches whatever sites it finds and returns how many.
//   The checks run all the time, so the patches are applied with every other thread of the process suspended, and only when
//   none of them is part-way through a site (it retries up to 50 times). The intact and obfuscated patches go in one batch,
//   so a check of one kind never runs between the two.
//
// THE CONTRACT
//   * Call arxan::Neutralise() once the game has decrypted its code (any time after ~4 s: from the game's Lua / UI thread is
//     fine) and BEFORE you patch any code in the exe. A call while .text is still encrypted finds no sites and returns 0.
//   * Returns the number of checks neutralised, or 0 if it did nothing (no sites: not the retail exe or still encrypted;
//     no executable memory within rel32 reach of the exe; the threads could not be frozen safely). On 0, treat code patching
//     as unsafe on retail.
//   * Once per process. Several modules may carry this code (weapon_tech.dll, stub_boot.dll, yours): a named mutex
//     "Local\bo3_arxan_mutex_<pid>" serialises them and a named shared block "Local\bo3_arxan_record_<pid>" records the
//     first module's result, which every later caller returns. The names and the record layout are the same as
//     weapon_tech's, so this interoperates with it. An older neutraliser that left no record is detected by counting
//     sites already turned into `push x ; call <stub outside the exe>` (100 or more calls to one stub = done), and the
//     obfuscated-store sites that tool missed are still patched.
//   * 64-bit only (x64 machine code is emitted). Not thread-safe against itself except through the mutex above.
//   * Leaves the exe's .text modified in memory only; nothing is written to disk.
//   * The stubs live in executable memory allocated within +-2 GB of the exe image (rel32 reach) and are never freed.
//
// LIMITS
//   Tested on the one retail build above only; any other exe that happens to contain the same instruction patterns would
//   be patched too, so only call this for the retail BO3 exe (check SizeOfImage / TimeDateStamp first). It defeats the
//   integrity checks only, not Arxan's other protections.
//
// USAGE
//   #include "arxan.hpp"
//   arxan::SetLog([](const char *line) { /* write line somewhere */ });   // optional; default is OutputDebugString
//   if (arxan::Neutralise() == 0) { /* do not patch code on retail */ }
//
// BUILD
//   cl /nologo /O2 /MT /EHsc /std:c++17 /c arxan.cpp          (or add arxan.cpp to any Windows x64 C++17 project)
#pragma once
#include <cstddef>

namespace arxan
{
using LogFn = void (*)(const char *line);

// Where progress lines go (one call per line, no trailing newline). Default: OutputDebugStringA.
void SetLog(LogFn fn);

// Neutralise the checks, once per process. Returns how many were neutralised (0 = none, patching code is unsafe).
size_t Neutralise();
}  // namespace arxan
