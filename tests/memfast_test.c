// Tests memfast.c with the game's own memcpy/memmove (both copies in the mapped empire.retail.dll; leaf code, runs
// without the game). The unhooked routines and then the hooked entries must equal memmove for every size 0..200,
// source alignments 0..15 and overlap distances -80..80 (return value dst, every byte of the buffer, EBX/ESI/EDI/EBP
// preserved). Large copies go through the trampoline into the game's routine. Then a changed entry byte is refused.
// usage: memfast_test.exe <path to empire.retail.dll>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include "../memfast.c"

#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); exit(1); } } while (0)
static void say(const char *fmt, ...) { va_list a; va_start(a, fmt); vprintf(fmt, a); va_end(a); putchar('\n'); }

typedef void *(__cdecl *copy_fn)(void *, const void *, unsigned);
copy_fn g_copy;
unsigned g_after[4];

// call_checked(d, s, n): calls g_copy with EBX/ESI/EDI/EBP set to markers and stores them afterwards.
void *call_checked(void *d, const void *s, unsigned n);
__asm__(
    ".globl _call_checked\n"
    "_call_checked:\n\t"
    "pushl %ebx\n\tpushl %esi\n\tpushl %edi\n\tpushl %ebp\n\t"
    "movl 28(%esp), %eax\n\tpushl %eax\n\t"   // n   (args at +20/+24/+28 after the 4 pushes)
    "movl 28(%esp), %eax\n\tpushl %eax\n\t"   // s
    "movl 28(%esp), %eax\n\tpushl %eax\n\t"   // d
    "movl $0x11111111, %ebx\n\tmovl $0x22222222, %esi\n\tmovl $0x33333333, %edi\n\tmovl $0x44444444, %ebp\n\t"
    "call *_g_copy\n\t"
    "addl $12, %esp\n\t"
    "movl %ebx, _g_after\n\tmovl %esi, _g_after+4\n\tmovl %edi, _g_after+8\n\tmovl %ebp, _g_after+12\n\t"
    "popl %ebp\n\tpopl %edi\n\tpopl %esi\n\tpopl %ebx\n\t"
    "ret\n");

static int markers_ok(void) { return g_after[0] == 0x11111111 && g_after[1] == 0x22222222 && g_after[2] == 0x33333333 && g_after[3] == 0x44444444; }

// Runs every case through `ref` and `hooked` on two identical buffers; returns the number of cases (all must match).
static long compare(copy_fn ref, copy_fn hooked, unsigned maxn, int big) {
    static unsigned char a[8192], b[8192];
    long cases = 0;
    int span = big ? 2400 : 80, base = big ? 2600 : 160;
    for (unsigned n = 0; n <= maxn; n += big ? 37 : 1)
        for (int sa = 0; sa < 16; sa += big ? 5 : 1)
            for (int delta = -span; delta <= span; delta += big ? 61 : 1) {
                int so = base + sa, dofs = so + delta;
                for (int i = 0; i < 8192; i++) a[i] = b[i] = (unsigned char)(i * 7 + n + sa);
                g_copy = ref;
                void *r1 = call_checked(a + dofs, a + so, n);
                CHECK(r1 == a + dofs && markers_ok());
                g_copy = hooked;
                void *r2 = call_checked(b + dofs, b + so, n);
                CHECK(r2 == b + dofs && markers_ok());
                CHECK(memcmp(a, b, sizeof a) == 0);
                cases++;
            }
    return cases;
}

int main(int argc, char **argv) {
    CHECK(argc == 2);
    HMODULE m = LoadLibraryExA(argv[1], NULL, DONT_RESOLVE_DLL_REFERENCES);
    CHECK(m);
    // First the unhooked game routines against the C library memmove (they are memmove), then the hooked entries
    // against the same reference: small copies run in our stub, large ones through the trampoline in the game's code.
    copy_fn crt = (copy_fn)(void *)memmove;
    copy_fn game[2] = {(copy_fn)(void *)((unsigned char *)m + MEMFAST_RVAS[0]), (copy_fn)(void *)((unsigned char *)m + MEMFAST_RVAS[1])};
    long pre = 0;
    for (int i = 0; i < 2; i++) pre += compare(crt, game[i], 200, 0);  // the game's routines are memmove
    printf("unhooked: both game routines match memmove on %ld cases\n", pre);
    CHECK(install_memfast(m, say) == 2);
    long cases = 0;
    for (int i = 0; i < 2; i++) cases += compare(crt, game[i], 200, 0);
    printf("hooked: %ld copies (sizes 0..200, 16 alignments, overlaps -80..80) identical to memmove\n", cases);
    long big = 0;
    for (int i = 0; i < 2; i++) big += compare(crt, game[i], 2100, 1);
    printf("hooked: %ld large copies (65..2100 bytes, through the trampoline to the game's routine) identical\n", big);
    CHECK(install_memfast(m, say) == 0);  // already hooked: entry bytes differ

    FreeLibrary(m);
    m = LoadLibraryExA(argv[1], NULL, DONT_RESOLVE_DLL_REFERENCES);
    CHECK(m);
    unsigned char *e = (unsigned char *)m + MEMFAST_RVAS[1] + 33;
    DWORD old;
    VirtualProtect(e, 1, PAGE_EXECUTE_READWRITE, &old);
    *e ^= 1;  // a different game version (second copy only)
    CHECK(install_memfast(m, say) == 1);
    printf("all memfast tests passed\n");
    return 0;
}
