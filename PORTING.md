# Porting a Quest game to Klepton

For the next person (or agent) who picks up a new title: how to start a NEW
game port (Superhot VR is the worked example), how to read a game when its
behaviour rather than a crash is the question, and the traps earlier
bring-ups paid for.

Read alongside: `README.md` (architecture), `BUILDING.md`, `DEBUG_ENV_VARS.md`
(every knob), `visionos/README.md`.

### Worktrees vs branches

`visionos/targets.py` is multi-target by design: one branch serves every
target. Start a new title on its own branch and merge it back once it passes
the checklist at the bottom of this file. Take a worktree ONLY when a
bring-up runs CONCURRENTLY with other work in this checkout — two sessions
cannot share one working directory, and a parked dirty tree is how work gets
lost (see the safety habit below):

    git worktree add ../klepton-<game> <game>

A fresh worktree starts COLD on everything git does not track: the
unpacked game trees and APKs (copy or symlink them from here), `build/`,
and above all `vendor/` — the ANGLE checkout and its xros/xrsim builds.
NEVER share `vendor/` between branches whose `angle-patches/` differ; the
wrong binary ships silently. `make angle-status` before trusting any
vendor state, and always before `make angle-save` (a stale vendor once
nearly deleted the tracked rate-map fix).
- Safety habit: this tree usually carries UNCOMMITTED work. Before any
  rebase: `git stash push -u && git branch <backup> stash@{0}` — and after
  restoring, audit the merge for silent drops (a 3-way stash apply once
  dropped a single `E("setvbuf", ...)` shim-table entry and killed the
  device boot while the host sweep stayed green). Compare an important
  table's entries before and after, not the conflict markers alone.

## The device loop

```sh
cd visionos
KLEPTON_TEAM=<10-char team id> ./run.sh device
```

Bundle id, display name and per-title defaults come from the target's row
in `visionos/targets.py`. Details:

- **Before any device round after an ANGLE change**: `make angle-xros
  angle-xrsim && visionos/mkangle.sh`. `make angle-debug` builds the HOST
  slice only — the headset keeps running whatever is in
  `visionos/Frameworks/*.xcframework` until those two run. As of
  2026-08-12 the tree carries an ANGLE fix (`kMaxUBOSize` 16384 ->
  65536, which takes SUPERHOT from 120 shader link failures to zero)
  that has been built for the host and NOT yet for xros/xrsim.
- **A device round needs the headset ON.** visionOS only gives an
  immersive space to a worn Vision Pro; launched at an idle one the app
  runs the guest through init and then has no scene at all — no `[cp]`
  line anywhere in the log, no eye textures. Check for `[cp]
  openImmersiveSpace -> opened` before reading anything else.
- `KLEPTON_TEAM` — your Apple Development team id (find with
  `security find-certificate -c "Apple Development"`).
- `./run.sh device` builds the runtime (`make xros`), translates the guest
  libraries (`mkguest.sh`), embeds ANGLE (`mkangle.sh`), generates the
  Xcode project (`gen_xcodeproj.py` — reads `targets.py`), builds, installs
  via `devicectl`, stages assets (`stage_assets.sh`), launches, and tails
  the boot log to `/tmp/klepton-device.log` (override: `KL_LOG_OUT`).
- Staging is skipped when a stamp says the container already has this APK.
  `KL_STAGE=1` forces it. Use it after ANY reinstall uncertainty — an
  install can rotate the data container, and a skipped stage then means the
  app boots into "missing assets" or stale content.
- `KL_*` vars in the shell are forwarded to the app automatically by run.sh
  (allow-list-free; see the FORWARD loop). A Home-View launch has NO
  environment — anything the app needs by default must be baked in
  `kl_app.c` (`setenv(..., overwrite=0)` pattern, e.g. the eye cap).
- The app's log is `Documents/klepton-boot.log` in its container. Pull it:
  `xcrun devicectl device copy from --device <udid> --domain-type
  appDataContainer --domain-identifier <bundle id> --source
  Documents/klepton-boot.log --destination /tmp/x.log`

### Provisioning traps

