// dinput8.dll proxy for Total War Saga: THRONES OF BRITANNIA (empire.retail.dll 64A47C87).
// empire.retail.dll imports DINPUT8.dll, so this loads before the game's own startup code runs.
// It installs the fixes ported from the Attila mod whose code matches ToB (see AGENT_LOG.md) and forwards the real
// DirectInput exports to the system dinput8.dll. Log: tobfix.log next to this DLL.
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "version.h"
#include "logtrim.h"

typedef void (*log_fn)(const char *fmt, ...);
int install_heapfix(HMODULE m, log_fn log);                             // heapfix.c
int install_pool(HMODULE m, log_fn log);                                // pool.c
int install_sortfix(HMODULE m, log_fn log);                             // sortfix.c
int install_frame(HMODULE m, log_fn log);                               // frame.c
extern void (*g_frame_check)(void);                                     // frame.c
int install_campfix(HMODULE m, log_fn log);                             // campfix.c
void campfix_frame(void);                                               // campfix.c
int install_x87fix(HMODULE m, log_fn log);                              // x87fix.c
void x87fix_frame(void);                                                // x87fix.c
int install_memfast(HMODULE m, log_fn log);                             // memfast.c
int install_loopfetch(HMODULE m, log_fn log);                           // loopfetch.c
int install_fileio(HMODULE m, log_fn log);                              // fileio.c
int install_attrcache(HMODULE m, int verify, log_fn log);               // attrcache.c
int install_texfix(HMODULE m, int verify, log_fn log);                  // texfix.c
int install_uikeep(HMODULE m, log_fn log);                              // uikeep.c
void diag_startup(HMODULE empire, const wchar_t *log_path, log_fn log);  // diag.c
void update_check(HMODULE self, log_fn log);                            // update.c

static HMODULE g_self;
static wchar_t g_log_path[MAX_PATH];

// One line per game start that stands out in the log: version and date.
static void log_header(void) {
    trim_log(g_log_path);  // past 1 MB, the oldest sessions go (players must be able to post it)
    FILE *f = _wfopen(g_log_path, L"a");
    if (!f) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(f, "\n======== Thrones of Britannia Engine Performance Fix v%s - game started %04d-%02d-%02d %02d:%02d ========\n",
            MOD_VERSION_STR, t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute);
    fclose(f);
}

