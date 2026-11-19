# Aroma plugin execution in Cemu

## Enable Aroma from the GUI

With the title closed, open **Options → General settings → General** and enable
**Aroma plugins**. Cemu loads `.wms` files from
`sdcard/wiiu/environments/aroma/modules/` at title startup. Put `.wps` files
in the neighboring `plugins/` folder. **Open plugins folder** takes you
there. The module set must include PluginBackend for SD plugin discovery.
Command-line module and plugin options remain available for debugging.

## Run Wii Street U Revival

Use a Wii Street U title with its Revival content prepared, and pass the actual
Aroma plugin file to this fork. The PC bridge environment variables are unset
in this example. Add `--python-debug` for guest breakpoints.

```sh
env -u CEMU_WII_STREET_U_MAPS -u CEMU_WII_STREET_U_TRACE -u CEMU_WII_STREET_U_AUTO_ENTER \
    ./bin/Cemu_relwithdebinfo --forward-console-logging \
    --aroma-plugin /path/to/WiiStreetU-Direct.wps -g /path/to/WiiULauncher.rpx
```

A startup check should show the plugin's mapped text address, its application
startup hook, the `wsudata:/` mount, and both service threads. Compare the
SHA-256 of the console and emulator plugin files before attributing differences
to the emulator. The version printed by the startup message alone is
insufficient to establish binary identity.

## Wii U observations

A TCP syslog capture on November 12, 2026 followed `aroma-debug` from system
boot to the Wii U Menu and into Wii Street U (title `0005000010132000`).
The following times are offsets in that boot log.

| Time | Observation |
| --- | --- |
| 00:00:16–17 | The WUPS backend scanned the debug environment's plugin folder, skipped files that failed its filter, and reported Wii Street U HTTPS Probe as active. |
| 00:00:22 | The Wii U Menu started under Aroma. |
| 00:00:37 | MCP prepared the Wii Street U title and mapped its original content. |
| 00:00:42 | SD Cafiine redirected title content. The Revival startup hook separately registered the virtual `wsudata:/` directory and started the Street View and map workers. |
| 00:00:44 | Revival intercepted SphereView's export lookup and applied its sprite and camera patches before the extension entry point executed. |
| 00:00:53 onward | Road tiles and panorama images were served. A later provider request reported curl timeout 28 after an HTTP 200 status. Requests continued afterward. |

This capture establishes the startup order and successful requests. The
plugin logged both road and coverage requests under `route=map`, so it does
not identify which responses contained Guide data. This capture ends without
a title exit. A read-only FTP copy of the active
Aroma debug environment's on-disk `WiiStreetU-Direct.wps` has SHA-256
`0d62a97f5c83f18ca12aaa6b4407a17e43cd0a86b07813d44e2c16d7c21b1488`.
It differs from both earlier emulator test binaries, even though they print
the same startup version.

Before the reported freeze, the console logged 49 successful map requests
out of 50 and all 16 logged panorama tile requests succeeded. Median
panorama queue time was 2,674 ms, with a 5,304 ms maximum. Median
fetch time was 1,171.5 ms. The level-2 panorama reported eight ready
tiles at 00:01:43.306. The map request that timed out after HTTP 200
was followed by successful traffic. The final successful map response
was logged at 00:02:18.348, followed by road-grid and road-tile events.
The capture then jumps to a new boot banner without a completion
record for the final events, a fault report, or an exit trace. The
queue measurements explain some latency before the freeze. Its cause
remains unknown.

The two final `road-tile` events both name tile 18/130982/87178.
In the recovered Revival scripts, that event is emitted after assigning
a successful image to the tile element. The request layer can deliver
that image from its cache or share one pending fetch among multiple
listeners. Consequently the two events establish repeated image
assignment, with no evidence of two upstream downloads or a completed
paint. The request trace does not record the map cache key, so it
cannot correlate these assignments to a particular provider response.

## Upstream implementation

The following is from WiiUPluginLoaderBackend revision
`e3e67f8a8ebee77b351a43ff79561ecb6955dea6`.

