# Compatibility and validation

## Wii Street U Revival on Linux

The November 12, 2026 test used the real SD Cafiine and Revival WPS files, an isolated profile, an emulated SD card, and the Python debugger. A hidden Xwayland display presented Vulkan frames through RADV on an AMD Radeon RX 6700 XT.

1. Start Wii Street U and confirm that SD Cafiine mounts the prepared content and Revival mounts its request filesystem.
2. Open **Search for a Place**, type `london england`, and submit. Confirm the London panorama loads.
3. Open search again, type `paris france`, and submit. Confirm a distinct request and a Paris panorama.
4. Close the title and preserve the console output, Cemu log, exit status, and any core dump report.

The run recorded two successful search requests, zero Vulkan validation errors, and exit status 0. The [London](images/london-amd.png) and [Paris](images/paris-amd.png) screenshots came from that session. An earlier software-Vulkan run also completed both searches after the renderer changes.

## Fixes exercised

- The keyboard reports a closing state before returning to idle, so the game can reopen it after a submission.
- The Revival emulator integration can read the complete submitted text via a fresh file URL, avoiding stale WebKit responses.
- Vulkan rectangle shaders declare matching per-vertex interfaces. Vulkan textures use the same minimum dimensions as uploads. Texture synchronization that requires vertex or geometry stages ends the render pass before issuing its barrier.

A prior desktop process exited with status 137 and no core dump. Its cause remains unknown. The runs above cover the search and panorama transition on Linux. Other titles, drivers, and operating systems need their own validation.

## Reporting a compatibility issue

Record the fork commit, Cemu renderer and CPU mode, title region and version, plugin versions or SHA-256 hashes, and the exact steps that trigger the issue. Include the relevant startup and failure lines from the Cemu and console logs. Strip credentials, account data, private locations, and unrelated filesystem paths before posting logs.

For a startup stall, capture the active guest thread list and the current PC and LR when practical. For a graphics failure, include whether validation was enabled and the first validation message before the failure.

See [Aroma implementation notes](../README-AROMA.md) and the [Python debugging guide](../README-PYTHON.md).
