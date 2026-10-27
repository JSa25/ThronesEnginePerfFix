// Heap fix (port of the Attila CPU Performance Fix 6.1 heap). Every game allocation over 80 bytes goes through one
// dlmalloc 2.8.x heap with a single spinlock; on the campaign map 16 worker threads spend ~45-48% of their time and the
// main thread 34-42% spinning on it (AGENT_LOG step 2, vanilla 1080p). This sends blocks up to 16 KB (all but ~0.02%
// of the calls in Attila) to mimalloc, which does not serialize threads, and keeps larger ones on the game's own
// dlmalloc, called through copies of its first instructions (dlmalloc gives big blocks' address space back, which the
// 32-bit game needs).
// mimalloc gets its own capped reservation (512 MB, grown in 32 MB best-fit pieces to 768 MB; past that small blocks
// spill to dlmalloc), because its own arenas grow by doubling and ran a heavy Attila setup out of address space.
// ToB's dlmalloc is the same code as Attila's (same first bytes, same layout); only the addresses differ.
#include <windows.h>
#include <string.h>
#include <mimalloc.h>

#define DL_MAX_REQUEST 0xFFFFFFC0u     // dlmalloc's own limit
#define PREFERRED_BASE 0x10000000u
#define RVA_MPARAMS_MAGIC 0x02006274u  // dlmalloc mparams.magic: non-zero once dlmalloc was used
#define SMALL_MAX 16384

static void (*g_log)(const char *fmt, ...);

// The game's dlmalloc (set by install_heapfix): trampolines that run its first instructions, then the rest of it.
static void *(__cdecl *dl_malloc)(size_t);
static void(__cdecl *dl_free)(void *);
static void *(__cdecl *dl_realloc)(void *, size_t);

void heap_address_space(size_t *free_total, size_t *largest) {
    MEMORY_BASIC_INFORMATION m;
    *free_total = *largest = 0;
    for (unsigned char *a = (unsigned char *)0x10000; VirtualQuery(a, &m, sizeof m) == sizeof m; a = (unsigned char *)m.BaseAddress + m.RegionSize) {
        if (m.State == MEM_FREE) {
            *free_total += m.RegionSize;
            if (m.RegionSize > *largest) *largest = m.RegionSize;
        }
        if ((unsigned char *)m.BaseAddress + m.RegionSize < a) break;  // wrapped past the top
    }
}

static void log_failure(const char *what, size_t n) {
    size_t free_total, largest;
    heap_address_space(&free_total, &largest);
    if (g_log)
        g_log("%s of %u bytes FAILED (address space: %u MB free, largest free block %u MB)", what, (unsigned)n,
              (unsigned)(free_total >> 20), (unsigned)(largest >> 20));
}

// mimalloc's reservations: 512 MB, grown in 32 MB steps up to 768 MB when full. A full mimalloc searches its whole
// reservation before failing (~400x slower than a normal allocation, measured in Attila), so after a failure it is
// skipped for 50 ms; frees keep returning space to it.
#define SMALL_HEAP_MB 512
#define SMALL_STEP_MB 32  // the smallest piece mimalloc can use on 32-bit (page info every 32 MiB)
#define SMALL_LIMIT_MB 768
static volatile LONG g_small_mb;  // reserved for mimalloc so far; 0 = no cap (the first reservation failed)
static volatile DWORD g_mi_full_until;
static SRWLOCK g_grow_lock = SRWLOCK_INIT;

