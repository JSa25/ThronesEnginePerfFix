// From the Attila mod (heapfix/loopfetch.c): its three render-pass sites whose functions are identical in ToB (but
// for call targets and relocated addresses). Attila's pass-loop sites (FUN_117b0400 and siblings) are compiled
// differently in ToB (other registers; CA also fixed the batching test there, so batchfix.c is not needed).
// Prefetch ahead in the battle render pass loops that walk an array of item pointers and stall on the first read
// of a field of each item (AGENT_LOG 47-49). The array is known in advance, so while handling item i the stub asks
// the CPU to start loading the field of item i+4 that the loop is about to read. It only prefetches through item
// pointers, never loads: a loop can stop early, so a slot it never reaches may hold a stale pointer. The array itself
// is only read inside the loop's range (i+4 < end). The loop's own instructions then run unchanged.
// Each site is a loop top that is the target of exactly one jb back-edge, with no jump into the patched bytes.
#include <windows.h>
#include <string.h>

volatile unsigned char g_loopfetch_on = 1;  // an A/B could flip it; the original instructions run either way

static void put32(unsigned char *p, DWORD v) { memcpy(p, &v, 4); }

struct site {
    DWORD rva;
    unsigned char len;
    unsigned char orig[9];
    unsigned char fetch[40];  // complete, including its own pushes and pops
    unsigned char fetch_len;
    unsigned char on;         // installed (off: measured no gain or worse, kept for the test)
    const char *name;
};

// Layout "frame in ebp ([ebp+0Ch] list, [ebp+18h] end), index in ecx" (the three functions below):
// eax is overwritten by the original next, so it holds the array here.
#define FETCH_EBP_FRAME_ECX_I_HEAD                                                                         \
    0x52,                          /* push edx */                                                        \
    0x8B, 0x45, 0x0C,              /* mov eax,[ebp+0Ch]      list */                                     \
    0x8B, 0x40, 0x08,              /* mov eax,[eax+8]        array */                                    \
    0x8D, 0x51, 0x04,              /* lea edx,[ecx+4] */                                                 \
    0x3B, 0x55, 0x18               /* cmp edx,[ebp+18h]      end */

static const struct site SITES[] = {
    {0x1823BB5, 6, {0x8B, 0x45, 0x0C, 0x8B, 0x40, 0x08},
     {FETCH_EBP_FRAME_ECX_I_HEAD, 0x73, 0x07, 0x8B, 0x14, 0x90, 0x0F, 0x18, 0x4A, 0x3C, 0x5A},
     23, 1, "render pass +0x1823AE0 (Attila FUN_117aff90)"},
    {0x1823832, 6, {0x8B, 0x45, 0x0C, 0x8B, 0x40, 0x08},
     {FETCH_EBP_FRAME_ECX_I_HEAD, 0x73, 0x07, 0x8B, 0x14, 0x90, 0x0F, 0x18, 0x4A, 0x40, 0x5A},
     23, 1, "render pass +0x1823760 (Attila FUN_117afc10)"},
    {0x1823345, 6, {0x8B, 0x45, 0x0C, 0x8B, 0x40, 0x08},
     {FETCH_EBP_FRAME_ECX_I_HEAD, 0x73, 0x0E, 0x8B, 0x14, 0x90, 0x0F, 0x18, 0x4A, 0x3C,
      0x0F, 0x18, 0x8A, 0xC0, 0x00, 0x00, 0x00, 0x5A},  // also prefetcht0 [edx+0C0h], read later in the loop
     30, 1, "render pass +0x1823270 (Attila FUN_117af720)"},
};
#define NSITES (sizeof SITES / sizeof *SITES)

// [cmp byte [flag],0; je orig] <fetch>; orig: <original bytes>; jmp site+len
static unsigned build(unsigned char *s, const struct site *t, unsigned char *site) {
    unsigned n = 0;
    s[n++] = 0x80; s[n++] = 0x3D;
    put32(s + n, (DWORD)(UINT_PTR)&g_loopfetch_on); n += 4;
    s[n++] = 0x00;
    s[n++] = 0x74; s[n++] = t->fetch_len;
    memcpy(s + n, t->fetch, t->fetch_len); n += t->fetch_len;
    memcpy(s + n, t->orig, t->len); n += t->len;
    s[n++] = 0xE9; put32(s + n, (DWORD)((site + t->len) - (s + n + 4))); n += 4;
    return n;
}

static int patch(unsigned char *site, unsigned len, unsigned char *to) {
    DWORD old;
    if (!VirtualProtect(site, len, PAGE_EXECUTE_READWRITE, &old)) return 0;
    site[0] = 0xE9;
    put32(site + 1, (DWORD)(to - (site + 5)));
    for (unsigned i = 5; i < len; i++) site[i] = 0x90;
    VirtualProtect(site, len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, len);
    return 1;
}

int install_loopfetch(HMODULE m, void (*log)(const char *fmt, ...)) {
    unsigned char *code = VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!code) return 0;
    int n = 0, want = 0;
    for (unsigned i = 0; i < NSITES; i++) {
        const struct site *t = &SITES[i];
        if (!t->on) continue;
        want++;
        unsigned char *site = (unsigned char *)m + t->rva, *s = code + 128 * i;
        if (memcmp(site, t->orig, t->len) != 0) {
            log("loop prefetch: %s differs from this game version - skipped", t->name);
            continue;
        }
        build(s, t, site);
        FlushInstructionCache(GetCurrentProcess(), s, 128);
        n += patch(site, t->len, s);
    }
    log("loop prefetch: %d of %d render loops prefetch ahead", n, want);
    return n;
}