- **Discovery.** The backend obtains the active Aroma environment path and
  scans its `plugins` directory. It skips directories, names beginning with
  a dot or underscore, and files without the `.wps` extension.
  [Discovery source](https://github.com/wiiu-env/WiiUPluginLoaderBackend/blob/e3e67f8a8ebee77b351a43ff79561ecb6955dea6/source/utils/utils.cpp)
- **Loading and linking.** The backend allocates text, data and branch-stub
  space, loads and relocates sections, then reads the hook and replacement
  tables. It resolves external symbols through `OSDynLoad_Acquire` and
  `OSDynLoad_FindExport`. The replacement table supports plugin callbacks
  that call the original function.
  [Section linker](https://github.com/wiiu-env/WiiUPluginLoaderBackend/blob/e3e67f8a8ebee77b351a43ff79561ecb6955dea6/source/plugin/PluginLinkInformationFactory.cpp)
  [Import resolution and patching](https://github.com/wiiu-env/WiiUPluginLoaderBackend/blob/e3e67f8a8ebee77b351a43ff79561ecb6955dea6/source/PluginManagement.cpp)
- **Initialization and execution.** The backend initializes per-thread C
  runtime state, WUT runtime hooks, devoptab and sockets, and the wrapper.
  Storage and settings APIs are prepared before plugin initialization.
  Application-start hooks then run in the title's guest context.
  [Lifecycle dispatcher](https://github.com/wiiu-env/WiiUPluginLoaderBackend/blob/e3e67f8a8ebee77b351a43ff79561ecb6955dea6/source/main.cpp)
- **Shutdown.** Aroma has separate application-exit-request,
  application-end, and plugin-unload paths. Application end calls the
  plugin's end hook before finishing sockets and devoptab and releasing
  imported modules. Plugin unload can also call deinit and wrapper finalizers.
  [Lifecycle dispatcher](https://github.com/wiiu-env/WiiUPluginLoaderBackend/blob/e3e67f8a8ebee77b351a43ff79561ecb6955dea6/source/main.cpp)

## Current comparison and test limits

The console and an isolated Cemu launch used the same Revival WPS
(SHA-256 `0d62a97f5c83f18ca12aaa6b4407a17e43cd0a86b07813d44e2c16d7c21b1488`).
Both reported identical service settings at mount and accepted their first
two map responses at 6,331 and 5,739 bytes. The hardware log recorded
fetch durations of 1,405 and 871 ms, and this Cemu run recorded 875 and
416 ms. The logs establish response sizes and successful application
handling. The bodies were not captured for byte comparison, and the
separate runs do not control for network conditions or provider caching.

Cemu maps one explicitly selected plugin and invokes its startup hooks on
the title's default guest thread after heap setup. Its content-redirection
module dispatches synchronous file operations to guest device callbacks.
The HTTPS compatibility module supplies the guest curl wrapper exports
using Cemu's host curl and delivers response chunks to the plugin's guest
callback. The provider HTTP status and plugin return value are distinct.

An isolated Cemu trace stopped in the actual plugin's `fetchMap` and
`receiveBody` while requesting Shibuya coverage tile `17/116399/51623`.
The raster provider returned HTTP 403. The metadata fallback returned HTTP
200 and the successful trace yielded 136 positions and 144 unique links.
A separate trace received HTTP 200 but rejected the response buffer because
the expected prefix was absent. The plugin's buffer-growth change was
followed by one successful fallback request without debugger breakpoints.
Repeat runs are needed to establish whether the intermittent failure is
resolved.

Console startup applied function replacements that Cemu has yet to run.
The emulator also reports unavailable WUPS settings/storage and kernel
services. Guest-initiated process exit and title changes now dispatch exit
request, application end, sockets finalization and devoptab finalization
before starting host teardown. A WUT-built probe verified that the application
end hook can signal and join a guest worker. Window-driven shutdown queues
the same hooks on a dedicated guest thread and waits up to eight seconds
before stopping the scheduler. The WUT probe also joined its worker when
Cemu was closed through File → Exit. Later checks below cover File → Close
game and an earlier hardware title exit.

## Original Aroma menu

A read-only copy of the console's environment supplied its root RPX, WUMS
loader RPX, PluginBackend module, and five supporting modules. The shared
image reader decoded their PowerPC sections and identified the modules by
their `.wums.meta` sections. The explicit `--aroma-plugin` launch option
continues to select WUPS plugins.

The upstream PluginBackend declares dependencies on FunctionPatcher,
MemoryMapping, Notifications and ButtonCombo. Its static hooks watch
GamePad input and open the configuration UI at the GX2 swap boundary.
[PluginBackend dependencies](https://github.com/wiiu-env/WiiUPluginLoaderBackend/blob/e3e67f8a8ebee77b351a43ff79561ecb6955dea6/source/main.cpp)
[Menu hooks](https://github.com/wiiu-env/WiiUPluginLoaderBackend/blob/e3e67f8a8ebee77b351a43ff79561ecb6955dea6/source/patcher/hooks_patcher_static.cpp)

Upstream WUMSLoader reads module names, ABI versions, and dependency names
from the image, orders modules by dependency, and collects export and hook
records after applying local relocations. Cemu can now read those records
and check their pointers against assigned guest section addresses before
import linking.
[WUMS module ordering](https://github.com/wiiu-env/WUMSLoader/blob/bc7113f1e5a290dc1335098adc74d934d2c171ff/wumsloader/src/module/ModuleManagement.cpp)
[WUMS export and hook records](https://github.com/wiiu-env/WUMSLoader/blob/bc7113f1e5a290dc1335098adc74d934d2c171ff/wumsloader/src/module/ModuleLinkInformationFactory.cpp)

A host-side parser probe used the six WMS files copied from the console.
All reported WUMS ABI 0.3.6. PluginBackend exposed 42 exports and 16 hook
records, including the `WUPSConfigAPI_InitEx` export. Both FunctionPatcher
and MemoryMapping declared a dependency on `homebrew_kernel`. The probe
validated the relocated addresses without running module code. Guest
export registration, import linking, lifecycle dispatch, and guest
function replacement are still needed to run the original menu in Cemu.

A non-executing full relocation probe with placeholder import addresses
also reached an `_SDA_BASE_` relocation outside the bounds of its ELF
`.data` section in all six modules. Cemu now rebases WUMS symbols
against their text or data segment, matching upstream WUMSLoader,
and checks that the caller preserves relative section offsets. All
six modules passed the host-side relocation pass. A second pass used
the relocated export addresses for imports between the captured
modules, resolving three distinct imports. Four `homebrew_kernel`
imports still needed placeholders, as did Cafe OS imports. These
checks covered linking in memory on the host. Guest execution and
the configuration menu still need verification.
[Upstream WUMS fixed relocations](https://github.com/wiiu-env/WUMSLoader/blob/bc7113f1e5a290dc1335098adc74d934d2c171ff/wumsloader/src/module/ModuleLinkInformationFactory.cpp#L145)

The fork also accepts `--aroma-link-modules /path/to/modules` during
title launch. It allocates guest segments and collects all exports
before linking imports, then installs the linked images if every module
links. A two-module fixture launched with the consumer sorted first
and linked its data import from the provider. The copied console set
reached a missing `sysapp::_SYSLaunchSettings` import from
AromaBaseModule during the same Cemu launch path. Reading the fixture
consumer's guest memory through the debugger confirmed the relocated
pointer matched the provider's data address.

Upstream AromaBaseModule imports `_SYSLaunchSettings` in its
`SYSReturnToCaller` replacement to send the three regional System
Transfer titles back to the transfer page in System Settings. WUT
documents the call as scheduling System Settings when the current
application exits. Wii Street U startup and PluginBackend's menu
hooks do not exercise that System Transfer branch.
[AromaBaseModule return hook](https://github.com/wiiu-env/AromaBaseModule/blob/f0af3da8152d9241b864eb070ffda0cf40a8b9fa/source/patches/patches_replacements.cpp#L58)
[WUT launch API](https://github.com/devkitPro/wut/blob/0e216c360a305f2fa9ff7eca948132204bfc13ae/include/sysapp/launch.h#L182)

An isolated Wii Street U launch now reports all 19 missing requirements
for the copied module set in one pass. This count is by module and
requirement, so dependencies shared by multiple modules appear more
than once. The original PluginBackend needs six unavailable Cafe OS
exports in this launch, including AVM display queries. ButtonCombo
needs three controller exports. FunctionPatcher and MemoryMapping
need the absent kernel module. The loader withheld the WMS images and
continued with the exact console Revival WPS through the explicit
plugin path. The WPS mapped and mounted `wsudata:/`, and the title
initialized its panorama graphics. A two-module fixture also still
linked successfully with the same build.

Cemu now orders image installation by declared module dependencies
after resolving imports. An isolated launch installed the provider
before its consumer even though their filenames sorted the other
way. A fixture with reciprocal dependencies reported a cycle before
installing either image. Cemu dispatches the application start, exit
request, and end hooks, including the corresponding all-modules
completion phases, for module sets that declare only these hooks.
Two fixtures ran all twelve callbacks in provider-first order and
exited cleanly in both CPU modes. Module sets requiring WUMS runtime
initialization remain link-only until the runtime hooks are supported.

The supported application lifecycle also dispatches devoptab and socket
setup before application start, and their finalizers after application end.
Two fixtures ran all 20 callbacks in order and exited cleanly in both CPU
modes. A fixture declaring early initialization linked without dispatching
callbacks, since the early relocation and initialization phase still needs
implementation.

WUMS ABI 0.3.6 reentrancy and thread setup hooks now run before the
application service hooks. WUMS reuses the same per-guest-thread context
callbacks as WUPS. The two-module fixture exercised dispatch of all 24
hooks in both CPU modes. A second guest probe verified the reentrancy
table reports ABI version 2, then called both function pointers and
recovered the same context value it registered on the current thread.
A follow-up module used an application-start hook to exit its guest
thread via `OSExitThread`. Its registered cleanup callback wrote the
original context value into guest memory, which the debugger verified.
This exercises the thread-exit callback directly. It does not exercise
registration from a separately spawned WUMS worker.

Cemu now dispatches WUMS malloc, newlib and C++ setup hooks after
reentrancy and thread setup. The runtime heap is available to those
hooks before they run, even when no WPS is selected. A combined fixture
verified all 30 WUMS hook calls and the WUPS worker join in both CPU
modes. The WUMS setup callbacks in that fixture return immediately, so
it tests dispatch order alongside a working WUPS runtime. It does not
verify WUMS guest libc initialization end to end. A separate WUMS-only
probe exercised the allocator directly from its malloc startup hook in
both CPU modes. It allocated a 64-byte block aligned to 128 bytes,
checked the expanded-heap block size and a written value, and returned
from the matching free call.

Cemu dispatches WUMS constructor and destructor wrapper hooks around
the application lifecycle, honoring each module's `skipInitFini`
metadata. In both CPU modes the provider ran both wrappers, the
consumer skipped them, and all 32 expected callbacks completed.
The fixture tests dispatch and metadata handling with return-only
callbacks. Cemu now supplies a version 14 module-information table
to regular relocation-complete and initialization hooks. A guest
fixture followed the argument pointer, and debugger inspection verified
the provider-first names and the provider's export and hook counts.
The table currently describes installed images and their declarations.
It leaves relocation history, acquired RPLs, function symbols and
entrypoints empty. Early initialization is still link-only.

Aroma's drawing code queries the TV output port and scan mode to
select its scale, and reads the aspect ratio on the analog fallback
path. Cemu's AVM and TVE exports now report the same virtual display
as GX2, with 16:9, 1080p, and HDMI. `OSIsECOMode` reports normal
power mode. A guest plugin called all four APIs and the debugger
verified its stored return values and output words.
[Aroma menu scaling](https://github.com/wiiu-env/WiiUPluginLoaderBackend/blob/e3e67f8a8ebee77b351a43ff79561ecb6955dea6/source/utils/DrawUtils.cpp)
[WUT display API](https://github.com/devkitPro/wut/blob/0e216c360a305f2fa9ff7eca948132204bfc13ae/include/avm/tv.h)

The loader now adds known Cafe OS dependencies declared by WPS and
WMS import sections before resolving those imports. The display probe
passed with only its WPS selected. An isolated launch of Wii Street U
with the six console modules reduced the reported missing requirements
from 19 to 15, and the exact console WPS still mounted its data. The
original menu still requires controller, display-memory, launch and
kernel compatibility.

ButtonCombo temporarily enables Pro Controller support while recording a
combo, then restores the previous setting. Cemu now implements the WPAD
enable and query calls and filters guest WPAD and KPAD lookups accordingly.
The KPAD shutdown export releases the configured ring buffer and disables
KPAD reads until the guest calls KPADInit again. A guest probe verified that
the configured Pro Controller disappeared from WPADProbe while disabled
and reappeared when re-enabled. The probe also verified that KPADReadEx
returned uninitialized after shutdown and resumed normal status after init.
The Wii Street U launch reported 13 missing requirements after the
padscore changes.
[ButtonCombo controller lifecycle](https://github.com/wiiu-env/ButtonComboModule/blob/416d0608599405a0ef1e7bfc61ef89537212b85f/source/ButtonComboManager.cpp#L654)

ButtonCombo disables the GamePad TV remote overlay when a registered combo
uses the TV button. Cemu's virtual GamePad has no TV remote overlay, so its
VPAD compatibility calls accept the suppression request and report that
the overlay is closed. A guest probe verified the calls on both channels
and confirmed execution returned to the caller. This clears the VPAD
import needed by ButtonCombo. It does not implement a TV remote overlay.
[ButtonCombo TV menu handling](https://github.com/wiiu-env/ButtonComboModule/blob/416d0608599405a0ef1e7bfc61ef89537212b85f/source/ButtonComboManager.cpp#L435)
[WUT VPAD API](https://github.com/devkitPro/wut/blob/0e216c360a305f2fa9ff7eca948132204bfc13ae/include/vpad/input.h#L709)

Aroma reads and saves display registers before it draws the configuration
menu. Cemu now exposes a guest-backed register window for the display
controller address used by Aroma. It reports the existing OSScreen
buffer geometry. A guest probe read both drawing widths, changed and
restored the TV pitch register, and verified that an unrelated address
returned null. The backing window holds guest register writes. It does
not drive the renderer's scanout or change the actual buffer pitch.
[Aroma display register access](https://github.com/wiiu-env/WiiUPluginLoaderBackend/blob/e3e67f8a8ebee77b351a43ff79561ecb6955dea6/source/utils/dc.h)

The backend requests a restart with `_SYSLaunchTitleWithStdArgsInNoSplash`
after saving the active plugin list. It then finishes menu cleanup.
Cemu queues the requested title and supplies a ProcUI exit message.
The existing guest process exit path consumes the queued title after
the WUPS exit hooks and hands it to the title launcher. A guest probe
verified that the request returned and the system queue delivered the
exit message. A separate two-title fixture made the caller exit after
receiving that message. Under host GDB, Cemu mounted the requested
title and reached its main function. Plain Xvfb runs, including runs
with line-buffered output, reached the caller's process exit but did
not reach the target within the test window. Shutdown tracing placed
the stall in the scheduler join. The main idle core polled system
events without checking whether the scheduler was stopping. With the
stop check at the top of the idle loop, three plain multicore runs
mounted the requested title and reached its main function.
[PluginBackend restart and cleanup](https://github.com/wiiu-env/WiiUPluginLoaderBackend/blob/e3e67f8a8ebee77b351a43ff79561ecb6955dea6/source/utils/config/ConfigUtils.cpp#L251)

The captured PluginBackend and its five required modules link in both the
Wii U Menu (0005001010040100) and Wii Street U (0005000010132000).
Cemu now accepts their unload hooks for module deinitialization and the
WUT runtime finalizers. A guest-exit fixture verified that the consumer
finished before its provider and exited with status zero. The fixture
exercised callback ordering. A separate window-close attempt on the
idle fixture timed out before the unload callbacks, so that path remains
unverified with these changes. The retail module set is still link-only
in both titles because KernelModule, FunctionPatcher and MemoryMapping
request early initialization. See
`aroma-module-link-check/settings-menu-unload.log`.

Cemu now runs early module initialization before the relocation-complete
callbacks and regular module initialization afterward. This orders guest
callbacks over already-linked images. It does not reproduce WUMSLoader's
two separate relocation passes or custom RPL allocator. A two-module
fixture observed early initialization, both relocation-complete callbacks,
regular initialization and a clean guest exit in that order. The original
KernelModule and FunctionPatcher initialization callbacks ran on the Wii U
Menu in the interpreter and on Wii Street U in the recompiler.
FunctionPatcher stopped both launches with an error patching
`OSDynLoad_GetRPLInfo` or `OSDynLoad_GetNumberOfRPLs`. PluginBackend
initialization and the settings menu are still pending. See
`aroma-module-link-check/settings-menu-early-init.log`.

Module-only launches now expose the same two dynamic-loader compatibility
slots as direct WPS launches. Imports of KernelModule's `KernelCopyData`
from WUMS modules use the existing guest-memory copy implementation.
Both titles then advanced past the captured FunctionPatcher's initialize
hook and entered the captured MemoryMapping module. Both faulted at the
same module-relative instruction, a read at guest address `0x017ff000`
in `SetupIBAT4DBAT5OnAllCores`. This is part of the original hardware
memory-mapping setup. PluginBackend still has not started.
See `aroma-module-link-check/settings-menu-mapping-crash.log`.
[FunctionPatcher startup](https://github.com/wiiu-env/FunctionPatcherModule/blob/1a294db7eab9c87d561fbbd3ab08a71a565b1ef2/source/main.cpp#L76)
[MemoryMapping initialization](https://github.com/wiiu-env/MemoryMappingModule/blob/b849fecf421f9f7ce3056d8d08581ef0bb0a4d91/source/main.cpp#L50)

## Deterministic transport check

A separate Cemu probe used the same guest curl wrapper and the original
`realloc` buffer-growth pattern. Two guest workers fetched a known 65,536-byte
body from a loopback server 12 times each. All requests returned HTTP 200 and
the full byte count, but five returned corrupted data. This reproduces the
integrity failure independently of Google. It does not establish whether
the failure occurs inside the allocator, the copy, or the callback handoff.

The next transport run checked bytes at callback entry, before and after
buffer growth, and after appending each chunk. No incoming chunk or append
mismatch was recorded in that run. With WUT `realloc`, 17 of 24 downloads
failed and corruption first appeared during growth. With explicit
allocation and copy, all 24 passed. The interpreter reproduced the
`realloc` failure. On failing allocations, the guest's
`MEMGetSizeForMBlockExpHeap` reported zero or an implausible size,
sometimes immediately after allocation. The allocation and heap metadata
path still needs inspection before attributing this to a specific Cemu
implementation defect.

Allocator tracing confirmed that the WUT title replaced its coreinit
allocator pointer with a newlib `memalign` implementation. Its allocations
do not have the expanded-heap headers that the plugin's WUT `realloc`
queries. Upstream Aroma redirects the three default-heap imports to its
plugin allocator. Cemu now redirects those data imports to a dedicated,
thread-safe 32 MiB expanded heap reserved before the title allocates its
memory. The original `realloc` regression probe passed all 24 requests
with the recompiler and all 24 with the interpreter.
[Upstream allocator import resolution](https://github.com/wiiu-env/WiiUPluginLoaderBackend/blob/e3e67f8a8ebee77b351a43ff79561ecb6955dea6/source/PluginManagement.cpp)

After the scheduler fix, the WUPS exit probe released and joined its
worker and the emulator exited with status zero in both CPU modes.
A fresh multicore transport run exposed a separate first-use crash
in `curl_easy_init`: the global initialized counter was set before
the guest allocator callbacks were installed. Initialization now
holds the curl mutex through allocator setup and host libcurl init.
The same two-worker probe passed all 24 transfers in each of two
multicore runs and one interpreter run.

Regular WUMS initialization now completes every provider's setup before
starting its consumer, following upstream WUMSLoader's per-module
initialization. The loader runs devoptab and socket setup again before
the application-start notification, as on upstream. Both CPU modes
passed the updated two-module sequence with all 36 expected callbacks.

The direct WUPS linker now resolves imports from installed WUMS module
exports. Exports from a link-only module set are withheld until its
runtime hooks can be supported. The two-module fixture and a separate
WPS verified in guest memory that a WUPS data import pointed to the
provider module's exported data.

A separate startup fixture exercised the function-import path. A WPS
application-start hook called a function exported by a WUMS image
through its relocated PowerPC branch. The debugger verified that the
plugin stored the function's expected return value, `0x4321`, in guest
memory. This test used the direct linker and does not cover OSDynLoad
acquisition of WUMS modules.

Guest code can acquire installed WUMS modules by name through OSDynLoad,
look up their function and data exports, and query the module name.
WUMS handles are pinned for the title lifetime and releasing one does not
unload the module. An isolated WPS called OSDynLoad_Acquire and
OSDynLoad_FindExport, then called the returned PowerPC function and
stored the expected result, `0x4321`. This test did not exercise module
notifications or the full Aroma backend.

The post-OSDynLoad smoke test ran Wii Street U with the exact console
WPS, a fresh emulator profile, and the six captured WMS images selected.
The WMS audit still reported ten requirements. The WPS mounted its
data and the title initialized panorama graphics. The first four
live map responses were accepted, including the same 6,331- and
5,739-byte startup responses seen in the hardware log.

A window-close test used the exact console Revival WPS. Cemu received
a normal X11 window-close request after the first live map response.
The application-end hook ran, an in-flight second map request finished,
the socket finalizer ran, and Cemu exited with status zero. This
verifies emulator shutdown for the real plugin. The earlier hardware
shutdown trace is described below.

The fork exposes File → Close game when an Aroma plugin or module
directory is selected. In an isolated UI run with the console WPS,
that action dispatched the application-end and socket finalizer hooks
and returned to the game list. Selecting Wii Street U again in the
same emulator process mapped the console WPS a second time. The test
closed the emulator after seeing the second mapping, before checking
a second live map response. Repeated cycles remain unverified.

A full same-process relaunch initially crashed in the recompiler while
WebKit evaluated JavaScript. Aroma executable allocations and coreinit's
generated-code range are now released during title teardown. The second
launch reused the same executable address but still crashed until the
interpreter-to-recompiler transition checked that the target lookup
block was mapped. With both fixes, three consecutive launches in one
multicore Cemu process each accepted a live map response and completed
the Revival socket finalizer through File → Close game.

A read-only FTP capture verified that the console's current
`maps-compat.js` and `streetview-direct.js` differ from the earlier
Cemu comparison profile. The emulator copy included revised coverage
hit detection and a Shibuya coverage replay. An isolated Cemu run
used the two byte-identical console scripts and the captured console
WPS. It mounted the data device, initialized panorama graphics, and
accepted startup map responses of 6,331 and 5,739 bytes. This matches
the recorded startup sizes. The console files were copied after the
hardware trace, so their identity at the time of that trace is
unverified. Other title assets were not checked for parity.

A subsequent read-only SD capture found the TV and GamePad entry
HTML byte-identical to Cemu's test copy. The launcher config differed
in the TV entry path, input flags, and requested RPLs. A fresh Cemu
profile ran with the console launcher config, the console WPS, and
the two captured map scripts. The console's content path was mapped
to the isolated copy of the same HTML entry files. The title mounted
the data device, initialized panorama graphics, and accepted both
startup responses at the same byte counts. This covers the launcher
config and entry files. Full SD content parity and later UI behavior
remain unverified.

Coreinit file-access logging on the matched-config Cemu profile confirmed
successful opens of the launcher config and both HTML pages under the
console entry path, as well as both captured map scripts. It recorded
guest opens for the two startup map requests under the redirected
`wsu-direct` mount, followed by accepted provider responses. The
hardware capture confirms the mount and request completion, without
a matching per-file open trace.

A diagnostic Revival build adds the provider and tile coordinates, HTTP
status, response size, and delivery result to detailed map logging.
Cemu verified that both startup responses were from EOX satellite
imagery at zoom 14. This corrects the earlier description of those
responses as road-map tiles. The hardware trace records the same byte
counts but cannot identify their provider or coordinates. The new
build was tested in Cemu only.

The hardware logger is listening on TCP 7965 and has an established
console connection. Its latest capture has no per-tile map keys or
Revival exit trace. A read-only checksum of the active debug Aroma
plugin still matches the original console WPS. The tested diagnostic
WPS is staged locally in the workspace under
`aroma-module-link-check/WiiStreetU-Direct-map-trace.wps` with SHA-256
`ab00ccbd4b38ff7954d7355906f3f2ece35c8a5900b91497ac73ce10efbf29d5`.
A matching hardware capture requires loading that build and running
the same startup and exit sequence on the Wii U.

The newer map-and-shutdown diagnostic WPS also reports filesystem detach,
worker joins, and completion of application-end cleanup. In Cemu, a
window-close request after the first live map response ran the end hook.
Both workers joined, including the map worker that completed its in-flight
request during shutdown. The application logged completion and Cemu
exited with status zero. The diagnostic WPS is staged locally for a
matching console capture.

The console trace also identifies two distinct filesystem layers for
the same launch. At 00:00:42.868 SDCafiine registered a merge layer from
the title's SD modpack over `/vol/content`. At 00:00:42.908 Revival
registered a replace layer from `wsudata:/` over
`/vol/content/app/HexagonRe_for_1.0j/wsu-direct`. The same trace shows
SDCafiine skipping add-on content because the modpack had no `aoc`
directory. The matched Cemu run used a host symlink for the outer
content mapping, so its successful file opens establish the inner
Revival layer and equivalent paths for the sampled files. They do
not exercise SDCafiine's merge and stock-content fallback. A direct
comparison should open an overridden file, a stock-only file, and a
Revival request through both layers in the same session.

The fork now accepts the read-only SD content-merge registration used
by SDCafiine. The FSA lookup uses the SD override when present, falls
back to the stock file on a miss, and honors a per-file deletion
marker. Revival's newer nested device registration still handles the
live request files. An isolated Cemu launch moved the prepared HTML
and scripts out of the title tree and onto the emulated SD card. A
diagnostic WPS registered the outer layer through `CRAddFSLayer` and
verified the three file cases, then launched the regular Revival
startup code. Both live satellite responses arrived with the same
keys and byte counts as the earlier diagnostic run. See
`aroma-module-link-check/revival-content-merge.log`.

This comparison exercises the API used by SDCafiine with a diagnostic
caller. It leaves the actual SDCafiine plugin lifecycle and merged
directory enumeration unverified. Upstream implementation reference:
<https://github.com/wiiu-env/ContentRedirectionModule/blob/main/src/export.cpp>.

A read-only FTP capture retrieved the console's `sdcafiine.wps`
(`251b60b9dbd111f97ff11f0d96d6e7b6e4bfdd7c4b2f81a33434dfbf4a43887d`).
A Cemu launch using that binary stops before its startup hook.
The WUPS loader now reports the complete set of unresolved imports
for a plugin and withholds the image when any import is missing.
This SDCafiine binary has two missing imports, both data exports
from `homebrew_memorymapping`: `MEMAllocFromMappedMemoryForGX2Ex`
and `MEMFreeToMappedMemory`. The console's matching MemoryMapping
module is already captured, but its own kernel requirements are
listed in the module audit above. The diagnostic merge probe still
loads successfully after the WUPS diagnostic change. See
`aroma-module-link-check/sdcafiine-console-binary.log`.

For plugin linking, the fork now resolves the two mapped-memory data
imports used by SDCafiine to aligned allocation and free on Cemu's
dedicated Aroma guest heap. The two exports are guest function-pointer
slots, matching the public libmappedmemory declarations. Cemu's
OSScreen reads its buffers directly from guest RAM, which makes this
a compatibility implementation for the screen-buffer use case.
Real WUMS exports take priority when available. This does not load or
implement the original MemoryMapping module.

The captured SDCafiine binary now links, runs its initialization and
application-start hooks, and opens the emulated SD modpack directory
for Wii Street U. Its settings reads fail because WUPS storage is
uninitialized, and config registration reports an invalid plugin
identifier. Upstream SDCafiine defaults to manual modpack selection
without stored settings. The test did not reach layer registration
with the original plugin. See
`aroma-module-link-check/sdcafiine-startup.log`.

A read-only console SD capture found that SDCafiine enables single-modpack
auto-apply, leaves the preparation screen enabled, and enables the plugin.
Upstream PluginBackend initializes storage before the plugin init hook.
The fork now reads root-level booleans from `config/<storage_id>.json`
beside the WPS and supplies the WUPS storage read callback before plugin
initialization. With the exact console WPS and its settings file, Cemu
ran SDCafiine through modpack discovery and registered the content merge.
The settings write path and config menu registration still report errors.
See `aroma-module-link-check/sdcafiine-storage.log`.

Repeat `--aroma-plugin` in application-start order to run more than one
WPS. The loader retains all mapped hook tables and associates each
storage callback with its plugin's own settings. A combined launch used
the captured console SDCafiine and Revival binaries with the console's
SDCafiine settings. SDCafiine registered the content merge first, Revival
mounted its nested device next, and Wii Street U opened its startup HTML
from the SD modpack. The first live responses measured 6,331 and 5,739
bytes, matching the console startup trace. Closing the Cemu window
dispatched both application-end hooks and returned status zero.
See `aroma-module-link-check/revival-sdcafiine-dual.log`.

A repeat of the combined launch delivered both startup map responses
and exited with status zero. ContentRedirection now logs each successful
layer removal. The resulting trace confirms that the SDCafiine layer at
`/vol/content` and the Revival layer at `wsu-direct` were each removed
by their respective application-end hooks before title teardown.
See `aroma-module-link-check/revival-sdcafiine-repeat.log`.

The earlier console capture `gyro-20261112-020920-567629.log`
contains GiveMiiYouTube's startup patch message and service callback.
The latest Cemu YouTube launch stops during plugin linking and later
crashes in its title startup. The two observations are recorded in
`aroma-module-link-check/youtube-current-build.log` and the hardware
capture. They leave the guest application and plugin failures separate.

A control launch of retail YouTube v193 in the same fork with no
`--aroma-plugin` reproduced the signal 11 at the same offset in
`coreinit::OSLockMutexInternal`, following the same SWKBD acquisition
message. This verifies that YouTube's title startup crash can occur
independently of GiveMiiYouTube's unresolved imports. See
`aroma-module-link-check/youtube-without-plugin.log`. The console
TCP logger remained connected during the WiiLoad transfer, but it
did not receive the launcher marker or a new title-start record.

The combined test also passed with `--python-debug`. The isolated
profile used loopback port 1342. The debugger read the paused entry PC,
its first instruction, and the guest thread count, then detached.
Revival delivered the two startup responses, both plugins removed their
filesystem layers on close, and Cemu exited with status zero. See
`aroma-module-link-check/revival-dual-python.log`.

A separate November 12 hardware session, recorded in
`gyro-20261112-162023-998552.log`, captured a normal Wii Street U v0.4.14
title exit. The title retrieved the exit message, its WebKit worker
finished, and ContentRedirection logged the attempt to remove
`wsudata:` before all three title-core driver shutdowns completed.
The capture does not establish the binary hash for that earlier run
or include the Revival worker joins or SDCafiine layer removal.
Cemu records both successful layer removals in its combined run.
See `aroma-module-link-check/revival-hardware-shutdown.log`.

The GamePad startup uses a positioned read to load Revival's
`settings.auto-enter.txt`. Cemu opened the virtual file successfully
but sent `FSReadFileWithPos` to ordinary FSA, which rejected the virtual
handle. The fork now seeks and reads through ContentRedirection for
that synchronous call, matching the normal FSA cursor behavior.

With the exact console WPS, an isolated existing Cemu save and
auto-enter enabled in the isolated plugin settings, the same two-byte
read succeeded. Two launches then reached Street View and fetched live
metadata and panorama tiles without a host bridge. The second run
completed eight level-2 tiles for panorama trace ID 4144544521 and
exited cleanly after removing both layers. The console log
`gyro-20261112-162023-998552.log` also records that ID completing at level
2 with eight tiles. This comparison establishes matching application
trace IDs and tile counts. It does not compare the underlying image
bytes or establish timing parity.

A longer combined Cemu run also reproduced the console trace's
sharpening sequence for panorama 4144544521: eight level-2 tiles
followed by 28 level-3 tiles. It used the captured console plugins
and the isolated auto-enter setting, then exited normally with both
layers removed. A separate Guide tile returned HTTP 403 during this
run, so the panorama result does not establish coverage parity.
See `aroma-module-link-check/revival-panorama-sharpen.log`.

The Guide 403 can be matched to an exact earlier hardware response.
For zoom 19 tile (465597, 206492), the console recorded HTTP 200
and all 8,248 response bytes. The captured chunks reconstruct a
valid 256×256 PNG. Two existing Shibuya replay profiles embed
byte-identical copies of that PNG. This establishes a difference in
live availability between the two observations and supplies an exact
input for rendering tests. The observations occurred at different
times, so the response difference alone does not isolate the cause.
See `aroma-module-link-check/revival-shibuya-tile.log`.

In the YouTube v193 control run, SWKBD advertised synthetic font
work and the title called OSLockMutex with guest address 0xac.
Cemu's keyboard implementation handles font rendering and text
entry on the host. It now reports no pending guest font or
prediction work and keeps the existing creation delay. The title
passed the former crash point both in a control launch and after
a Python debugger attach and detach. GiveMiiYouTube itself still
fails to link on its three missing exports. Upstream also declares
an NN_ACT function replacement, so successful import resolution
alone would not verify that the plugin works. See
`aroma-module-link-check/youtube-keyboard-fix.log`.

The console's native `nn_ac.rpl` was copied read-only from the SLC
system-library directory and tested through Cemu's existing
`cafeLibs` override in an isolated profile. The module supplied
both `ACClose` and `ACFinalize`. The captured Aroma KernelModule
linked but its early runtime was unavailable. GiveMiiYouTube's
remaining unresolved import in this run was `KernelCopyData`.
See `aroma-module-link-check/youtube-console-nn-ac.log`.

The WPS linker can now supply the guest-memory copy export when the
original kernel runtime is unavailable. The copied GiveMiiYouTube
WPS linked and entered its startup hook alongside the console
`nn_ac.rpl`. Its dynload instruction check then rejected Cemu's
HLE instruction word. The Python debugger attached at the YouTube
entry point after the hook returned. The trace is in
`aroma-module-link-check/youtube-plugin-startup.log`.

The unmodified GiveMiiYouTube WPS now runs its declared named
foreground-game replacement for the HLE `nn_act` export.
The replacement uses a nearby branch stub and preserves an original
HLE trampoline for the plugin to call. The Python debugger observed
the branch and stopped in the plugin callback during real YouTube
startup. The callback then logged the same service result as on the
console. YouTube displayed its loading splash. Menu and playback
remain unverified. This implementation covers the tested named HLE
replacement form. See
`aroma-module-link-check/youtube-function-replacement.log`.

A repeat launch hit the GiveMiiYouTube replacement again under the
Python debugger. A separate run with socket logging recorded positive
guest receives after connection setup. An extended launch remained on
the loading splash. The trace therefore establishes guest socket data
delivery, while the application-level result and the cause of the
loading stall remain open. See
`aroma-module-link-check/youtube-post-hook-network.log`.

The console `nn_ac.rpl` exposed the needed exports but attempted
to open `/dev/ac_main`, which this fork does not provide. The HLE
library now exports its own AC close/finalize counterparts, with
socket lifetime still managed by nsysnet. In the isolated run with
the console library removed, the debugger reached the plugin's
replacement again and guest socket reads continued. The missing
AC-device and DIAL local-URL errors were absent in this trace.
The red splash still remained during observation.

A follow-up HLE AC run captured the splash after socket traffic and
sampled the LBShell, WebKit, compositor, DIAL, and I/O guest thread
registers using the debugger. The app remained on its splash during
the sample. Thread PCs and link registers are recorded in
`aroma-module-link-check/youtube-splash-threads.log` for comparison
against subsequent runs. The sample does not yet identify an
application-level error or establish why the splash persists.

A host GDB trace of the same build resolved the saved splash PCs:
LBShell Main was in `GX2WaitForVsync`, WebKit, the compositor, and
the I/O thread were in `OSWaitEventWithTimeout`, and DIAL was in
`OSWaitEvent`. These are normal wait primitives. A single sample
does not establish that an event was lost or that these functions
are faulty. See `aroma-module-link-check/youtube-splash-symbols.log`.

Filesystem and unsupported-import logging verified that YouTube
opens its packaged assets and a bundled certificate. The trace
also exposed a missing nsysnet `freeaddrinfo`. Cemu now frees
host resolver results after conversion and pairs guest resolver
allocation with a guest free export. The next debugger-assisted
run resolved www.youtube.com and called the new export, then
continued to show the red splash. See
`aroma-module-link-check/youtube-dns-cleanup.log`.

A debugger-assisted repeat run traced host `getsockopt(SO_ERROR)`
while the app was loading. All observed checks returned zero, and
subsequent guest reads delivered data. This rules out a reported
TCP connection failure for the observed sockets. It does not
establish whether the app accepted the server responses.
See `aroma-module-link-check/youtube-socket-connect-status.log`.

An isolated OpenGL control launch reached the same YouTube splash
as Vulkan. It ran the plugin replacement and received network
data. Cemu reported about 27 frames per second while three sampled
crops of the visible spinner had identical pixels. This shows
the stall can occur on either backend and that the visible
splash can stay unchanged while Cemu reports frame activity.
See `aroma-module-link-check/youtube-opengl-control.log`.

The single-core interpreter control (`--force-interpreter`) passed
the red splash and displayed YouTube's live welcome screen. The
Python debugger stopped in the GiveMiiYouTube callback before that
screen appeared. Earlier recompiled runs remained on the splash.
The interpreter also changes scheduling, so the comparison does
not yet isolate a recompiler bug. Playback remains unverified.
See `aroma-module-link-check/youtube-interpreter-control.log`
and `aroma-module-link-check/youtube-interpreter-welcome.png`.

The interpreter welcome screen was reproduced. Input API tracing
confirmed regular `VPADRead` calls. Channel 0 returns one sample,
then channel 1 returns zero using the same status buffer. A
pause-time buffer snapshot cannot establish whether channel 0
contained the injected A press because the next call can overwrite
it. Verification needs a channel-0 return breakpoint or equivalent
logging. See `aroma-module-link-check/youtube-vpad-polling.log`.

YouTube reuses its VPAD buffer when querying an absent second
controller. Cemu now leaves that buffer intact on the no-controller
return. The debugger observed the A bit in the preserved guest
sample. Pressing A on the live welcome page advanced to Get started
and showed Watch as guest. Guest playback has not yet been tested.
See `aroma-module-link-check/youtube-vpad-preserve.log`.

Using the corrected VPAD implementation and a keyboard-backed
GamePad in the isolated interpreter profile, a subsequent run
advanced through Get started, selected Watch as guest, and opened
the signed-out home and search screens. The search screen showed
live suggestions and accepted text through its on-screen keyboard.
The timed run ended before a video could be selected. Screenshots
are `youtube-guest-home.png` and `youtube-guest-search.png` in
`aroma-module-link-check`.

### Reproduce the GiveMiiYouTube guest session

From the Cemu checkout, pass your retail YouTube v193 RPX and the
original GiveMiiYouTube WPS from your console. Use the single-core
interpreter for this test. The verified guest run used Cemu's built-in
`nn_ac` and the guest-memory implementation of `KernelCopyData`.

```sh
./bin/Cemu_relwithdebinfo --force-interpreter --python-debug \
    --forward-console-logging --aroma-plugin /path/to/GiveMiiYouTube.wps \
    -g /path/to/YouTube/code/lb_shell.rpx
```

Attach the Python debugger on the loopback port configured for that
profile and detach or resume from the entry breakpoint. Use a configured
Wii U GamePad to select Get started, then Watch as guest. The desktop
launch repeated the WPS mapping, replacement installation, and callback.
The separate interpreter test confirmed guest home and live search.
A second isolated Vulkan interpreter launch stopped at the replacement
callback with the Python debugger, then played a public video selected
through guest search. H.264 callbacks advanced past 30 seconds of video
time and captured video pixels changed in both the TV and GamePad views.
The pre-roll image remained identical in two TV samples taken three
seconds apart while the GamePad countdown advanced. Later pre-roll frames
changed. This run did not establish why the pre-roll picture stalled or
why the desktop session reported frozen video with audible playback.
The recompiled startup stall also remains unexplained.
See `aroma-module-link-check/youtube-video-playback.log`.

## Settings menu startup follow-up

The upstream MemoryMapping module uses hardware BAT and page-table setup
before creating its heaps. Cemu uses a flat guest address space. The fork
now supplies the module's allocator and address-translation exports through
Cemu's existing Aroma heap and memory mapping. The original hardware
initialization hooks are skipped for this module.

FunctionPatcher stores its generated branch trampolines in its data
segment and targets them with absolute PowerPC branches. The fork now
places that module's data in the low guest code cave. A separate, muted
Wii Street U run with the captured WMS modules advanced through both
earlier failures and reached PluginBackend initialization. PluginBackend
reported that IOPShell was absent. Its source continues to register the
static hooks after that optional service fails. The next observed fault
was at guest PC `0x01806dc4` in low trampoline storage, with LR
`0x037cb520` in PluginBackend. The run did not reach an interactive title
or verify the menu shortcut.

The dumped retail coreinit `OSGetUPID` starts with `lis r3,0x1001`
and `lwz r3,0x3c20(r3)`. FunctionPatcher copies their immediate operands
into a corresponding r11 load in its process-filter trampoline. Cemu
previously supplied a single HLE opcode at this export, so the copied
operands did not name a process-ID word. The Aroma-mode export now
starts with a retail-shaped load of Cemu's process ID. In the next
hidden Vulkan run the same trampoline at `0x01806dc0` loaded the word
at `0x00e06db4` and filtered process IDs 2 and 15. PluginBackend
completed startup and Wii Street U reached its splash. With SD Cafiine
in the native backend's plugin directory, a separate run logged
`LOAD (ACTIVE) SDCafiine` and faulted at `0x126726cc`. With an
empty directory the shortcut attempt did not visibly open the menu.


A corrected keyboard-backed GamePad profile sent D-pad Down, Minus and
L together in the isolated Wii Street U session. A breakpoint at the
real PluginBackend VPAD comparison recorded mask `0x124`. After the
debugger detached, the GamePad displayed **Wii U Plugin System Config
Menu v0.4.1**. The session used Xvfb, Vulkan and zero TV/GamePad volume.
The native backend's plugin directory was empty, so this verifies menu
entry and rendering. Native plugin registration and settings changes
remain unverified. [GamePad capture](docs/images/aroma-config-wii-street-u.png)


## Four-plugin compatibility validation — 2026-11-18

The isolated Linux test uses **Aroma Base Plugin, FTPiiU, Homebrew on Wii U Menu,
and WiiLoad**, with their required WUMS modules. The guest WUPS backend discovers
and loads the plugins from the active emulated SD environment; Cemu does not map
a second copy of each plugin when that backend is present.

This update fixes the following emulator-side problems:

- Missing module imports prevented the shared backend from initializing. Added
  the default AppIO completion queue and filesystem callback worker, settings
  launch support, positioned FSA reads and the required filesystem helpers.
- WUMS loader-only allocator hooks incorrectly disabled every module lifecycle
  hook. Cemu now recognizes those hooks while retaining ownership of RPL memory.
- Runtime WUMS export tables disagreed with Cemu's linker substitutions. Guest
  dynamic lookups now see the same mapped-memory allocator slots and kernel-copy
  implementation as static imports.
- ContentRedirectionModule calls private Cafe OS FSA request builders at fixed
  firmware addresses. Cemu maps those addresses to its existing request builders
  and allocates their recompiler lookup blocks. Mapping memory alone was not
  sufficient: a compiled indirect call otherwise faulted in the jump table.
- Homebrew on Menu needs the production-settings serial fields. These now read
  the existing EEPROM dump rather than remaining empty. Identifying values are
  not added to these documentation or test reports.
- Added the Mocha API handshake, active-environment path and filesystem-client
  permission compatibility needed by these plugins. Unsupported IOSU operations
  still return errors.
- Unmapped addresses no longer pass `OSEffectiveToPhysical`; `OSIsAddressValid`
  is implemented. This prevented a diagnostic thread from treating an invalid
  pointer as readable during Kirby's exit.
- Account finalization and per-thread run-quantum support are available to small
  homebrew executables as well as larger ports.
- The Python debugger can stop before system/plugin startup and retain its
  listener across an executable reload.

### Verified behavior

| Test | Result |
| --- | --- |
| Four plugins, guest WUPS backend, Kirby Triple Deluxe | Reached the first rendered frame in multicore recompiler mode |
| FTPiiU upload/download | 4,096 bytes matched SHA-256; test file deleted; passed in interpreter and recompiler modes |
| WiiLoad diagnostic receiver to replacement RPX | Replacement executable printed its unique entry marker |
| Kirby to replacement RPX through native WiiLoad | Replacement entry marker confirmed after normal title exit |
| Identity during RPX-only reload | Same title ID and base RPX hash; updated RPX hash changed |
| Debugger after reload | Python reattached and resumed the replacement executable |
| FTP after reload | Upload/download round trip still passed |
| Python debugger unit tests | 15 passed |

WiiLoad's transfer test uses a 50,625-byte diagnostic RPX. Receiving a TCP payload
alone is not the success criterion: the replacement's entry marker must appear.

### Reproducing transfers

On Linux, use `--aroma-ftp-port 2121` to explicitly map the guest FTP listener on
port 21 to an unprivileged host port. The guest still observes port 21. Connect to
the host address shown by the listener; it may bind the PC's LAN address rather
than loopback.

```sh
python3 tools/python/aroma_ftp_smoke_test.py --host YOUR_PC_IP --port 2121
python3 tools/python/wiiload_send.py rebuilt.rpx --host 127.0.0.1
```

The sender uses WiiLoad protocol 0.4 without compression. The tested WiiLoad
plugin's compressed file path selects the same temporary filename for input and
output and later removes it. An uncompressed transfer avoids that separate
plugin problem; the plugin binary was not patched for this test.

Cemu's Mocha handoff currently verifies **RPX-only reloads using the current
title's assets and save identity**. It validates the SD path, bounded file slice
and RPX header, then consumes the prepared executable on the scheduled reload.
It does not establish compatibility with arbitrary new WUHB content mounts or
prove that the full Wii U Menu homebrew interface works. Raw SLC, optical-disc
and USB device access is also not implemented by this test setup; those mount
requests can report errors while SD FTP works.

The MemoryMapping and kernel-copy services use Cemu's flat mapped address space.
These tests validate the plugin paths above, not full Wii U kernel emulation or
hardware-equivalent memory pressure.

### Native spinlock comparison (2026-11-18)

A hardware capture from a 3Dii U title stopped in coreinit's uninterruptible heap lock. Disassembling the locally dumped coreinit showed ownership is cleared before scheduling is restored. The local HLE release now follows that order, and OSIsInterruptEnabled is exported (it previously existed only as an internal helper). A native PowerPC probe installed the same four acquisition/release tracing wrappers used by the hardware diagnostic module and passed 24 transitions: recursive and ordinary locks, with the interrupt mask initially disabled and enabled, restored afterward. Evidence is retained locally with the Aroma flight recorder validation artifacts.

This does not make HLE scheduling hardware-equivalent. The single-core wait still deliberately yields in an uninterruptible state to prevent the emulator itself from deadlocking. Such a successful emulator run cannot establish that native module contention will be safe on Wii U hardware. The tracing hooks preserve lock semantics and do not force-release locks.
