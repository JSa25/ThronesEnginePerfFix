// Game builds the fixes know. A fix with addresses per build keeps them in the same order as here and uses the entry
// whose bytes match, so an unknown build is still refused by the byte checks.
//   0: Steam buildid 11631387 (empire.retail.dll 64A47C87)
#pragma once
#include <windows.h>

#define NBUILDS 1

// for the log: the game version in plain words, by empire.retail.dll's TimeDateStamp (NULL = unknown build)
static inline const char *build_name(DWORD stamp) {
    return stamp == 0x64A47C87u ? "Steam version (build 11631387)" : NULL;
}

// rva..rva+n lies inside the mapped image (a table entry for another build may point past a smaller one)
static inline int in_image(const void *m, DWORD rva, DWORD n) {
    const IMAGE_NT_HEADERS *nt = (const IMAGE_NT_HEADERS *)((const char *)m + ((const IMAGE_DOS_HEADER *)m)->e_lfanew);
    return rva < nt->OptionalHeader.SizeOfImage && n <= nt->OptionalHeader.SizeOfImage - rva;
}
