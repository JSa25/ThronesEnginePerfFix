// Test for frame.c on the real empire.retail.dll mapped without running it: the hook goes in (jump to the stub, the
// trampoline holds the original 8 bytes and jumps back behind them), a second install and a changed byte are refused,
// and the stub calls the per-frame check and then continues into the trampoline with registers intact.
// usage: frame_test.exe <path to empire.retail.dll>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include "../frame.c"

#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); exit(1); } } while (0)
static void say(const char *fmt, ...) { va_list a; va_start(a, fmt); vprintf(fmt, a); va_end(a); putchar('\n'); }

static int g_checks;
static void check(void) { g_checks++; }
static int __cdecl stand_in(int x) { return x * 3; }  // where the trampoline would go: returns to the stub's caller

int main(int argc, char **argv) {
    CHECK(argc == 2);
    HMODULE m = LoadLibraryExA(argv[1], NULL, DONT_RESOLVE_DLL_REFERENCES);
    CHECK(m);
    unsigned char *site = (unsigned char *)m + RVA_FRAME;
    unsigned char orig[8];
    memcpy(orig, site, 8);
    CHECK(install_frame(m, say) == 1);
    DWORD rel;
    memcpy(&rel, site + 1, 4);
    CHECK(site[0] == 0xE9 && site + 5 + rel == (unsigned char *)frame_stub);
    CHECK(!memcmp(g_tramp, orig, 8) && g_tramp[8] == 0xE9);
    memcpy(&rel, g_tramp + 9, 4);
    CHECK(g_tramp + 13 + rel == site + 8);
    CHECK(install_frame(m, say) == 0);  // already hooked: bytes differ

    g_frame_check = check;
    g_frame_tramp_ptr = (void *)stand_in;
    int r = ((int(__cdecl *)(int))(void *)frame_stub)(14);
    CHECK(r == 42 && g_checks == 1);
    g_frame_check = NULL;
    r = ((int(__cdecl *)(int))(void *)frame_stub)(5);
    CHECK(r == 15 && g_checks == 1);

    FreeLibrary(m);
    m = LoadLibraryExA(argv[1], NULL, DONT_RESOLVE_DLL_REFERENCES);
    CHECK(m);
    site = (unsigned char *)m + RVA_FRAME;
    DWORD old;
    VirtualProtect(site + 2, 1, PAGE_EXECUTE_READWRITE, &old);
    site[2] ^= 1;  // a different game version
    CHECK(install_frame(m, say) == 0 && site[0] == 0x83);
    printf("all frame tests passed\n");
    return 0;
}