#ifdef PROFILING
// Profiling builds: bytes live on dlmalloc (from its chunk headers), lowest free address space seen, and a log line
// every 20 s.
#include <mimalloc-stats.h>
static volatile LONG g_dl_live, g_dl_calls, g_spill_calls;
static size_t dl_chunk(void *p) { return p ? (((size_t *)p)[-1] & ~(size_t)7) : 0; }
static void *dl_malloc_c(size_t n) {
    InterlockedIncrement(&g_dl_calls);
    if (n <= SMALL_MAX) InterlockedIncrement(&g_spill_calls);
    void *p = dl_malloc(n);
    InterlockedExchangeAdd(&g_dl_live, (LONG)dl_chunk(p));
    return p;
}
static void dl_free_c(void *p) { InterlockedIncrement(&g_dl_calls); InterlockedExchangeAdd(&g_dl_live, -(LONG)dl_chunk(p)); dl_free(p); }
static void *dl_realloc_c(void *p, size_t n) {
    InterlockedIncrement(&g_dl_calls);
    size_t was = dl_chunk(p);
    void *q = dl_realloc(p, n);
    if (q) InterlockedExchangeAdd(&g_dl_live, (LONG)dl_chunk(q) - (LONG)was);
    return q;
}
static DWORD WINAPI memstats(LPVOID a) {
    (void)a;
    size_t min_free = (size_t)-1, min_largest = (size_t)-1;
    for (;;) {
        Sleep(20000);
        mi_stats_t st;
        mi_stats_init(&st);
        mi_stats_get(&st);
        size_t fr, big;
        heap_address_space(&fr, &big);
        if (fr < min_free) min_free = fr;
        if (big < min_largest) min_largest = big;
        LONG calls = InterlockedExchange(&g_dl_calls, 0), spills = InterlockedExchange(&g_spill_calls, 0);
        g_log("memstats: mimalloc %ld MB reserved, committed %u MB, live small %u MB; dlmalloc live %u MB, %ld calls/s "
              "(%ld small spills/s); address space free %u MB (lowest %u), largest %u MB (lowest %u)", g_small_mb,
              (unsigned)(st.committed.current >> 20), (unsigned)(st.malloc_normal.current >> 20), (unsigned)(g_dl_live >> 20),
              calls / 20, spills / 20, (unsigned)(fr >> 20), (unsigned)(min_free >> 20), (unsigned)(big >> 20),
              (unsigned)(min_largest >> 20));
    }
    return 0;
}
#define DL_MALLOC dl_malloc_c
#define DL_FREE dl_free_c
#define DL_REALLOC dl_realloc_c
#else
#define DL_MALLOC dl_malloc
#define DL_FREE dl_free
#define DL_REALLOC dl_realloc
#endif

// mimalloc's first reservation, 32 MiB aligned as mimalloc wants on 32-bit, taken bottom-up while the address space is
// still empty (at the top it left too small a largest free block in Attila: 142 vs 377 MB in a 4K Mk1212 battle).
static int reserve_small_heap(size_t size) {
    const size_t align = 32u << 20;
    unsigned char *p = VirtualAlloc(NULL, size + align, MEM_RESERVE, PAGE_NOACCESS);
    if (!p) return 0;
    unsigned char *a = (unsigned char *)(((UINT_PTR)p + align - 1) & ~(UINT_PTR)(align - 1));
    VirtualFree(p, 0, MEM_RELEASE);
    if (VirtualAlloc(a, size, MEM_RESERVE, PAGE_NOACCESS) != a) return 0;
    if (mi_manage_os_memory(a, size, false, false, true, -1)) return 1;
    VirtualFree(a, 0, MEM_RELEASE);
    return 0;
}

// A growth step comes late in a session, when the address space is already in pieces. A plain reservation takes the
// lowest gap with room, i.e. it cuts into the big free gaps the game needs for its large blocks (Attila 6.0, a player's
// MK1212 campaign crashed that way). So each 32 MB step goes into the smallest free gap that holds an aligned 32 MB
// piece, at the end of that gap that leaves the larger part free.
static unsigned char *best_fit_piece(size_t size) {
    const UINT_PTR align = 32u << 20;
    MEMORY_BASIC_INFORMATION m;
    unsigned char *best = NULL;
    size_t best_gap = ~(size_t)0, best_left = 0;
    for (unsigned char *a = (unsigned char *)0x10000; VirtualQuery(a, &m, sizeof m) == sizeof m; a = (unsigned char *)m.BaseAddress + m.RegionSize) {
        if (m.State == MEM_FREE) {
            UINT_PTR lo = (UINT_PTR)m.BaseAddress, hi = lo + m.RegionSize;
            UINT_PTR first = (lo + align - 1) & ~(align - 1), last = (hi - size) & ~(align - 1);
            if (hi >= size && first >= lo && last >= first && last + size <= hi) {
                // the larger free part left over: below the first piece or above the last one
                size_t keep_first = hi - (first + size) > first - lo ? hi - (first + size) : first - lo;
                size_t keep_last = last - lo > hi - (last + size) ? last - lo : hi - (last + size);
                UINT_PTR at = keep_last > keep_first ? last : first;
                size_t left = keep_last > keep_first ? keep_last : keep_first;
                if (m.RegionSize < best_gap || (m.RegionSize == best_gap && left > best_left)) {
                    best = (unsigned char *)at;
                    best_gap = m.RegionSize;
                    best_left = left;
                }
            }
        }
        if ((unsigned char *)m.BaseAddress + m.RegionSize < a) break;  // wrapped past the top
    }
    return best;
}

// One growth step for mimalloc: 1 when it got another 32 MB.
static int grow_small_heap_step(void) {
    const size_t size = (size_t)SMALL_STEP_MB << 20;
    for (int tries = 0; tries < 4; tries++) {  // another thread may take the gap between the search and the reservation
        unsigned char *p = best_fit_piece(size);
        if (!p) return 0;
        if (VirtualAlloc(p, size, MEM_RESERVE, PAGE_NOACCESS) != p) continue;
        if (mi_manage_os_memory(p, size, false, false, true, -1)) return 1;
        VirtualFree(p, 0, MEM_RELEASE);
        return 0;
    }
    return 0;
}

