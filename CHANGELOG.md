# Cemu Aroma changelog

This log tracks changes made in the fork since upstream Cemu commit `3b103161`. Add new entries under Unreleased as changes land. Record the title, renderer and CPU mode when they affect what was verified. Wii Street U Revival’s map rendering and Guide hit-testing changes belong in the Revival project.

## Unreleased

- Added an Aroma toggle and plugin folder shortcut in General Settings. When enabled, Cemu loads modules and discovers plugins from the emulated SD card at title startup.

### November 12, 2026 · Aroma plugin support

Fork commit `e5cd8c85`.

- Added repeatable `--aroma-plugin` to load PowerPC WPS images in launch order. The loader relocates sections, resolves imports, registers supported function replacements and runs startup and shutdown hooks in the guest. It reports unresolved imports before installing a plugin.
- Added `--aroma-link-modules` to load WUMS images from a directory. The linker resolves imports and exports, installs modules in dependency order, detects dependency cycles, and exposes installed modules to WPS and guest OSDynLoad. Added early and regular initialization, module information tables, runtime and application callbacks, and unload hooks.
- Added a dedicated thread-safe expanded heap for Aroma’s allocator imports. This fixed corrupted response bodies when a plugin grew a download buffer using WUT `realloc`. Serialized curl initialization so simultaneous workers can’t use it before the guest allocator callbacks are ready.
- Added the guest file callbacks, HTTPS transport and SD content redirection used by Revival and SD Cafiine. The read-only merge checks SD overrides and deletion markers, then falls back to stock files. Revival can mount its own nested request filesystem. Added per-plugin boolean settings reads and mapped-memory allocation compatibility for SD Cafiine.
- Added compatibility exports for the virtual display and OSScreen, WPAD and KPAD controller lifecycle, VPAD TV-overlay calls, selected dynamic-loader and kernel-copy functions, and queued title launches. Added AC close/finalize and paired guest DNS allocation with `freeaddrinfo` for YouTube.
- Added **File → Close game** for Aroma sessions. Title exit, title changes and window close dispatch plugin cleanup before scheduler teardown. Fixed an idle scheduler loop that could block title switches. Released Aroma executable memory and coreinit generated code on title teardown, and guarded recompiler entry against missing lookup blocks. Three consecutive multicore Wii Street U launches each loaded live map data and cleaned up on close.
- Fixed second-search lockups in Wii Street U by having the software keyboard report its closing state before it returns to idle. The integration can read the complete submitted text through a fresh file URL to avoid stale WebKit responses. London and Paris searches passed in the same Linux Vulkan session.
- Fixed Vulkan rectangle shader interface mismatches, minimum texture dimensions during uploads and barriers that needed to run outside a render pass. Also synchronized the recompiler’s data-cache flush queue.
- Fixed VPAD reads for an absent controller overwriting the caller’s previous sample. YouTube now retains the GamePad input when it polls a second channel. GiveMiiYouTube’s callback, guest search and changing video frames on both screens were verified in a Linux Vulkan interpreter session.

**Open compatibility issues.** The original Aroma settings menu now opens in the empty-plugin Wii Street U test described below. Native plugin loading through PluginBackend still needs work. WUPS settings registration and writes remain incomplete. YouTube has stalled at startup under the recompiler, and one desktop session reported frozen video with audible playback. Linux results are documented in the [Aroma implementation notes](README-AROMA.md) and [validation guide](docs/VALIDATION.md).

### November 8–12, 2026 · Python debugger

- Added `--python-debug` and a Python client with a JSON-lines mode for scripting. The client lists guest threads and modules, reads and writes memory and registers, controls execution, and supports reversible patches and software breakpoints. Fixed register packets, floating-point writes, fragmented packet handling and detach cleanup. (`67259210`)
- Fixed attach during title startup and reattach after a disconnect. Added live debugger checks and enabled TCP_NODELAY in the client to avoid delayed-ACK stalls. (`810f3e47`, `3dcae311`)
- Rejected oversized addresses before guest memory access. Serialized multicore breakpoint handling and checked repeated watchpoint delivery. Fixed cache invalidation across disjoint translated ranges after code writes. (`47eab6b8`, `4b96dc87`, `4fbef3d1`)
- Kept debugger requests synchronized when a breakpoint stop races a pause request. Enabled Linux process inspection when the debugger listener starts and exposed the checked paired-single PS1 register lane. (`d91aed41`, `7393b112`, `8e96cded`)
- Documented raw PC redirection limits, watchpoint constraints and the need to wait for patched code to execute before measuring results. Linux live checks used Mario Kart 8 US v81. See the [Python debugger guide](README-PYTHON.md). (`1ec2ee90`)

### November 13, 2026 · Aroma settings startup

- Added Cemu-backed mapped-memory exports for Aroma and placed FunctionPatcher’s writable trampoline storage in low guest memory. Wii Street U now reaches PluginBackend initialization. The test then faulted in a generated trampoline, before the settings shortcut could be tested. PluginBackend also reported the optional IOPShell module absent.
- Made the Aroma-mode `OSGetUPID` export start with the same PowerPC instruction pattern as retail coreinit. FunctionPatcher copies those two instructions into its process filters. The original PluginBackend now completes startup with an empty plugin directory. Verified that D-pad Down + Minus + L opens the original settings menu in Wii Street U. The guest input hook received mask `0x124` and PluginBackend v0.4.1 drew its menu in a hidden, muted Vulkan run. The menu reported no plugins available. That initial SD Cafiine crash was fixed in the follow-up below. [GamePad capture](docs/images/aroma-config-wii-street-u.png)

### Native plugin loading and SD Cafiine

- Enabled content redirection and curl compatibility for Aroma module launches. Fixed x64 recompiler dispatch to Aroma code outside its normal executable range by falling back to the interpreter. The real backend loaded SD Cafiine and Wii Street U Revival, SD Cafiine mounted the test modpack, and the original Aroma menu displayed Revival's settings.
- Fixed clipped SD Cafiine text by honoring the OSScreen text origin and clipping glyphs to the framebuffer. Verified Minus changes and persists the single-modpack autostart option in the selector. Added GamePad flip pacing for the automatic preparation countdown. Opening the selector with Minus during that countdown remains unverified.

### Inkay compatibility

- Added Aroma-mode support for the legacy Cafe OS current-thread load used by Inkay v3.0.0's WMS module and WPS plugin. The recompiler hands that instruction to the interpreter so it reads the current emulated thread on the executing core.
- Verified that both Inkay components get past their thread initialization and Mario Kart 8 US v81 starts with audio. Cemu was set to Nintendo. Its account request still reached Nintendo and returned error 106. Inkay's Mocha initialization reported that `/dev/iosuhax` is missing, and its game patches reported that `Turbo.rpx` could not be found through module enumeration. Pretendo login remains unverified.
- Preserved the main executable's original filename in guest module notifications. Inkay v3.0.0 could then find `Turbo.rpx` in Mario Kart 8 US v81. This verifies the executable lookup independently of Inkay's IOSU network redirection.
