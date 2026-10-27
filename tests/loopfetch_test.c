// Test for loopfetch.c: every stub runs with the register and stack state its loop has, for every index of arrays
// of 1..40 items placed at the very end of a page followed by a no-access page, with every slot past the current
// index holding a pointer into that no-access page (a stale pointer: the stub may only prefetch through it), with
// prefetching on and off. Afterwards the registers must equal what the original instructions produce and edx must
// be unchanged. Then install_loopfetch must accept the real game code (and refuse a second time).
// From the Attila mod (heapfix/loopfetch_test.c), for ToB's three sites.
// usage: loopfetch_test.exe <path to empire.retail.dll>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include "../loopfetch.c"

#define CHECK(c) do { if (!(c)) { printf("FAIL line %d (site %u, count %u, i %u): %s\n", __LINE__, k, count, i, #c); exit(1); } } while (0)
#define CHECK0(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); exit(1); } } while (0)

unsigned g_r[8];  // eax ecx edx ebx esp ebp esi edi as seen by the pad
__attribute__((naked)) void pad(void) {
    __asm__ volatile("pushal\n\t"
                     "movl 28(%esp), %eax\n\tmovl %eax, _g_r\n\t"
                     "movl 24(%esp), %eax\n\tmovl %eax, _g_r+4\n\t"
                     "movl 20(%esp), %eax\n\tmovl %eax, _g_r+8\n\t"
                     "movl 16(%esp), %eax\n\tmovl %eax, _g_r+12\n\t"
                     "movl 8(%esp), %eax\n\tmovl %eax, _g_r+20\n\t"
                     "movl 4(%esp), %eax\n\tmovl %eax, _g_r+24\n\t"
                     "movl 0(%esp), %eax\n\tmovl %eax, _g_r+28\n\t"
                     "popal\n\tret\n\t");
}
enum { EAX, ECX, EDX, EBX, ESP_, EBP, ESI, EDI };
static unsigned char *g_stub[NSITES];
static void say(const char *fmt, ...) { va_list a; va_start(a, fmt); vprintf(fmt, a); va_end(a); putchar('\n'); }

unsigned g_in[7], g_fill;
unsigned char *g_cur_stub;
// Calls a stub with eax, ecx, ebx, ebp, esi, edi set; `fill` dwords sit between the return address and `end`,
// so that [esp + 4*(fill+1)] at stub entry is the loop's end value.
static void run(unsigned k, unsigned eax, unsigned ecx, unsigned ebx, unsigned ebp, unsigned esi, unsigned edi,
                unsigned end, int fill) {
    g_in[0] = eax; g_in[1] = ecx; g_in[2] = ebx; g_in[3] = ebp; g_in[4] = esi; g_in[5] = edi; g_in[6] = end;
    g_fill = (unsigned)fill;
    g_cur_stub = g_stub[k];
    __asm__ volatile(
        "pushal\n\t"
        "movl _g_fill, %%ecx\n\t"
        "pushl _g_in+24\n\t"
        "1: testl %%ecx, %%ecx\n\tjz 2f\n\tpushl $0x11111111\n\tdecl %%ecx\n\tjmp 1b\n\t"
        "2: movl _g_in, %%eax\n\tmovl _g_in+4, %%ecx\n\tmovl _g_in+8, %%ebx\n\tmovl _g_in+12, %%ebp\n\t"
        "movl _g_in+16, %%esi\n\tmovl _g_in+20, %%edi\n\tmovl $0x5EED5EED, %%edx\n\t"
        "calll *_g_cur_stub\n\t"
        "movl _g_fill, %%ecx\n\t"
        "leal 4(%%esp,%%ecx,4), %%esp\n\t"
        "popal\n\t"
        ::: "memory", "cc");
}

int main(int argc, char **argv) {
    CHECK0(argc == 2);
    unsigned char *page = VirtualAlloc(NULL, 8192, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    CHECK0(page);
    for (unsigned k = 0; k < NSITES; k++) {  // fake site k: the stub jumps back to site+len, where a jmp to the pad sits
        unsigned char *site = page + 256 * k;
        site[SITES[k].len] = 0xE9;
        void *to = (void *)pad;
        put32(site + SITES[k].len + 1, (DWORD)((unsigned char *)to - (site + SITES[k].len + 5)));
        g_stub[k] = page + 4096 + 256 * k;
        build(g_stub[k], &SITES[k], site);
    }
    FlushInstructionCache(GetCurrentProcess(), page, 8192);

    static unsigned items[128][64];
    for (int j = 0; j < 128; j++) items[j][0] = 1000 + j;
    unsigned char *arr_pages = VirtualAlloc(NULL, 8192, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    DWORD old;
    CHECK0(VirtualProtect(arr_pages + 4096, 4096, PAGE_NOACCESS, &old));
    unsigned stale = (unsigned)(UINT_PTR)(arr_pages + 4096 + 64);
    static unsigned list[4], frame[16];
    long runs = 0;
    for (int on = 0; on < 2; on++) {
        g_loopfetch_on = (unsigned char)on;
        for (unsigned count = 1; count <= 40; count++) {
            unsigned *arr = (unsigned *)(arr_pages + 4096 - 4 * count);  // ends exactly at the no-access page
            list[2] = (unsigned)(UINT_PTR)arr;                            // list+8 = array
            for (unsigned i = 0; i < count; i++) {
                for (unsigned j = 0; j < count; j++) arr[j] = j <= i ? (unsigned)(UINT_PTR)items[(j * 7) % 128] : stale;
                unsigned A = (unsigned)(UINT_PTR)arr, L = (unsigned)(UINT_PTR)list, F = (unsigned)(UINT_PTR)frame;
                frame[0x0C / 4] = L;
                frame[0x18 / 4] = count;
                for (unsigned k = 0; k < NSITES; k++) {
                    // ebp = frame ([+0Ch] list, [+18h] end), ecx = i: afterwards eax = array, nothing else changed
                    run(k, 0xAAAA, i, 0xBBBB, F, 0xE51E51, 0xD1D1, 0, 0);
                    CHECK(g_r[EAX] == A && g_r[ECX] == i && g_r[EBP] == F && g_r[EBX] == 0xBBBB && g_r[ESI] == 0xE51E51 && g_r[EDI] == 0xD1D1);
                    CHECK(g_r[EDX] == 0x5EED5EED);
                    runs++;
                }
            }
        }
    }
    printf("%ld stub runs over %u sites (arrays of 1..40 items ending at a no-access page, unreached slots pointing into it, prefetch on/off): registers match the original instructions\n",
           runs, (unsigned)NSITES);

    HMODULE m = LoadLibraryExA(argv[1], NULL, DONT_RESOLVE_DLL_REFERENCES);
    CHECK0(m);
    CHECK0(install_loopfetch(m, say) == 3);
    CHECK0(install_loopfetch(m, say) == 0);
    printf("all loop prefetch tests passed\n");
    return 0;
}
