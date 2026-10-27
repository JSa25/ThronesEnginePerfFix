// Small-copy fast path for the game's memcpy/memmove (from the Attila mod's memfast.c). ToB has two identical copies of
// the MSVC CRT routine (cdecl (dst, src, n), returns dst): +0x1B2ACE0 and +0x1B2BBA0, both = Attila's FUN_11a27ca0 but
// for relocated addresses. In Attila ~48,000 calls per frame, 86% of them 64 bytes or less: the routine's size and
// overlap checks cost more than the copying. ToB battle profile (AGENT_LOG step 2): +0x1B2ACE0 is among the top
// main-thread functions.
// For n <= 64 the copy is done here: every byte of the source is loaded into registers before any byte is stored, so
// overlapping source and destination give the same result as memmove. Larger copies go to the game's routine
// unchanged. Uses EAX/ECX/EDX and XMM0-3 (all caller-saved in the 32-bit ABI) and changes flags, as the original does.
#include <windows.h>
#include <string.h>

static const DWORD MEMFAST_RVAS[2] = {0x1B2ACE0, 0x1B2BBA0};
// The first 48 bytes of both copies (no relocated address in them): prologue, overlap test and size dispatch.
static const unsigned char MEMFAST_ENTRY[48] = {
    0x57, 0x56, 0x8B, 0x74, 0x24, 0x10, 0x8B, 0x4C, 0x24, 0x14, 0x8B, 0x7C, 0x24, 0x0C, 0x8B, 0xC1,
    0x8B, 0xD1, 0x03, 0xC6, 0x3B, 0xFE, 0x76, 0x08, 0x3B, 0xF8, 0x0F, 0x82, 0x94, 0x02, 0x00, 0x00,
    0x83, 0xF9, 0x20, 0x0F, 0x82, 0xD2, 0x04, 0x00, 0x00, 0x81, 0xF9, 0x80, 0x00, 0x00, 0x00, 0x73};
static unsigned char g_memfast_tramp[2][16];  // push edi; push esi; mov esi,[esp+10h] (6 bytes) + jmp back
void *g_memfast_tramp_ptr[2] = {g_memfast_tramp[0], g_memfast_tramp[1]};

