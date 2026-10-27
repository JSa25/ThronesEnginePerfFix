# Thrones of Britannia Engine Performance Fix

Source code for the Total War Saga: THRONES OF BRITANNIA engine performance mod: a `dinput8.dll` that the game loads
at startup and that patches the engine in memory. No game files are changed. Thrones runs on Attila's engine, so this is
a port of the [Attila Engine Performance Fix](https://github.com/JSa25/AttilaCPUPerfFix): every fix whose code is the
same in Thrones was carried over, with its test.

Download the ready-made DLL:
- Steam Workshop: https://steamcommunity.com/sharedfiles/filedetails/?id=3828420829 (after subscribing, run `install.bat` in the mod's folder)

Created with the help of AI (Claude by Anthropic). Tested in game by JGdaCaesar.

## What each file does

| File | Fix |
|---|---|
| `proxy.c` | Entry point (`DllMain`, or a DLL load notification when something loaded `dinput8.dll` before the game DLL), dinput8 forwarding, `tobfix.log` with the STATUS block, the off switches |
| `heapfix.c` | The main fix: replaces the game's single-lock dlmalloc heap with mimalloc for blocks up to 16 KB (its own 512-768 MB reservation, grown in 32 MB best-fit pieces); larger blocks stay on the game's dlmalloc |
| `pool.c` | Faster free lookup for the engine's small-object pools |
| `sortfix.c` | The two render-list sorts (by +0x6C/+0x68 and +0x4C/+0x48): key extraction plus radix sort, same order as the game's sorts |
| `campfix.c` | Campaign map: four x87 rounding-mode blocks (fog of war, terrain height, curve table) |
| `x87fix.c` | The same x87 rounding idiom at the ~1,260 other sites in the engine (`fisttp` / SSE4.1 `roundss`) |
| `frame.c` | Hook on the tile-feedback scan, which runs once per frame, for the rounding-mode safety checks of `campfix.c` and `x87fix.c` |
| `memfast.c` | Fast path for the engine's two memcpy/memmove copies on copies of 64 bytes or less |
| `loopfetch.c` | Three battle render loops prefetch the field the loop reads next, four items ahead (prefetch only, never a load through an item pointer) |
| `fileio.c` | Loading: `.pack` files opened without `FILE_FLAG_NO_BUFFERING`; small reads of read-only files in the game's user folder (the unpacked save) served from a 64 KB buffer, file position kept exactly |
| `attrcache.c` | Loading and menus: cache of missing loose-file folders behind the game's `GetFileAttributesW` and read-only `CreateFileW` calls. Every folder above a cached name is watched; any change clears the cache. Answers and last-error values are the same as Windows' |
| `texfix.c` | Battle loading: the DXT1 compressor for unit imposters redone with SSE2, byte-identical output |
| `uikeep.c` | Menus: the deletion step of the UI picture queue keeps a picture loaded instead of deleting it and remembers its name, up to 1,024 |
| `update.c` | Self-update from the Steam Workshop copy (compares a version marker in the file; never downgrades, no network) |
| `diag.c` | Diagnostics in `tobfix.log`: mod version, game build and whether it is supported, system, settings, other hook DLLs, and crash records with a plain line saying whose code it was (a vectored exception handler that only logs) |
| `logtrim.c` | Keeps `tobfix.log` under about 1 MB |
| `builds.h`, `version.h`, `dinput8.def` | Known game builds, the mod version, the exports a `dinput8.dll` must have |

Every patch first compares the game's bytes at its address with what it expects, and skips itself (and says so in
`tobfix.log`) if they differ. So on another game version it does nothing.

## Building

You need:
- [llvm-mingw](https://github.com/mstorsjo/llvm-mingw) (the release used: 20261006, UCRT, x86_64 host), with its `bin` folder on `PATH`
- the [mimalloc](https://github.com/microsoft/mimalloc) **v3.5.3** source, in a folder named `mimalloc` next to `build.bat` (or set `MIMALLOC` to its path)

Then run:

```
build.bat
```

The DLL lands in `build\dinput8.dll`. The build is reproducible: with the same llvm-mingw and mimalloc versions you
should get exactly the same file as the released one. Compare the SHA-256
(`certutil -hashfile build\dinput8.dll SHA256`) with [dinput8.dll.sha256](dinput8.dll.sha256).

### Automated builds

Every commit is also built on GitHub's own machines by [.github/workflows/build.yml](.github/workflows/build.yml),
with the compiler and mimalloc downloaded from their official releases (checksums pinned). Open the latest run under
**Actions**: its summary shows the SHA-256 of the DLL, and the DLL itself can be downloaded there. Builds also carry a
signed build attestation (`gh attestation verify dinput8.dll -R JSa25/ThronesEnginePerfFix`).

## Tests

The tests run the game's own code next to the replacements on millions of inputs and check the results are
identical. `lateload_test` loads the built DLL before the game DLL, as an overlay can, and checks every fix still
installs from Windows' load notification; `diag_test` checks the crash records on deliberate faults; `fileio_test` and
`attrcache_test` compare against plain Windows calls on a temporary folder tree while it changes; `uikeep_test` runs
the menu fix against a stand-in for the game's picture table. They read your copy of the game's `empire.retail.dll`
(it is not included here):

```
build.bat test "C:\...\steamapps\common\Total War Saga Thrones of Britannia\empire.retail.dll"
```

## License

See [LICENSE](LICENSE). mimalloc is (c) Microsoft Corporation and Daan Leijen, MIT license
([third_party/mimalloc-LICENSE.txt](third_party/mimalloc-LICENSE.txt)).
