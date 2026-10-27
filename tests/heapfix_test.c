// Heap test (heapfix.c): semantics, a 32-thread stress run, cross-thread frees, memory overhead and speed, and
// patching a real copy of empire.retail.dll mapped without running it. Blocks over 16 KB go to the game's own
// dlmalloc, which cannot run in an image mapped this way (imports not resolved): lateload_test covers those.
// usage: heapfix_test.exe <path to empire.retail.dll>
#include "../heapfix.c"
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <psapi.h>

#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); exit(1); } } while (0)

static void tlog(const char *fmt, ...) {
    va_list a;
    va_start(a, fmt);
    vprintf(fmt, a);
    va_end(a);
    putchar('\n');
}

static volatile LONG g_errors;

static DWORD WINAPI stress(LPVOID arg) {
    unsigned seed = (unsigned)(UINT_PTR)arg * 2654435761u;
    enum { SLOTS = 512 };
    unsigned char *slot[SLOTS] = {0};
    size_t len[SLOTS] = {0};
    for (int it = 0; it < 300000; it++) {
        seed = seed * 1103515245u + 12345u;
        int s = (seed >> 8) % SLOTS;
        if (slot[s]) {
            for (size_t k = 0; k < len[s]; k += 61)
                if (slot[s][k] != (unsigned char)(s + k)) InterlockedIncrement(&g_errors);
            if (seed & 0x10000) {
                size_t n = 81 + (seed >> 17) % 8000;
                unsigned char *p = hf_realloc(slot[s], n);
                if (!p) { InterlockedIncrement(&g_errors); continue; }
                size_t keep = n < len[s] ? n : len[s];
                for (size_t k = 0; k < keep; k += 61)
                    if (p[k] != (unsigned char)(s + k)) InterlockedIncrement(&g_errors);
                for (size_t k = 0; k < n; k++) p[k] = (unsigned char)(s + k);
                slot[s] = p;
                len[s] = n;
            } else {
                hf_free(slot[s]);
                slot[s] = NULL;
            }
        } else {
            size_t n = 81 + (seed >> 17) % 8000;
            slot[s] = hf_malloc(n);
            if (!slot[s]) { InterlockedIncrement(&g_errors); continue; }
            for (size_t k = 0; k < n; k++) slot[s][k] = (unsigned char)(s + k);
            len[s] = n;
        }
    }
    for (int s = 0; s < SLOTS; s++) hf_free(slot[s]);
    return 0;
}

// Cross-thread queue: the producer allocates and fills blocks, the consumer checks and frees them.
struct xq { volatile LONG head, tail; void *slot[4096]; size_t len[4096]; int seed; };

static DWORD WINAPI xproducer(LPVOID arg) {
    struct xq *q = arg;
    unsigned seed = q->seed;
    for (int i = 0; i < 200000; i++) {
        while (q->head - q->tail >= 4096) SwitchToThread();
        seed = seed * 1103515245u + 12345u;
        size_t n = 81 + (seed >> 8) % 4000;
        unsigned char *p = hf_malloc(n);
        if (!p) { InterlockedIncrement(&g_errors); return 0; }
        memset(p, (unsigned char)i, n);
        q->slot[q->head % 4096] = p;
        q->len[q->head % 4096] = n;
        MemoryBarrier();
        InterlockedIncrement(&q->head);
    }
    return 0;
}

static DWORD WINAPI xconsumer(LPVOID arg) {
    struct xq *q = arg;
    for (int i = 0; i < 200000; i++) {
        while (q->tail == q->head) SwitchToThread();
        MemoryBarrier();
        unsigned char *p = q->slot[q->tail % 4096];
        size_t n = q->len[q->tail % 4096];
        if (p[0] != (unsigned char)i || p[n - 1] != (unsigned char)i) InterlockedIncrement(&g_errors);
        hf_free(p);
        InterlockedIncrement(&q->tail);
    }
    return 0;
}

static HANDLE g_bench_heap;  // NULL: this heap, else a Windows heap