static void wlog(const char *fmt, ...) {
    FILE *f = _wfopen(g_log_path, L"a");
    if (!f) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(f, "[%02d:%02d:%02d.%03d] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list a;
    va_start(a, fmt);
    vfprintf(f, fmt, a);
    va_end(a);
    fputc('\n', f);
    fclose(f);
}

// ---- dinput8 forwarding (the real DLL is loaded on first use, never inside DllMain)
static HMODULE g_real;

static FARPROC real_proc(const char *name) {
    if (!g_real) {
        wchar_t path[MAX_PATH];
        UINT n = GetSystemDirectoryW(path, MAX_PATH);  // SysWOW64 for this 32-bit game
        if (n == 0 || n > MAX_PATH - 14) return NULL;
        wcscat(path, L"\\dinput8.dll");
        g_real = LoadLibraryW(path);
        if (!g_real) return NULL;
    }
    return GetProcAddress(g_real, name);
}

HRESULT WINAPI stub_DirectInput8Create(HINSTANCE h, DWORD v, REFIID r, LPVOID *o, LPUNKNOWN u) {
    typedef HRESULT(WINAPI * fn)(HINSTANCE, DWORD, REFIID, LPVOID *, LPUNKNOWN);
    fn f = (fn)real_proc("DirectInput8Create");
    return f ? f(h, v, r, o, u) : E_FAIL;
}
HRESULT WINAPI stub_DllCanUnloadNow(void) {
    typedef HRESULT(WINAPI * fn)(void);
    fn f = (fn)real_proc("DllCanUnloadNow");
    return f ? f() : S_FALSE;
}
HRESULT WINAPI stub_DllGetClassObject(REFCLSID c, REFIID r, LPVOID *o) {
    typedef HRESULT(WINAPI * fn)(REFCLSID, REFIID, LPVOID *);
    fn f = (fn)real_proc("DllGetClassObject");
    return f ? f(c, r, o) : E_FAIL;
}
HRESULT WINAPI stub_DllRegisterServer(void) {
    typedef HRESULT(WINAPI * fn)(void);
    fn f = (fn)real_proc("DllRegisterServer");
    return f ? f() : E_FAIL;
}
HRESULT WINAPI stub_DllUnregisterServer(void) {
    typedef HRESULT(WINAPI * fn)(void);
    fn f = (fn)real_proc("DllUnregisterServer");
    return f ? f() : E_FAIL;
}

// The plain-language summary after the install lines (those stay as the details for bug reports).
enum { FIX_SWITCHED_OFF = -1, FIX_INACTIVE = 0, FIX_ON = 1, FIX_PARTLY = 2 };
enum { S_HEAP, S_POOL, S_SORTS, S_X87, S_COPY, S_PREFETCH, S_FILES, S_MISSING, S_TEXTURES, S_MENUS, NFIX };
static const char *const g_fix_names[NFIX] = {
    "FPS: memory allocator (heap)", "FPS: small-object pools", "FPS: render-list sorts", "FPS: x87 rounding",
    "FPS: small memory copies", "FPS: render-loop prefetch", "Loading: file reading", "Loading: missing-file cache",
    "Loading: battle textures", "Menus: textures stay loaded (no stutter on reopening)"};

static void log_status(const int st[NFIX]) {
    int on = 0;
    for (int i = 0; i < NFIX; i++) on += st[i] == FIX_ON;
    if (on == NFIX)
        wlog("STATUS: all %d fixes are on", NFIX);
    else
        wlog("STATUS: %d of %d fixes are on", on, NFIX);
    for (int i = 0; i < NFIX; i++)
        wlog("  %-4s %s%s", st[i] == FIX_ON ? "on" : st[i] == FIX_PARTLY ? "part" : "OFF", g_fix_names[i],
             st[i] == FIX_SWITCHED_OFF ? " (switched off by its _off.txt file in the game folder)"
             : st[i] == FIX_PARTLY ? " (only part of it fits this game version)"
             : st[i] == FIX_INACTIVE ? " (does not fit this game version, so it stays off; details above)" : "");
}

static int g_campfix_on, g_x87fix_on;

// Per frame (frame.c): the rounding fixes' safety checks.
static void frame_checks(void) {
    if (g_campfix_on) campfix_frame();
    if (g_x87fix_on) x87fix_frame();
}

// Installs everything into the game DLL m (mapped and relocated, its own startup code not run yet).
static void install_all(HMODULE m) {
    int st[NFIX] = {0};
    diag_startup(m, g_log_path, wlog);  // version, process, game build, system, settings; crash records
    update_check(g_self, wlog);         // a newer Workshop copy replaces this DLL for the next launch
    // Off switches: empty files in the game folder.
    wchar_t f[MAX_PATH];
    wchar_t *name = wcsrchr(g_log_path, L'\\') - g_log_path + f + 1;
    wcscpy(f, g_log_path);
#define PRESENT(n) (wcscpy(name, n), GetFileAttributesW(f) != INVALID_FILE_ATTRIBUTES)
    // The heap must go in before the game's first allocation, which is still to come here. mimalloc frees a thread's
    // state when the thread ends, so thread notifications stay on while it is installed.
    if (PRESENT(L"heapfix_off.txt")) {
        wlog("heapfix_off.txt found - heap not replaced");
        st[S_HEAP] = FIX_SWITCHED_OFF;
    } else
        st[S_HEAP] = install_heapfix(m, wlog) > 0;
    if (st[S_HEAP] != FIX_ON) DisableThreadLibraryCalls(g_self);
    if (PRESENT(L"pool_off.txt")) {
        wlog("pool_off.txt found - pool lookup not patched");
        st[S_POOL] = FIX_SWITCHED_OFF;
    } else
        st[S_POOL] = install_pool(m, wlog) > 0;
    if (PRESENT(L"sortfix_off.txt")) {
        wlog("sortfix_off.txt found - render-list sorts not replaced");
        st[S_SORTS] = FIX_SWITCHED_OFF;
    } else {
        int sorts = install_sortfix(m, wlog);
        st[S_SORTS] = sorts == 2 ? FIX_ON : sorts ? FIX_PARTLY : FIX_INACTIVE;
    }
    // Rounding: the 4 campaign blocks first (the general x87 pattern matches them too), both checked every frame.
    int frame = install_frame(m, wlog), camp = 0, x87 = 0;
    if (!frame)
        wlog("x87 rounding fixes not installed (they need the frame hook)");
    else if (PRESENT(L"x87fix_off.txt")) {
        wlog("x87fix_off.txt found - x87 rounding fixes not installed");
        st[S_X87] = FIX_SWITCHED_OFF;
    } else {
        g_campfix_on = (camp = install_campfix(m, wlog)) > 0;
        g_x87fix_on = (x87 = install_x87fix(m, wlog)) > 0;
        g_frame_check = frame_checks;
        st[S_X87] = x87 > 0 && camp == 4 ? FIX_ON : x87 > 0 || camp > 0 ? FIX_PARTLY : FIX_INACTIVE;
    }
    if (PRESENT(L"memfast_off.txt")) {
        wlog("memfast_off.txt found - memcpy fast path not installed");
        st[S_COPY] = FIX_SWITCHED_OFF;
    } else {
        int n = install_memfast(m, wlog);
        st[S_COPY] = n == 2 ? FIX_ON : n ? FIX_PARTLY : FIX_INACTIVE;
    }
    if (PRESENT(L"loopfetch_off.txt")) {
        wlog("loopfetch_off.txt found - render loop prefetch not installed");
        st[S_PREFETCH] = FIX_SWITCHED_OFF;
    } else {
        int n = install_loopfetch(m, wlog);
        st[S_PREFETCH] = n == 3 ? FIX_ON : n ? FIX_PARTLY : FIX_INACTIVE;
    }
    // Loading: packs through the file cache and buffered save reads (fileio.c), then the missing-folder cache
    // (attrcache.c), whose open hook calls fileio's.
    if (PRESENT(L"fileio_off.txt")) {
        wlog("fileio_off.txt found - file I/O fix not installed");
        st[S_FILES] = FIX_SWITCHED_OFF;
    } else
        st[S_FILES] = install_fileio(m, wlog) > 0;
    if (PRESENT(L"attrcache_off.txt")) {
        wlog("attrcache_off.txt found - attribute cache not installed");
        st[S_MISSING] = FIX_SWITCHED_OFF;
    } else {
        int verify = 0;
#ifdef PROFILING
        verify = PRESENT(L"attrcache_verify.txt");  // answer from Windows, count where the cache would differ
#endif
        st[S_MISSING] = install_attrcache(m, verify, wlog) > 0;
    }
    if (PRESENT(L"texfix_off.txt")) {
        wlog("texfix_off.txt found - texture compression not replaced");
        st[S_TEXTURES] = FIX_SWITCHED_OFF;
    } else {
        int verify = 0;
#ifdef PROFILING
        verify = PRESENT(L"texfix_verify.txt");  // the game's compressor writes, ours is compared
#endif
        st[S_TEXTURES] = install_texfix(m, verify, wlog) > 0;
    }
    if (PRESENT(L"uikeep_off.txt")) {
        wlog("uikeep_off.txt found - menu textures freed when closed, as the game does");
        st[S_MENUS] = FIX_SWITCHED_OFF;
    } else
        st[S_MENUS] = install_uikeep(m, wlog) > 0;
    log_status(st);
}

// Something else in the process (an overlay, another hook DLL) can load dinput8.dll before the game loads
// empire.retail.dll; Windows then never runs this DllMain again. So wait for the game DLL and install when it is
// mapped, which is still before its own startup code runs (the same state as the normal path).
typedef struct { USHORT Length, MaximumLength; PWSTR Buffer; } NOTIF_STR;
typedef struct { ULONG Flags; const NOTIF_STR *FullDllName, *BaseDllName; PVOID DllBase; ULONG SizeOfImage; } NOTIF_DATA;
static PVOID g_notif_cookie;

static VOID CALLBACK on_dll_loaded(ULONG reason, const NOTIF_DATA *d, PVOID ctx) {
    (void)ctx;
    static const wchar_t want[] = L"empire.retail.dll";
    if (reason != 1 || !d || !d->BaseDllName || d->BaseDllName->Length != sizeof want - sizeof(wchar_t)) return;
    if (_wcsnicmp(d->BaseDllName->Buffer, want, d->BaseDllName->Length / sizeof(wchar_t)) != 0) return;
    wlog("empire.retail.dll is loading now - installing the fixes");
    install_all((HMODULE)d->DllBase);
}

static int wait_for_game(void) {
    typedef LONG(NTAPI * reg_fn)(ULONG, VOID(CALLBACK *)(ULONG, const NOTIF_DATA *, PVOID), PVOID, PVOID *);
    reg_fn reg = (reg_fn)(void *)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "LdrRegisterDllNotification");
    return reg && reg(0, on_dll_loaded, NULL, &g_notif_cookie) == 0;
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID r) {
    (void)r;
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    g_self = h;
    DWORD n = GetModuleFileNameW(h, g_log_path, MAX_PATH);
    wchar_t *sl = wcsrchr(g_log_path, L'\\');
    if (n == 0 || n >= MAX_PATH || !sl || sl - g_log_path > MAX_PATH - 20) {  // no log path: do nothing
        DisableThreadLibraryCalls(h);
        return TRUE;
    }
    wcscpy(sl + 1, L"tobfix.log");
    log_header();
    HMODULE m = GetModuleHandleW(L"empire.retail.dll");
    if (m)
        install_all(m);
    else if (wait_for_game())
        wlog("dinput8.dll was loaded before empire.retail.dll (by another program or DLL) - the fixes install when "
             "the game loads it");
    else {
        diag_startup(NULL, g_log_path, wlog);
        DisableThreadLibraryCalls(h);
    }
    return TRUE;
}
