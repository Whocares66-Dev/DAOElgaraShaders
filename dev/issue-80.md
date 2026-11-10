# Steam overlay stack overflow (#80)

Initially investigated 2026-11-10 at `46b34a5` (Elgara 0.5.1, DAOLib submodule `2716248`). [Issue 80](https://github.com/Whocares66-Dev/DAOElgaraShaders/issues/80) and its attached `Elgara-Steam-crash-logs.zip` contain three reports. A standalone x86 Direct3D 9 probe reproduces the presentation recursion with the installed Steam overlay; no game, DAOLib, shadows, shader changes or MSAA is needed. The subsequent implementation and game tests on `gh-80-steam-overlay` are recorded below.

## What the reports establish

All three exceptions are `EXCEPTION_STACK_OVERFLOW` (`0xc00000fd`). Their address spaces hold 1,361, 1,465 and 1,486 MiB of 4,096 MiB, each with a 2,029.1 MiB free block. This differs from [issue 73](https://github.com/Whocares66-Dev/DAOElgaraShaders/issues/73), where RTSS removed the inline Present hooks and the shadow draw list grew until allocation failed.

At 10:58, the stack repeatedly returns through Steam's `gameoverlayrenderer.dll+79AB4` and Elgara's `ChainPresent` (`+AB03`). The final nested call happens to run out of stack in `SetReceiversFor`, reached by `ChainPresent → ShowScene → BeginScene → HandFills`; that last function is not evidence of a separate shadow fault. The window regains focus at 10:58:19.721, and the crash is at 10:58:19. The supplied report has no hook-pointer snapshot, so exactly what the focus change did is unknown.

At 11:37 and 11:49, the same repeated Steam return address is below D3D9/NVIDIA work. The README says these were startup crashes with the per-game overlay option off, then with built-in MSAA disabled. Steam's overlay DLL remains loaded and on the call stack in both reports. Two normal Steam launches with Elgara paused and DAOLib active reportedly worked. The instruction to disable the overlay in Steam's per-game options therefore did not remove the hooking path in these particular runs.

## Reproduced mechanism

`Graphics.cpp` installs `g_chainPresent` by changing slot 3 of the swap chain's shared vtable. `dao::VtableHook::Original()` retains the address previously in that slot; it does not make an independent trampoline. With Steam already loaded, that saved address is D3D9's Present entry, whose code Steam has patched to jump to its overlay handler.

Steam also intercepts `IDirect3DDevice9::GetSwapChain`. On a subsequent successful query, it reads the returned chain's current slot 3 and checks whether its own handler has hooked that address. Because the slot now names Elgara's callback, Steam hooks the callback's machine code as another Present target. It replaces its shared saved-next pointer with a trampoline into that callback. The old D3D9 Present entry still jumps to the same Steam handler.

The next Present then loops:

```text
game → Elgara ChainPresent
           → saved D3D9 Present entry
               → Steam swap-chain Present handler
                   → Steam's new trampoline into Elgara ChainPresent
                       → saved D3D9 Present entry → Steam → Elgara → …
```

The installed overlay has PE timestamp `0x6ad92a09`, image size `0x18c000` (the size in the reporter's module lists), SHA-256 `1afe9b93e0f36cbee628346e03ccae0c3f75567fdd2dd99c7b326b7f01eec841`. In this binary, `+78F20` is the GetSwapChain interceptor: after calling its saved getter, it reads the returned object's vtable and passes slot 3 to its hook installer at `+78F70`. The install uses the handler at `+79900` and saved-next storage at `+143634`. The Present handler reads that storage at `+79A9F` and calls it at `+79AB2`, producing the exact repeated return address `+79AB4` in all three reports. Matching size and offsets support the comparison; the attachment does not supply a binary hash to establish byte-for-byte identity.

A second GetSwapChain alone is sufficient in the probe. A second device is unnecessary. After that query, the callback's original `55 8b ec …` bytes become an `e9 …` jump placed by Steam, and the very first Present enters the callback nine times before the probe's depth limit returns `E_FAIL`. Without the diagnostic limit this call cycle is unbounded. The diagnostic guard is not a proposed fix: returning early would not restore a real Present.

This reproduces a mechanism that explains the reporter's stack. It does not establish which caller queried the swap chain, or whether an equivalent Steam rehook path ran, in the reporter's process. The subsequent local game reproduction below matches the stack; the reporter's exact triggering caller remains unknown.

## Repeatable probe

[`repro-steam-present.cpp`](repro-steam-present.cpp) changes only its own process. Steam must already run, its 32-bit overlay must be at `C:\Program Files (x86)\Steam\GameOverlayRenderer.dll` (adjust the source if installed elsewhere), and GPU access is needed. It opens no visible window, uses no MSAA, loads no game files and caps recursive callback entry at nine calls. The vtable write is the same shared-slot replacement as DAOLib's helper. The test intentionally leaves its COM objects and hook until process exit.

From an **x86 Native Tools Command Prompt**, at the repository root:

```bat
if not exist build\investigation-80 mkdir build\investigation-80
cl /nologo /EHsc /Od /Zi dev\repro-steam-present.cpp /Fobuild\investigation-80\probe.obj /Fdbuild\investigation-80\compile.pdb /link /INCREMENTAL:NO /OUT:build\investigation-80\probe.exe d3d9.lib user32.lib
build\investigation-80\probe.exe hook requery
build\investigation-80\probe.exe steam requery
build\investigation-80\probe.exe steam hook
build\investigation-80\probe.exe steam hook requery
```

| Steam loaded | Vtable hook | Second GetSwapChain | Observed result |
| --- | --- | --- | --- |
| No | Yes | Yes | 120 frames, 120 callback calls |
| Yes | No | Yes | 120 frames, no callback calls |
| Yes | Yes | No | 120 frames, 120 callback calls |
| Yes | Yes | Yes | Frame 0 recurses to the depth limit; exit code 2 |

Local downloaded logs, captured probe output and subsequent game-test artifacts are under `build/investigation-80/` (ignored by Git).

## Consequences for a fix

The 0.5.1 vtable change protects against RTSS erasing later inline hooks, but it exposes the replacement callback when Steam discovers Present through GetSwapChain. Fixing this needs a presentation-hook design that keeps Steam's discovery and saved-next chain consistent while retaining issue 73's protection. Merely limiting recursion or disabling Improved lighting does not repair that chain; blindly restoring inline Present hooks would reintroduce the RTSS failure.

A cleaner first approach than a swap-chain COM wrapper is available in the existing engine address map: move presentation interception to `D3DDriver::D3DGraphicsDriver::Present` (`kD3DGraphicsDriver_Present`, ID 23543) and `D3DDriver::D3DSwapChain::Present` (`kD3DSwapChain_Present`, ID 24028). These short engine functions call the real COM methods and return whether the device is not lost. The address library maps both across GOG, Steam and EA; disassembly of GOG and the saved Steam/EA analysis images confirms the calls. DAOLib's `perf/fastload/LoadingScreen.cpp` already detours both for loading-screen presentation throttling.

The preferred design is a shared render-thread presentation boundary in DAOLib, used by Elgara and DAOLib's render watcher, with fast-loading's skipped presents and per-frame cleanup handled deliberately. Then remove the COM Present vtable replacements from those consumers; the other D3D method hooks need not be replaced wholesale. Steam continues to discover the real D3D9 Present methods, and RTSS restoring D3D9's code cannot erase hooks in the engine's code. This avoids COM wrapper identity, lifetime and getter interception machinery. The existing `dao::OnFrame` callback runs on the update thread and is not a substitute for this render-thread boundary.

The initial recommendation was based on source and static disassembly. The implementation and tests below now exercise it in the game. The COM wrapper remains an alternative if further testing uncovers presentation paths outside these two engine methods.

Before releasing a fix, finish the remaining checks below, including native Steam launch and focus loss/regain. The investigation was [posted on issue 80](https://github.com/Whocares66-Dev/DAOElgaraShaders/issues/80#issuecomment-6764963960) at the user's request.

## Branch implementation and game tests, 2026-11-10

Both this repository and its DAOLib submodule have a local `gh-80-steam-overlay` branch. DAOLib adds `dao::render::OnFrameEnd` in `RenderFrame.h/.cpp`: each static-library copy detours the two mapped engine functions, dispatches guarded callbacks with a borrowed device and which path ended, then calls through the existing detour chain. The loader and its fast-loading detours are unchanged. A rendered frame is ended even if the loader subsequently skips displaying it; that deliberately keeps draw cleanup from spanning skipped frames. Each consuming DLL has its own callbacks, as with `dao::OnFrame`; there is no new loader export or plugin-ABI requirement.

Elgara uses that callback to compose its scene, finish GPU timing and clear its per-frame shadow draws. DAOLib's render watcher uses the same facility to finish captures. Both consumers remove their COM Present vtable replacements. Other D3D method hooks remain as vtable hooks. The scene remains 16-bit float. No shader, material or lighting formula was changed.

The test was GOG's game in the existing `elgara-agent` instance, owned as `elgara-gh80`, with isolated staged plugins; nothing was deployed into the game installation. A diagnostic plugin loads the installed 32-bit Steam overlay before Elgara/Eluvian and queries GetSwapChain at rendered frame 30, then every 120 frames. It logs which module owns slot 3 and Steam's saved-next pointer. Loading the overlay after the graphics plugins did not activate the relevant interception and was discarded as a crash control. Eluvian's current source needs a newer movement API than this DAOLib release, so a read-only source archive of its `e0d0534` revision was built against the modified library, entirely under this repository's ignored build directory. The Eluvian checkout was unchanged.

| Check | Result |
| --- | --- |
| Unmodified 0.5.1 + early-loaded Steam + original Eluvian | Crashed during startup before the diagnostic's first injected query. `baseline-reproduced-crash.log`: `EXCEPTION_STACK_OVERFLOW`, 1,364 MiB address space used, repeated Steam `+79AB4`, matching the supplied reports. |
| Patched Elgara and rebuilt Eluvian + early-loaded Steam | Loaded the West Brecilian Forest; at least 16,800 rendered frames and 141 injected GetSwapChain queries, without a crash. Slot 3 stayed in `d3d9.dll`, and Steam's saved-next pointer stayed constant. Both engine paths ran (354 device, 16,446 swap-chain at the 16,800-frame sample). |
| MSAA/device reset | Through the Video screen: 8x → Off → 8x. The scene was recreated at 0 and 8 samples. Continued rendering and subsequent save loads worked. |
| Loading and several areas | Forest (`Lyna` 471), Orzammar Commons (347), Arl of Denerim's dungeon (35), then back to the forest. Window screenshots inspected, with no black/stale scene. |
| Render capture under Steam | Forest: 1,243 draws, 255 textures, 69 materials. After the dungeon load: 508 draws, 188 textures, 67 materials. Both captures completed through the new frame boundary. |
| RTSS running, then shut down during gameplay | Verified both D3D9 Present code entries initially jumped into `RTSSHooks.dll`. After ending only the RTSS process started for the test, both entries reverted to `8b ff 55 8b ec`. Render callbacks and captures continued (1,244 draws in the last capture), and private memory settled around 1,123–1,136 MiB. The run reached 4,200 logged frames. |
| Visual comparison | Same forest save, 1680×1050, 8x MSAA, cycle disabled through the Video screen. Original/no-overlay versus patched/Steam: hero crop mean RGB 19.278/16.878/8.033 → 19.365/17.001/8.240; portrait 86.335/59.416/45.433 → 86.543/59.525/45.532. Whole-frame means differed by at most 0.304/255. These are separate launches with different idle poses/particles, not a pixel-identical comparison. |
| Build and automated tests | Release build passed; all 43 Elgara tests passed. The modified library was also compiled into the test Eluvian. Formatting and clang-tidy passed for Graphics.cpp, Render.cpp and RenderFrame.cpp with the project's x86 target arguments. |

The Steam reproduction and RTSS regression used the actual installed overlay DLLs; the game itself was GOG's build. This does not establish native Steam-client launch behavior, EA runtime coverage, fullscreen, Alt-Tab/device loss, or whether the visible Steam panel draws. The desktop input helper was unavailable, so focus and panel checks could not be completed. Window capture became black during the RTSS run; a game-source screenshot after detachment confirmed that the rendered scene itself was intact. Both the owned game and the RTSS/helper processes started for the test were closed afterward. The instance's original 8x MSAA setting was restored through the Video screen.

Other plugins that use DAOLib's old render watcher must also be rebuilt to remove their own COM Present hooks: a new Elgara DLL cannot remove hooks compiled into another plugin. No loader update is required by this API; the existing 0.10.0 loader was used throughout these tests.

## Follow-up: installed Steam executable, 2026-11-10

The installed Steam copy was also tested, at `E:/SteamLibrary/steamapps/common/Dragon Age Ultimate Edition/bin_ship/DAOrigins.exe`. Its SHA-256 is `400c2c9e97bb1a534121553be66f202429ef6f7320c93c86f9b7a491864647bd`, the stock wrapped `steam-1.05` build in the address catalog. The executable and installed files were not modified; the same isolated profile and staged patched plugins were used, with the diagnostic's manual overlay loading disabled. The installed Steam loader identifies itself as 0.3.0.

A direct start without Steam's app-ID environment exited in the wrapper and opened `DAOriginsLauncher.exe` through Steam. Retrying the installed executable with `SteamAppId=47810` and `SteamGameId=47810` allowed it to start and load the forest. It reached at least 7,080 logged rendered frames and 60 diagnostic GetSwapChain queries, loaded Orzammar Commons, and completed frame captures (forest 1,245 draws; Commons 1,005 draws). Both the owned game and the launcher created by the first attempt were closed.

Module inspection confirmed that this direct start did **not** load `GameOverlayRenderer.dll`; both D3D9 Present entries remained unpatched. This therefore adds actual Steam-build/loader smoke coverage, not a normal Steam-client/overlay test. The injected-overlay GOG run above remains the verified overlay regression. A normal client launch goes through the graphical launcher; no supported automatic-play switch was established from its configuration, and native desktop input remains unavailable.

## Plugin-interface and combined-plugin compatibility, 2026-11-10

There is no plugin-ABI change. Against DAOLib `2716248`, `src/dao/Plugin.h` is byte-for-byte unchanged (Git blob `e09472e13acb18db47b1ffa2548cc50ce5ffd493`): API version 2, the same LoaderInfo layout and DAOLib_Load entry point. The loader sources, Hook.h and the game-thread Frame.h/Frame.cpp are unchanged too. OnFrameEnd is an additive C++ static-library API, not a new loader export or a change to an existing cross-DLL table. Eluvian's plugin interface stays at version 3.

An existing turn-based DLL was copied unchanged from `C:/project/DAOTurnBased/build/release/daolib/plugins/turnbased/`: source and staged SHA-256 both `135a42d6a9a0c76f8016453ff54bd6afa58840597a597af565fcc55b346075c2`. It identifies itself as turnbased 0.3.0 on DAOLib 0.10.0. It was run beside patched Elgara, rebuilt Eluvian and the diagnostic plugin, with the real Steam overlay loaded by the diagnostic. No turn-based source or binary was rebuilt or changed.

- Stock Steam executable: all four DLLs loaded; turn-based registered all seven Eluvian tools and 27 event descriptions with zero refusals. A staged dragonling fight exercised Delay, three End Turn calls, a player attack command, rendered enemy portraits and fight-end cleanup. The turn bar, portraits and Elgara-rendered scene were inspected together. Eluvian completed a 1,568-draw combat capture. Both plugins' options appeared in their normal Game and Video tabs; the screen was cancelled without changing values.
- A subsequent Orzammar load on the stock 2-GiB Steam executable logged two light-probe `bad allocation` warnings and did not produce a completion event before the test was closed. This is consistent with turn-based's documented requirement for an LAA Steam/EA executable; it is not counted as a successful load test.
- The same plugin set on the 4-GiB GOG executable loaded Orzammar, returned to the forest, repeated the staged combat test, and then loaded Orzammar again successfully. Turn-based returned to idle after combat and after loading. Frame capture worked during combat (1,354 draws) and after the final load (2,201 draws); the GOG run's Elgara and turn-based logs had no warnings/errors. The stock executable was not patched for these tests.

Turn-based does not use the render watcher and needs no rebuild for this fix. Existing DLLs that install the old render watcher's COM Present hooks still need rebuilding to receive the hook fix, even though their loader ABI is compatible. Raw contract hashes, binary exports, tool responses, screenshots and logs are in `build/investigation-80/compatibility-*`. The tests used the isolated profile, wrote no game save, and closed their owned game processes afterward.

## Merge review, 2026-11-10

The DAOLib change was rebased onto current master (`7c258753`) before the merge checks. Review found that copying the vector of `std::function` callbacks on every frame both allocated in the render loop and discarded mutable callback state. Registration now publishes an immutable list of shared callable objects; dispatch retains the snapshot and calls the same objects, without allocating or copying their state. Empty callbacks are rejected, and each callback still runs through the existing exception guard so another callback and the original engine function can continue after one throws.

The reviewed source passed DAOLib's debug build/tests/format/lint, ASan tests (705 cases), MSVC static analysis and core-x64 tests (695 cases). Optional game-file cases without fixtures were skipped. The original Plugin.h hash above and the game-thread Frame.h/Frame.cpp remain unchanged after the rebase.

A fresh isolated GOG 4-GiB run used rebuilt Elgara and Eluvian, the unchanged turn-based DLL and the manually loaded installed Steam overlay. A diagnostic compiled the actual reviewed RenderFrame.cpp: empty registration was rejected, mutable counters before and after a throwing callback both reached 3,000, and the intentional exception occurred exactly once. At that sample the probe counted 214 device and 2,786 swap-chain frames, with 26 GetSwapChain queries; the COM slot remained in d3d9.dll and Steam's saved-next pointer stayed constant. The combined combat test passed again (Delay, three End Turn calls, enemy portrait, attack, cleanup and a 1,618-draw capture), then Orzammar loaded successfully and completed a 996-draw capture. Turn-based returned to idle. Both screenshots were inspected, the Elgara and turn-based logs contained no warnings/errors, and the owned game was closed. Logs and captures are under `build/investigation-80/review-*`.

Elgara's final `release -Test -Format -Lint -Jobs 3` gate passed against this source: the DLL, shaders and night-lighting assets built, all 43 tests passed, and clang-tidy, ruff and PSScriptAnalyzer reported no findings. The merge-gate logs are `build/investigation-80/merge-*.txt`.

DAOLib `v0.11.0` shipped before the fix was pushed. The final dependency pin carries the same reviewed patch on top of that tag. DAOLib's address-ID check passed, and both presentation IDs resolve to the same addresses for GOG, Steam and EA in the release's address library.