static DWORD WINAPI bench(LPVOID arg) {
    unsigned seed = (unsigned)(UINT_PTR)arg;
    void *keep[64] = {0};
    for (int i = 0; i < 2000000; i++) {
        seed = seed * 1103515245u + 12345u;
        int k = (seed >> 3) & 63;
        size_t n = 81 + (seed >> 10) % 400;
        if (g_bench_heap) { if (keep[k]) HeapFree(g_bench_heap, 0, keep[k]); keep[k] = HeapAlloc(g_bench_heap, 0, n); }
        else { hf_free(keep[k]); keep[k] = hf_malloc(n); }
    }
    for (int k = 0; k < 64; k++) { if (g_bench_heap) HeapFree(g_bench_heap, 0, keep[k]); else hf_free(keep[k]); }
    return 0;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);  // a crash must not swallow the progress lines
    CHECK(argc == 2);
    // Map the real game DLL: relocations applied, no imports resolved, DllMain not run.
    HMODULE m = LoadLibraryExA(argv[1], NULL, DONT_RESOLVE_DLL_REFERENCES);
    CHECK(m != NULL);
    printf("mapped empire.retail.dll at %p\n", (void *)m);

    CHECK(install_heapfix(m, tlog) == (int)NHEAP - 1);
    for (size_t i = 0; i < NHEAP; i++) {
        if (!g_heap_patches[i].to) continue;  // dltrim: checked only
        unsigned char *site = (unsigned char *)m + g_heap_patches[i].rva;
        DWORD rel;
        memcpy(&rel, site + 1, 4);
        CHECK(site[0] == 0xE9 && site + 5 + rel == (unsigned char *)g_heap_patches[i].to);
    }
    CHECK(install_heapfix(m, tlog) == 0);  // already patched: bytes no longer match, so it refuses

    // Through the game's own patched entry points (small blocks only, see above).
    void *(__cdecl *g_malloc)(size_t) = (void *)((unsigned char *)m + 0x12B640);
    void(__cdecl *g_free)(void *) = (void *)((unsigned char *)m + 0x12AFF0);
    void *(__cdecl *g_realloc)(void *, size_t) = (void *)((unsigned char *)m + 0x12B900);
    char *p = g_malloc(100);
    CHECK(p && ((UINT_PTR)p & 7) == 0);
    strcpy(p, "thrones");
    p = g_realloc(p, 10000);
    CHECK(p && strcmp(p, "thrones") == 0);
    p = g_realloc(p, 0);
    CHECK(p != NULL);
    g_free(p);
    g_free(NULL);
    CHECK(g_malloc(0) != NULL);
    char *q = g_malloc(32);
    CHECK(g_realloc(q, 0xFFFFFFF0u) == NULL);  // fails and keeps q
    q[31] = 1;
    g_free(q);
    q = g_realloc(NULL, 50);
    CHECK(q != NULL);
    g_free(q);
    printf("semantics ok\n");

    enum { T = 32 };
    HANDLE th[T];
    DWORD t0 = GetTickCount();
    for (int i = 0; i < T; i++) th[i] = CreateThread(NULL, 0, stress, (LPVOID)(UINT_PTR)(i + 1), 0, NULL);
    WaitForMultipleObjects(T, th, TRUE, INFINITE);
    printf("stress: %d threads x 300000 ops in %lu ms, errors %ld\n", T, GetTickCount() - t0, g_errors);
    CHECK(g_errors == 0);

    {
        enum { PAIRS = 8 };
        HANDLE ht[PAIRS * 2];
        for (int i = 0; i < PAIRS; i++) {
            struct xq *xq = calloc(1, sizeof *xq);
            xq->seed = i + 1;
            ht[2 * i] = CreateThread(NULL, 0, xproducer, xq, 0, NULL);
            ht[2 * i + 1] = CreateThread(NULL, 0, xconsumer, xq, 0, NULL);
        }
        WaitForMultipleObjects(PAIRS * 2, ht, TRUE, INFINITE);
        printf("cross-thread: %d x 200000 blocks allocated on one thread and freed on another, errors %ld\n", PAIRS, g_errors);
        CHECK(g_errors == 0);
    }

    {  // footprint during a game-like churn stays bounded and drops after freeing
        PROCESS_MEMORY_COUNTERS_EX pm = {.cb = sizeof pm};
        K32GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&pm, sizeof pm);
        size_t before = pm.PrivateUsage;
        enum { LIVE = 300000 };  // ~300 MB live, within the 512 MB reservation
        void **live = malloc(sizeof(void *) * LIVE);
        unsigned seed = 7;
        size_t want = 0;
        for (int i = 0; i < LIVE; i++) { seed = seed * 1103515245u + 12345u; size_t n = 81 + (seed >> 8) % 1920; live[i] = hf_malloc(n); memset(live[i], 1, n); want += n; }
        for (int r = 0; r < 5000000; r++) {
            seed = seed * 1103515245u + 12345u;
            int i = (seed >> 4) % LIVE;
            hf_free(live[i]);
            size_t n = 81 + (seed >> 12) % 1920;
            live[i] = hf_malloc(n);
            ((char *)live[i])[n - 1] = 2;
        }
        K32GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&pm, sizeof pm);
        size_t during = pm.PrivateUsage - before;
        for (int i = 0; i < LIVE; i++) hf_free(live[i]);
        free(live);
        mi_collect(true);
        K32GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&pm, sizeof pm);
        size_t after = pm.PrivateUsage > before ? pm.PrivateUsage - before : 0;
        printf("memory: ~%zu MB requested live, %zu MB private while live (%.2fx), %zu MB left after freeing\n",
               want >> 20, during >> 20, (double)during / want, after >> 20);
        CHECK(during < want * 3 / 2);
        CHECK(after < 64u << 20);
    }

    {  // speed: the same churn on 32 threads, this heap vs the Windows heap
        HANDLE wh = HeapCreate(0, 0, 0);
        ULONG lfh = 2;
        HeapSetInformation(wh, HeapCompatibilityInformation, &lfh, sizeof lfh);
        for (int pass = 0; pass < 2; pass++) {
            g_bench_heap = pass ? wh : NULL;
            HANDLE bt[32];
            LARGE_INTEGER f, a, b;
            QueryPerformanceFrequency(&f);
            QueryPerformanceCounter(&a);
            for (int i = 0; i < 32; i++) bt[i] = CreateThread(NULL, 0, bench, (LPVOID)(UINT_PTR)(i + 1), 0, NULL);
            WaitForMultipleObjects(32, bt, TRUE, INFINITE);
            QueryPerformanceCounter(&b);
            double s = (b.QuadPart - a.QuadPart) / (double)f.QuadPart;
            printf("speed (%s): 32 threads x 2M alloc+free in %.2f s = %.0f M ops/s\n", pass ? "Windows heap" : "this heap", s, 32 * 4.0 / s);
        }
    }

    // Refuses when dlmalloc was already used: fresh mapping with mparams.magic set.
    FreeLibrary(m);
    m = LoadLibraryExA(argv[1], NULL, DONT_RESOLVE_DLL_REFERENCES);
    CHECK(m != NULL);
    DWORD old;
    DWORD *magic = (DWORD *)((unsigned char *)m + RVA_MPARAMS_MAGIC);
    VirtualProtect(magic, 4, PAGE_READWRITE, &old);
    *magic = 0x12345678;
    CHECK(install_heapfix(m, tlog) == 0);
    CHECK(((unsigned char *)m + 0x12B640)[0] == 0x83);  // untouched

    // Refuses a different game version: fresh mapping with one byte of dlfree changed, nothing patched.
    FreeLibrary(m);
    m = LoadLibraryExA(argv[1], NULL, DONT_RESOLVE_DLL_REFERENCES);
    CHECK(m != NULL);
    unsigned char *fb = (unsigned char *)m + 0x12AFF0 + 6;
    VirtualProtect(fb, 1, PAGE_EXECUTE_READWRITE, &old);
    *fb ^= 1;
    CHECK(install_heapfix(m, tlog) == 0);
    CHECK(((unsigned char *)m + 0x12B640)[0] == 0x83);
    printf("all heap tests passed\n");
    return 0;
}