#define MEMFAST_BODY(TRAMP)                                                                                           \
    __asm__ volatile(                                                                                                 \
        "movl 12(%esp), %ecx\n\t" /* n */                                                                             \
        "cmpl $64, %ecx\n\t"                                                                                          \
        "ja 90f\n\t"                                                                                                  \
        "movl 4(%esp), %eax\n\t" /* dst (also the return value) */                                                    \
        "movl 8(%esp), %edx\n\t" /* src */                                                                            \
        "cmpl $16, %ecx\n\t"                                                                                          \
        "jbe 20f\n\t"                                                                                                 \
        "cmpl $32, %ecx\n\t"                                                                                          \
        "jbe 10f\n\t"                                                                                                 \
        /* 33..64: four 16-byte pieces (first, second, second-to-last, last), loaded before storing */                \
        "movups (%edx), %xmm0\n\t"                                                                                    \
        "movups 16(%edx), %xmm1\n\t"                                                                                  \
        "movups -32(%edx,%ecx), %xmm2\n\t"                                                                            \
        "movups -16(%edx,%ecx), %xmm3\n\t"                                                                            \
        "movups %xmm0, (%eax)\n\t"                                                                                    \
        "movups %xmm1, 16(%eax)\n\t"                                                                                  \
        "movups %xmm2, -32(%eax,%ecx)\n\t"                                                                            \
        "movups %xmm3, -16(%eax,%ecx)\n\t"                                                                            \
        "ret\n"                                                                                                       \
        "10:\n\t" /* 17..32: first and last 16 bytes */                                                               \
        "movups (%edx), %xmm0\n\t"                                                                                    \
        "movups -16(%edx,%ecx), %xmm1\n\t"                                                                            \
        "movups %xmm0, (%eax)\n\t"                                                                                    \
        "movups %xmm1, -16(%eax,%ecx)\n\t"                                                                            \
        "ret\n"                                                                                                       \
        "20:\n\t"                                                                                                     \
        "cmpl $8, %ecx\n\t"                                                                                           \
        "jb 30f\n\t" /* 8..16: first and last 8 bytes */                                                              \
        "movq (%edx), %xmm0\n\t"                                                                                      \
        "movq -8(%edx,%ecx), %xmm1\n\t"                                                                               \
        "movq %xmm0, (%eax)\n\t"                                                                                      \
        "movq %xmm1, -8(%eax,%ecx)\n\t"                                                                               \
        "ret\n"                                                                                                       \
        "30:\n\t"                                                                                                     \
        "cmpl $4, %ecx\n\t"                                                                                           \
        "jb 40f\n\t" /* 4..7: first and last 4 bytes */                                                               \
        "movd (%edx), %xmm0\n\t"                                                                                      \
        "movd -4(%edx,%ecx), %xmm1\n\t"                                                                               \
        "movd %xmm0, (%eax)\n\t"                                                                                      \
        "movd %xmm1, -4(%eax,%ecx)\n\t"                                                                               \
        "ret\n"                                                                                                       \
        "40:\n\t"                                                                                                     \
        "testl %ecx, %ecx\n\t"                                                                                        \
        "jz 50f\n\t" /* 1..3: first byte, middle byte (n/2), last byte */                                             \
        "pushl %ebx\n\t"                                                                                              \
        "pushl %esi\n\t"                                                                                              \
        "movzbl (%edx), %ebx\n\t"                                                                                     \
        "movl %ecx, %esi\n\t"                                                                                         \
        "shrl $1, %esi\n\t"                                                                                           \
        "movzbl (%edx,%esi), %esi\n\t"                                                                                \
        "movzbl -1(%edx,%ecx), %edx\n\t"                                                                              \
        "movb %bl, (%eax)\n\t"                                                                                        \
        "pushl %ecx\n\t"                                                                                              \
        "shrl $1, %ecx\n\t"                                                                                           \
        "movl %esi, %ebx\n\t"                                                                                         \
        "movb %bl, (%eax,%ecx)\n\t"                                                                                   \
        "popl %ecx\n\t"                                                                                               \
        "movb %dl, -1(%eax,%ecx)\n\t"                                                                                 \
        "popl %esi\n\t"                                                                                               \
        "popl %ebx\n"                                                                                                 \
        "50:\n\t"                                                                                                     \
        "ret\n"                                                                                                       \
        "90:\n\t"                                                                                                     \
        "jmp *" TRAMP "\n\t")

__attribute__((naked)) void memfast_stub(void) { MEMFAST_BODY("_g_memfast_tramp_ptr"); }
__attribute__((naked)) void memfast_stub2(void) { MEMFAST_BODY("_g_memfast_tramp_ptr+4"); }

// Returns the number of copies hooked (each works alone).
int install_memfast(HMODULE m, void (*log)(const char *fmt, ...)) {
    void *stubs[2] = {(void *)memfast_stub, (void *)memfast_stub2};
    int n = 0;
    for (int i = 0; i < 2; i++) {
        unsigned char *site = (unsigned char *)m + MEMFAST_RVAS[i], *t = g_memfast_tramp[i];
        if (memcmp(site, MEMFAST_ENTRY, sizeof MEMFAST_ENTRY) != 0) {
            log("memcpy fast path: +0x%lX differs from this game version - not installed there", MEMFAST_RVAS[i]);
            continue;
        }
        DWORD old;
        if (!VirtualProtect(t, 16, PAGE_EXECUTE_READWRITE, &old)) continue;
        memcpy(t, site, 6);
        t[6] = 0xE9;
        DWORD rel = (DWORD)((site + 6) - (t + 11));
        memcpy(t + 7, &rel, 4);
        FlushInstructionCache(GetCurrentProcess(), t, 16);
        if (!VirtualProtect(site, 6, PAGE_EXECUTE_READWRITE, &old)) continue;
        site[0] = 0xE9;
        rel = (DWORD)((unsigned char *)stubs[i] - (site + 5));
        memcpy(site + 1, &rel, 4);
        site[5] = 0x90;
        VirtualProtect(site, 6, old, &old);
        FlushInstructionCache(GetCurrentProcess(), site, 6);
        n++;
    }
    log("memcpy fast path: %d of 2 copies hooked (copies of 64 bytes or less)", n);
    return n;
}