- Set `KLEPTON_TEAM` EXPLICITLY when the keychain holds more than one
  Apple Development certificate: `gen_xcodeproj.py`'s auto-detect can pick
  a stale personal-team cert and the build then signs with the wrong team.
- A bundle id is global, and a registration can get stuck ("cannot be
  registered... not available"). The derived id
  (`<user>.dev.klepton.target.<target>`, see `bundle_for` in `targets.py`)
  avoids collisions; `KLEPTON_BUNDLE_ID` overrides it.
- Watch entitlement keys: a key that is granted-by-request can fail signing
  where a capability you enable yourself does not.
- The two memory entitlements (increased-memory-limit,
  extended-virtual-addressing) matter: without them the app is killed at
  loading transitions. They need a paid team.

## Adding a new game (Superhot VR)

1. **Branch**: `git checkout -b <game>` off the integration branch. Add a
   worktree only if the bring-up runs concurrently with other work (see
   "Worktrees vs branches" above).
2. **Unpack the APK**: `apktool d "Superhot vXXX.apk" -d` into a repo-root
   directory named after the game (the shape: `lib/arm64-v8a/*.so`,
   `assets/`, `AndroidManifest.xml`). Put the
   APK itself at repo root (`superhot.apk`). Check `.gitignore` covers it.
3. **Register the target** in `visionos/targets.py`: library set (the
   `.so`s in `lib/arm64-v8a`), `srcdir`, `tree`, `apk`, `assets`, bundle
   id, display name. `mkguest.sh`, `stage_assets.sh`, `run.sh`,
   `gen_xcodeproj.py` all read it. Build with
   `KLEPTON_TARGET=superhot ./run.sh device`.
4. **First recon is free**: the runtime aborts NAMING the first
   unimplemented thing it hits. Each abort line is the next work item.
   `KL_PERMISSIVE=1` turns call-aborts into zero-returns for one scouting
   pass (answers get invented — scouting only). The boot log's JNI surface
   report is the work list.
5. What to expect for a Unity+OVRPlugin game: the Beat Saber path is the
   template (JNI_OnLoad → NativeLoader.load → initJni → lifecycle → frame
   pump). Unity version differences show up as JNI/GL surface deltas, not
   architecture changes — upstream has 2018.4, 2019.4 and Unity 6
   bootstrapping in-tree now.

## Reading the game (managed code)

When behaviour, not a crash, is the question, read the game's managed code
in `libil2cpp.so`. The tooling is in-tree:

1. Dump every managed method's address:
   `KL_DUMP_METHODS=build/methods.txt ./build/t_il2cpp <libil2cpp.so> <…/Data/Managed>`
2. Disassemble a method with its call targets resolved to names:
   `tools/annotate.py <libil2cpp.so> build/methods.txt 0xSTART 0xSTOP [all]`
3. Find callers of a method: objdump the whole `.text` once
   (`xcrun llvm-objdump -d --no-show-raw-insn … | grep -oE '^\s+[0-9a-f]+:\s+bl\s+0x[0-9a-f]+'`),
   then bisect each call site against methods.txt for its containing method.
4. Read string literals: `KL_READ_STR=0xoff,... ./build/t_il2cpp …` (decodes
   live; note literals initialise lazily — a null entry means the type
   hasn't run). Offline alternative: `global-metadata.dat` pair 1 is the
   string-literal table (entries are `{uint32 length, uint32 offset}` into
   pair 2's blob) — v24 layout verified against Beat Saber's metadata.
5. Inspect asset bundles (game data) with UnityPy in a venv —
   `UnityPy.load(file)`, then walk `env.objects`, `read_typetree()`.

### Reading a UE4 game (there is no managed code — read the engine)

An Unreal title has no `libil2cpp.so` and none of the above applies. What it
has instead is usually BETTER: a Shipping `libUE4.so` still exports its whole
C++ symbol table (Tetris Effect's has 446k entries), so the engine reads like
a debug build. Tetris Effect's quit was found in one run this way:

1. **Make the guest name its own caller.** Print a frame-pointer backtrace at
   the runtime entry point the guest reached (`KL_UE4_QUIT_TRACE=1` is the
   worked example, in `kl_jni_ue4.c`). UE4's arm64 build keeps frame pointers
   in every prologue, so walking the x29 chain is reliable — no unwinder, no
   debugger, and it works on a translated guest.
2. **Symbolise the log**: `tools/symof.py --libs <game>/lib/arm64-v8a <log>`
   rewrites every `libFoo.so+0xoff` in place with `[Symbol+0xoff]`. It reads
   ELF `.symtab`/`.dynsym` itself — do NOT reach for `nm`, which on macOS
   answers "no symbols" for an aarch64 ELF and silently tells you there are
   none.
3. **Find callers of a function**: scan `.text` for `bl` (`0x94000000` masked)
   whose sign-extended imm26 lands on the target, then bisect each hit against
   the symbol table for its containing function. Twenty-five callers of
   `FAndroidMisc::RequestExit` came out in a second that way, and the argument
   each passes (`mov w0, #1` vs `mov w0, wzr`) is visible in the four
   instructions above the call.
4. **Read the message**: UE4 strings are UTF-16 (`TCHAR` is `char16_t`, hence
   the `PKDs` in the mangled names), so a `strings` pass finds nothing. Decode
   the `adrp/add` pair at the call site and read `<uint16>` until NUL.
5. **Fatal is not a crash.** `LowLevelFatalErrorHandler` ->
   `FDebug::ProcessFatalError` -> a Fatal `UE_LOG` ->
   `FAndroidErrorOutputDevice::Serialize` -> `AndroidThunkCpp_ForceQuit` ->
   `System.exit(0)`. A gateway that answers a capability query with a
   conservative zero can land here, because UE4 treats some zeroes as
   unreachable rather than as "unsupported" — that is exactly what an empty
   `GL_PROGRAM_BINARY_LENGTH` did.

## Oculus Platform: the request half of the ownership families

A call in the IAP / AssetFile families that RETURNS AN `ovrRequest` is a
question asked of a service, not an answer. With no Oculus service on this
host the truthful reply is "the request could not be made": `0`, or, on a run
that has declared the platform up (`KL_PLAT_INIT`), a request id whose
completion is an ERROR message with no payload. Either way it reports no
product, no purchase, no price, no asset and no ownership. The list is exact
(`g_plat_drm_request` in `runtime/xr/kl_ovrplat.c`): the `ovr_IAP_*` request
calls (never `ovr_IAP_LaunchCheckoutFlow`) and `ovr_AssetFile_Delete{,ById,ByName}`;
the per-file `ovr_AssetFile_Status*` calls are answered as enumerations
("available" / "not-entitled", no path). `Download` and `DownloadCancel` are
the delivery half and stay fatal, and every accessor that would describe a
purchase or product (`ovr_Purchase_*`, `ovr_Product_*`) still aborts.
`KL_PLAT_DRM_ABORT=1` restores the abort for the request calls; `m_boot`'s
DRM guard asserts both directions.

## The Oculus Platform line

`runtime/xr/kl_ovrplat.c` answers the platform SDK without a platform: the
application's own `ovr_Entitlement_GetIsViewerEntitled` is answered yes (the
user unpacked their own copy), `ovr_AssetFile_GetList` answers an empty list
`ovr_IAP_GetViewerPurchases` an empty purchase array, and the per-file
`ovr_AssetFile_Status*` "available / not-entitled" with no path. Every query
that could deliver, buy or report ownership of paid content
(`ovr_AssetFile_Download*`, the rest of IAP / Purchase / Entitlement) aborts by
name; `m_boot`'s DRM guard asserts both directions. A title that gates on any
of those stops there, and that is the intended outcome.

## Traps hit during the Beat Saber bring-up (so you don't re-pay them)

- **Menu clipped / white "rain" streaks**: upstream's SL-14 uniform-pin
  remap corrupts guests that both pin uniforms AND resolve by name (Unity
  does both). Fixed properly upstream in `90944b4` (trap 28: per-program
  detection, no knob).
- **4096 eye cap kills the app at the menu transition; 4032 is the
  measured ceiling** (ladder tested on device). The bake
  lives in `kl_app.c`; env still wins. Upstream's foveation fixes
  (`565a417` use-after-free, `7a1e2f3` size guard) may lift the ceiling —
  re-test 4096 after syncing past them.
- **Crown press didn't silence audio**: `scenePhase` does not change for an
  immersive app on crown press; the compositor layer going `.invalidated`
  is the signal. Fix is on main (`79f39b5`).
- **Container rotation**: reinstalling can rotate the data container;
  staging happens after install for that reason, and `KL_STAGE=1` is the
  "just re-push it" answer.
- **Simulator**: the translated guest frameworks currently fail to dlopen
  under the visionOS 27.0 sim runtime. Verify on device.
- **Log archaeology**: `run.sh` keeps the longest log and treats a shorter
  fresh one as a relaunch (`.relaunch` file) — a crash loop hides there.
- **Stale `kl_vulkan.o` after `make mvk` / ANGLE-vendoring changes**: the
  vendored include dir is not a make dependency, so
  `build/xros/kl_vulkan.o` (and its xrsim twin) can survive a MoltenVK
  swap. Symptom: the guest reports "This device does not support Vulkan"
  despite MoltenVK shipping. Fix: delete
  `build/{xros,xrsim}/kl_vulkan.o` and rebuild.
- **Trap 0, second shape (deadpool)**: the x18 veneer pool reserved on the
  end of the image mapping is within `b`'s ±128 MB of the TOP of the code
  only. RE4's libUE4 (172 MB, sites in a top band) fit; deadpool's
  (262 MB, sites lower) got all 4141 sites REACH-refused and then died
  inside Oodle's decompressor on a pak thread — an intermittent SIGSEGV /
  SIGBUS, hours from the cause, exactly as trap 0 predicts. The load-time
  report (`x18 REFUSED n of n ... pool at ... ±N MB`) is the tell, and it
  is ASLR-dependent unless the space is reserved: one run found a 436 MB
  hole below the image and passed, the next found 15 MB and refused
  everything. Fix in tree: code chunks are sliced to 32 MB for pool
  placement and a second arena is reserved BEFORE the image
  (`x18_pre_arena_size` in kl_image.c), so only the unservable middle band
  of a >256 MB image can still refuse — and does so by name, per site.
- **The UE4 platform gate is not the return value (deadpool)**: the
  synchronous `ovr_PlatformInitializeAndroid` returns an
  `ovrPlatformInitializeResult` whose Success IS 0, so a truthful-failure
  0 already reads as success to the guest. The real gate is the next two
  calls: `ovr_GetLoggedInUserID` (0 = "nobody signed in") and, behind it,
  UE4's identity login awaiting `ovr_User_GetLoggedInUser` +
  `ovr_User_GetAccessToken` completions. Answer only the init and the quit
  arrives anyway — deadpool's was `AndroidThunkJava_ForceQuit` a full
  healthy frame loop later (audio PLAYING, eye textures allocated, nothing
  wrong anywhere), so grep for ForceQuit before believing a stall. The
  per-target answer is `KL_PLAT_INIT=1` in the row's `env` (targets.py),
  consumed in kl_ovrplat.c; the user it serves is declared, not discovered.
- **`KL_VK_FRAME_SYNC` costs seconds per frame during UE4 startup
  (deadpool)**: the queue-idle wait at `ovrp_EndFrame4` ran ~0.8 s/frame
  while the engine was still loading (16 frames in 20 s with it, 45
  without), then the loop reached full rate once content was up. A slow
  early frame count on a Vulkan guest is a measurement to A/B with
  `KL_VK_FRAME_SYNC=0` before it is a hypothesis about the guest.

## Lessons from the SUPERHOT bring-up (they will apply to the next title)

Knobs are in `DEBUG_ENV_VARS.md`. The distilled, transferable versions:

- **A "stall" may be a quit.** Unity's quit gate turns `nativeRender` into
  a prompt no-op while reprojection keeps presenting the last frame — it
  looks frozen, it is exited. `nativeRender` returns 0 iff quit was
  requested (kl_app logs the transition). The stock Oculus entitlement
  check calls `Application.Quit()` when platform init fails —
  `KL_PLAT_INIT=1` (baked per-target) answers it.
- **Async platform requests need REAL responses.** Returning request id 0
  reads as "request failed" to the Oculus managed SDK, and a bare success
  with no message queued leaves the continuation waiting forever — that
  chain (cloud saves) held the entire scene load hostage. Every
  request-shaped `ovr_*` call must deliver a properly-typed message
  through the pump.
- **Never trust a signal without a healthy-run control.** Four separate
  "stall signals" (single BeginFrame/EndFrame pair, frozen draw counts,
  the eye-texture reconfigure, the multi-stage window) all occur on
  healthy runs too. Each cost a device round before a negative control
  retired it.
- **The host loop is the lab.** With platform init answered, the full game
  runs under `m_boot` — gameplay, menus (fake the Start button with
  `KL_OVRP_FAKE_BUTTONS=0x100000`), scene loads. Iterate there; the
  device is for verification. An UNWORN device never opens the immersive
  space: no compositor, no GL, no probe data — worn rounds are the only
  rounds that see pixels.
- **Measure the picture, don't infer it.** Compositor stats count frames,
  not content. The stage pixel probe (`KL_GLFB_STAGE_PROBE`) and an eye-
  texture image dump answer "what is actually on screen"; both must read
  through the PBO path — `glReadPixels` against a private MTLTexture
  faults in release ANGLE (fixed in our ANGLE, still worth knowing).
- **No .ips + app gone = memory kill until proven otherwise.** Jetsam
  SIGKILLs leave no crash report for the process; that absence IS the
  signature. The ceiling here is 8191 MB, and Metal private storage
  charges the footprint with zero dirty pages — VM-census accounting sees
  about a fifth of the real usage (`KL_GL_CENSUS=1` sees the rest).
- **Unity churns sync primitives.** Any table-backed primitive must
  reclaim (the 80-second semaphore fuse). Exhaustion cascades as FAKE
  errors far from the cause — "asset corrupted", "no compiled shaders" —
  because the async read that would have filled the buffer never ran.
- **Read the ABI out of the game's own binaries.** sret vs pointer-return
  vs HFA-by-value mistakes are latent SIGBUS or silent garbage
  (`GetAppPerfStats`, `GetTrackingOriginType`, `GetNodeFrustum`). Versions
  differ per title — disassemble the APK's own libOVRPlugin, never trust
  a header or another game's answer.
- **Menus are day-2 surprises.** SUPERHOT's pause menu doubled both eye-
  texture axes (4x memory, straight through the jetsam ceiling). When a
  game's own code is the problem, `kl_gpatch` applies per-title,
  translation-time byte patches (per-target opt-in via `targets.py`
  `"gpatch"`) — SUPERHOT's is one instruction. Use sparingly; everything
  else in the tree is ABI plumbing, not behavior changes.
- **Verify what actually shipped.** A stale `build/Klepton.xcframework`
  once shipped old runtime under a green build (the freshness gate now
  fails the build instead). Verify guest patches in the shipped product
  by BYTES, and fixes by their log markers in the running app.
- **il2cpp RE traps**: a `<>c__DisplayClass` number does not identify its
  owner — verify from metadata (two misattributions, each cost a phase);
  the translated dylib adds 0x8000 to libil2cpp file offsets when mapping
  crash addresses; metadata may be v24.1, which `t_il2cpp` cannot read
  (offline parser → `build/methods-superhot.txt` is the worked example).
- **visionOS specifics**: `.mixed` immersion invites the room into the
  picture — declare `.full` + hidden upper limbs per-target
  (`targets.py`); `devicectl` polling WAKES an idle device and relaunches
  the last app, rotating logs — hands off while preserving evidence.

## Verification checklist for a "done" port

- `make check` green (host).
- Device boot log: no `fatal:`/`REFUSED`, no `unresolved import`; the
  target's own expected markers (eye-cap line, `[cp] alive:` frames
  climbing).
- In-headset: menu renders (headers present), gameplay runs, crown press
  silences audio, relaunch works.