// Called after mimalloc returned NULL: 1 if it has more room now (this thread or another one added a step).
static int grow_small_heap(LONG seen) {
    if (seen == 0 || seen >= SMALL_LIMIT_MB) return 0;
    AcquireSRWLockExclusive(&g_grow_lock);
    int ok = g_small_mb != seen;
    if (!ok && g_small_mb < SMALL_LIMIT_MB) {
        ok = grow_small_heap_step();
        g_small_mb = ok ? g_small_mb + SMALL_STEP_MB : SMALL_LIMIT_MB;  // no room for a step: stop trying
        if (g_log)
            g_log(ok ? "memory: the mod's small-block heap grew to %ld MB (normal with big mods; the limit is 768 MB)"
                     : "memory: the mod's small-block heap is full at %ld MB and found no room to grow - small blocks also "
                       "use the game's own heap from now on (works, a little slower)", g_small_mb);
    }
    ReleaseSRWLockExclusive(&g_grow_lock);
    return ok;
}

// The clock is only read while a skip window is set (g_mi_full_until != 0), not on every allocation.
static int mi_skipped(void) {
    DWORD until = g_mi_full_until;
    if (!until) return 0;
    if ((LONG)(GetTickCount() - until) < 0) return 1;
    g_mi_full_until = 0;  // window over: try mimalloc again
    return 0;
}

static void mi_full(void) { g_mi_full_until = (GetTickCount() + 50) | 1; }  // | 1: never 0

// Requests mimalloc does not take (big, huge, mimalloc full) go to dlmalloc unchanged, so its own limits and failure
// behaviour stay (it fails requests >= 0xFFFFFFC0 with NULL; realloc failures keep the old block).
void *__cdecl hf_malloc(size_t n) {
    if (n <= SMALL_MAX && !mi_skipped()) {
        LONG seen = g_small_mb;
        void *p = mi_malloc(n ? n : 1);
        if (p || (grow_small_heap(seen) && (p = mi_malloc(n ? n : 1)))) return p;
        mi_full();
    }
    void *p = DL_MALLOC(n);
    if (!p) log_failure("malloc", n);
    return p;
}

void __cdecl hf_free(void *p) {
    if (!p) return;
    if (mi_is_in_heap_region(p)) mi_free(p);
    else DL_FREE(p);
}

void *__cdecl hf_realloc(void *p, size_t n) {
    if (!p) return hf_malloc(n);
    if (!mi_is_in_heap_region(p)) {  // large blocks stay with dlmalloc, even when shrinking
        void *q = DL_REALLOC(p, n);
        if (!q && n) log_failure("realloc", n);
        return q;
    }
    if (n >= DL_MAX_REQUEST) return NULL;  // as dlrealloc: fails, the old block stays valid
    if (n <= SMALL_MAX && !mi_skipped()) {
        LONG seen = g_small_mb;
        void *q = mi_realloc(p, n ? n : 1);
        if (q || (grow_small_heap(seen) && (q = mi_realloc(p, n ? n : 1)))) return q;
        mi_full();  // mimalloc is full: p is still valid, move it to dlmalloc
    }
    void *q = DL_MALLOC(n);  // grew past the small limit (or mimalloc is full): move to dlmalloc
    if (!q) {                // the old block stays valid
        log_failure("realloc", n);
        return NULL;
    }
    size_t old = mi_usable_size(p);
    memcpy(q, p, old < n ? old : n);
    mi_free(p);
    return q;
}

struct heap_patch {
    const char *name;
    DWORD rva;
    unsigned char expect[12];
    int reloc;      // offset of an absolute address inside expect, or -1
    void *to;       // NULL: only checked (dlmalloc's trim keeps running as is)
    unsigned copy;  // whole instructions moved into the trampoline (same bytes as Attila's, where nothing jumps into them)
    void **orig;    // gets the trampoline: the original function
};

static struct heap_patch g_heap_patches[] = {
    {"dlmalloc",  0x12B640, {0x83,0xEC,0x08,0x83,0x3D,0x74,0x62,0x00,0x12,0x00,0x75,0x05}, 5,  (void *)hf_malloc, 10, (void **)&dl_malloc},
    {"dlfree",    0x12AFF0, {0x83,0xEC,0x1C,0x56,0x8B,0x74,0x24,0x24,0x85,0xF6,0x0F,0x84}, -1, (void *)hf_free, 8, (void **)&dl_free},
    {"dlrealloc", 0x12B900, {0x53,0x8B,0x5C,0x24,0x08,0x85,0xDB,0x75,0x0E,0xFF,0x74,0x24}, -1, (void *)hf_realloc, 5, (void **)&dl_realloc},
    {"dltrim",    0x12AFA0, {0x83,0x3D,0x74,0x62,0x00,0x12,0x00,0x75,0x05,0xE8,0x32,0xE8}, 2,  NULL, 0, NULL},
};
#define NHEAP (sizeof g_heap_patches / sizeof g_heap_patches[0])

