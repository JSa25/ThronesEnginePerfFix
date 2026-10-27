// Frame hook: a jump on the entry of the tile-feedback scan (+0x194D080, Attila's FUN_118d7970, Rome 2's tile scan), which
// the game runs once per drawn frame in campaign and battle. It only calls the per-frame safety checks (x87fix.c) and then
// runs the original function through a trampoline. Attila's own frame function (+0xC1950 there) has no byte-identical
// copy in ToB.
#include <windows.h>
#include <string.h>

#define RVA_FRAME 0x194D080
static const unsigned char FRAME_ENTRY[8] = {0x83, 0xEC, 0x14, 0x53, 0x8B, 0x5C, 0x24, 0x20};  // sub esp,14h; push ebx; mov ebx,[esp+20h]

static unsigned char g_tramp[16];
void *g_frame_tramp_ptr = g_tramp;
void (*g_frame_check)(void);  // set by proxy.c

void frame_call(void) {
    if (g_frame_check) g_frame_check();
}

// Saves every register and flag, runs the checks, then the original function via the trampoline.
__attribute__((naked)) static void frame_stub(void) {
    __asm__ volatile(
        "pushal\n\t"
        "pushfl\n\t"
        "call _frame_call\n\t"
        "popfl\n\t"
        "popal\n\t"
        "jmp *_g_frame_tramp_ptr\n\t");
}

// Returns 1 if the frame hook was installed.
int install_frame(HMODULE m, void (*log)(const char *fmt, ...)) {
    unsigned char *site = (unsigned char *)m + RVA_FRAME;
    if (memcmp(site, FRAME_ENTRY, sizeof FRAME_ENTRY) != 0) {
        log("frame hook: +0x%X does not match this game version - not installed", RVA_FRAME);
        return 0;
    }
    DWORD old;
    if (!VirtualProtect(g_tramp, sizeof g_tramp, PAGE_EXECUTE_READWRITE, &old)) return 0;
    memcpy(g_tramp, site, sizeof FRAME_ENTRY);
    g_tramp[8] = 0xE9;
    DWORD rel = (DWORD)((site + 8) - (g_tramp + 13));
    memcpy(g_tramp + 9, &rel, 4);
    FlushInstructionCache(GetCurrentProcess(), g_tramp, sizeof g_tramp);
    if (!VirtualProtect(site, 8, PAGE_EXECUTE_READWRITE, &old)) return 0;
    site[0] = 0xE9;
    rel = (DWORD)((unsigned char *)frame_stub - (site + 5));
    memcpy(site + 1, &rel, 4);
    memset(site + 5, 0x90, 3);  // never executed (the jmp leaves first)
    VirtualProtect(site, 8, old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, 8);
    log("frame hook ready (per-frame safety checks)");
    return 1;
}