#define MI_STR(x) MI_STR2(x)
#define MI_STR2(x) #x

// Returns the number of functions redirected: all three, or 0 if anything does not match.
int install_heapfix(HMODULE m, void (*log)(const char *fmt, ...)) {
    g_log = log;
    unsigned char *base = (unsigned char *)m;
    DWORD delta = (DWORD)(base - (unsigned char *)PREFERRED_BASE);
    for (size_t i = 0; i < NHEAP; i++) {
        unsigned char want[12];
        memcpy(want, g_heap_patches[i].expect, sizeof want);
        if (g_heap_patches[i].reloc >= 0) {
            DWORD v;
            memcpy(&v, want + g_heap_patches[i].reloc, 4);
            v += delta;
            memcpy(want + g_heap_patches[i].reloc, &v, 4);
        }
        if (memcmp(base + g_heap_patches[i].rva, want, sizeof want) != 0) {
            log("heap: %s at +0x%X does not match this game version - heap not replaced", g_heap_patches[i].name, g_heap_patches[i].rva);
            return 0;
        }
    }
    // Pointers dlmalloc already handed out could not be freed by the new heap.
    if (*(volatile DWORD *)(base + RVA_MPARAMS_MAGIC) != 0) {
        log("heap: dlmalloc was already in use when this loaded - heap not replaced");
        return 0;
    }
    unsigned char *tramp = VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tramp) {
        log("heap: VirtualAlloc for the trampolines failed (%lu) - heap not replaced", GetLastError());
        return 0;
    }
    for (size_t i = 0; i < NHEAP; i++) {  // original first instructions, then a jump to the rest
        if (!g_heap_patches[i].to) continue;
        unsigned char *t = tramp + 32 * i, *site = base + g_heap_patches[i].rva;
        memcpy(t, site, g_heap_patches[i].copy);
        t[g_heap_patches[i].copy] = 0xE9;
        DWORD rel = (DWORD)((site + g_heap_patches[i].copy) - (t + g_heap_patches[i].copy + 5));
        memcpy(t + g_heap_patches[i].copy + 1, &rel, 4);
        *g_heap_patches[i].orig = t;
    }
    FlushInstructionCache(GetCurrentProcess(), tramp, 4096);
    mi_option_set(mi_option_retry_on_oom, 0);  // a full cap must fail fast, not wait 400 ms
    mi_option_set(mi_option_max_errors, 0);
    mi_option_set(mi_option_max_warnings, 0);
    unsigned cap_mb = SMALL_HEAP_MB;
    while (cap_mb >= 256 && !reserve_small_heap((size_t)cap_mb << 20)) cap_mb -= 128;
    if (cap_mb >= 256) {
        g_small_mb = (LONG)cap_mb;
        mi_option_enable(mi_option_disallow_os_alloc);
    } else {
        cap_mb = 0;
        mi_option_set(mi_option_arena_reserve, 32 * 1024);  // KiB: no cap, but small arenas at least
    }
    for (size_t i = 0; i < NHEAP; i++) {
        if (!g_heap_patches[i].to) continue;
        unsigned char *site = base + g_heap_patches[i].rva;
        DWORD old;
        if (!VirtualProtect(site, 5, PAGE_EXECUTE_READWRITE, &old)) {
            // Only reachable for the first patch in practice; a partial install would mix heaps.
            log("heap: VirtualProtect failed at %s (%lu)%s", g_heap_patches[i].name, GetLastError(),
                i ? " - GAME STATE UNSAFE, remove dinput8.dll" : " - heap not replaced");
            return 0;
        }
        site[0] = 0xE9;
        DWORD rel = (DWORD)((unsigned char *)g_heap_patches[i].to - (site + 5));
        memcpy(site + 1, &rel, 4);
        VirtualProtect(site, 5, old, &old);
        FlushInstructionCache(GetCurrentProcess(), site, 5);
    }
    log("game heap replaced: mimalloc " MI_STR(MI_MALLOC_VERSION) " for blocks <= 16 KB (%u MB reserved, grows to 768 MB "
        "in 32 MB steps if needed; 0 = no cap), larger blocks stay on the game's heap", cap_mb);
#ifdef PROFILING
    HANDLE t = CreateThread(NULL, 0, memstats, NULL, 0, NULL);
    if (t) CloseHandle(t);
#endif
    return (int)NHEAP - 1;
}
