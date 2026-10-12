#!/usr/bin/env python3
"""Which guest a visionOS build is for — one table, three consumers.

`mkguest.sh`, `stage_assets.sh`, `run.sh` and `gen_xcodeproj.py` all need the
same handful of facts about a target, and until there was a second one they each
carried their own copy as Beat Saber literals. A table in four places is a table
that disagrees with itself, and the way that failure presents is the worst kind:
the build succeeds, the app installs, and the guest is described wrongly to
itself.

So this is the table, and the shells read it the same way the generator does:

    python3 targets.py <target> <field>      # one field, bare
    python3 targets.py <target>              # every field, as shell assignments
    python3 targets.py --list
    python3 targets.py --c-table             # ...and the RUNTIME's copy, generated

The last one is the fifth consumer and the reason this file grew `entry` and
`kind`: the runtime needs the same table (which tree, which APK, which library
the chain starts at, which boot sequence), and it needed it twice — once in
`visionos/Sources/kl_app.c` for the app and once in `mains/m_boot.c` for the
host driver. Two hand-written C copies of a Python table is the exact failure
this file's header warns about, so `make targets` generates
`runtime/kl_target_table.h` from here instead. It is committed, like
`kl_libc_table.h` and `kl_jni_slots.h`, so a build never depends on Python.

Two apps built from this tree must not collide, and a bundle ID separates none
of the things that would. So the target also decides
the PRODUCT name — which carries the .xcodeproj, the .app and the derived-data
directory with it — and the subdirectory the translated guest frameworks are
staged into. ANGLE is deliberately NOT per-target: it is the same renderer for
every guest and `mkangle.sh` writes it once, into Frameworks/ itself.
"""
import sys
import shlex

TARGETS = {
    "beatsaber": {
        # Unity + IL2CPP + Oculus Mobile SDK. The chain is staged: libmain
        # dlopens libunity, which dlopens the rest — but every one is embedded,
        # because kl_load_auto resolves a DT_NEEDED against Frameworks/ and does
        # not care who asked.
        #
        # **This is 1.40.6 (build 1661, package com.beatgames.beatpaber), and
        # 1.28 is RETIRED.** It was 1.28 for the whole of this project's life;
        # 1.40 renders, plays and finds the DLC, and the tester's answer to keeping
        # both was "1.28 can go, not needed anymore that 1.40 works". So this is
        # a repoint rather than a second row, and the repoint is the point: an
        # installed app is a BUNDLE ID, and the id here is derived from the
        # target NAME (see bundle_for). Keeping the name `beatsaber` is what
        # makes the next install an UPGRADE of the Beat Saber already on the
        # Home View instead of a second icon beside it. A `beatsaber140` row
        # would have been a different name, a different id and a different app.
        # The 1.28 tree and apk are still on disk; nothing points at them, and
        # restoring them means restoring a build the tester has finished with.
        #
        # It is a SPLIT APPLICATION BINARY, which 1.28 was not. The APK holds
        # ~10 MB of assets/bin/Data stub and the game is
        # main.1661.com.beatgames.beatpaber.obb, 1.34 GB beside it. The guest
        # builds that FILENAME out of its own versionCode and package name, both
        # of which it reads from apktool.yml and AndroidManifest.xml — so those
        # two files being staged beside assets/ is as load-bearing as the OBB
        # itself (stage_assets.sh says what the silence looks like otherwise).
        #
        # DISCOVERED, not pinned (None asks the Makefile — see libs_for below).
        # It used to name five libraries, which was the whole of 1.28; 1.40
        # ships eleven translatable ones and the pinned list silently left
        # libOculusXRPlugin — the XR-SDK display provider, i.e. the entire
        # render path — out of the bundle. On device that is not a fallback but
        # a dead end: the ELF tree is deliberately not in the bundle, so a
        # library with no translation cannot load at all. Same lesson as
        # `4dc27b1`: the guest library set is a property of the APK.
        "libs":    None,
        "srcdir":  "beatsaber/lib/arm64-v8a",
        "tree":    "beatsaber",
        "apk":     "beatsaber.apk",
        "assets":  "beatsaber/assets",
        # Nothing in this guest reads a library as a FILE, so no ELF goes into
        # the container at all. See steamlink-vr's `qtplugins`.
        "qtplugins": "",
        # Where this guest looks for its OBB, relative to the external-storage
        # root (kl_jni_files_dir()). Unity asks Java for it —
        # Context.getObbDirs() answers <files>/obb — so every Unity target here
        # shares one layout and only the version code in the NAME differs.
        "obb":     "obb",
        # The library the chain STARTS at — libmain's JNI_OnLoad is the whole
        # entry point for a Unity title — and, on device, the one whose translation being
        # present proves this target's guest was embedded rather than the other
        # app's.
        "entry":   "libmain",
        # Which boot sequence runs. "unity" is libmain -> NativeLoader.load ->
        # libunity -> the Android lifecycle; "steamlink" is kl_slink's doors.
        "kind":    "unity",
        "product": "Klepton",
        "display": "Beat Saber",
        # mkicon.py's source art and back-layer colour (the brief: black).
        "icon":     "icons/beat saber.ico",
        "iconback": "0,0,0",
        # The house style, both halves of it. Beat Saber draws an opaque world
        # the player stands inside — .mixed would composite the room through a
        # sky it does not have — and it puts sabers in the player's hands, so
        # the system's own upper-limb rendering arrives as a second pair in the
        # same place. The tester, on the first 1.40 build that rendered: "MY HANDS
        # are visible … very distracting in game", then "NO HANDS visible while
        # in the app. exactly how every other port is."
        "immersion": "full",
        "own_hands": True,
    },
    "superhot": {
        # SUPERHOT VR — Unity + IL2CPP + the Oculus Mobile SDK, i.e. the same
        # shape as Beat Saber and the reason it is the third target: it shares
        # the entire Unity path and exercises it against an APK nothing here was
        # written against. Its own manifest declares the same
        # com.unity3d.player.UnityPlayerActivity and the same VR intent
        # category, so nothing about the front door differs.
        # 
        # Not a split-binary build: the assets are in the APK, so there is no
        # OBB to stage (stage_assets.sh treats one as present-or-absent).
        "libs":    None,
        "srcdir":  "superhot/lib/arm64-v8a",
        "tree":    "superhot",
        "apk":     "superhot.apk",
        "assets":  "superhot/assets",
        "qtplugins": "",
        # Where this guest looks for its OBB, relative to the external-storage
        # root (kl_jni_files_dir()). Unity asks Java for it —
        # Context.getObbDirs() answers <files>/obb — so every Unity target here
        # shares one layout and only the version code in the NAME differs.
        "obb":     "obb",
        "entry":   "libmain",
        "kind":    "unity",
        "product": "KleptonSuperhot",
        "display": "Klepton SUPERHOT",
        # The key art (red on transparent) on the game's signature white —
        # mark on Front so it is the layer that pops under gaze.
        "icon":     "icons/superhotvr.ico",
        "iconback": "255,255,255",
        # The style the immersive space OPENS in, declared in the scene
        # manifest. Full for superhot: its guest draws an opaque world the
        # player is inside, and .mixed composites the room into it. The
        # runtime half (opaque_world) is derived from this key in resolve().
        "immersion": "full",
        # It models the player's hands itself (and puts guns in them), so the
        # system's own upper-limb rendering is a second pair of hands.
        "own_hands": True,
    },
    "bonelab": {
        # BONELAB — Unity 2021.3.16f1 + IL2CPP, and the same front door again
        # (com.unity3d.player.UnityPlayerActivity, the Oculus VR intent
        # category). Two things make it the fourth target rather than a fourth
        # copy of the third:
        # 
        #  - it is a SPLIT APPLICATION BINARY with TWO obbs. Beat Saber 1.40
        #     taught this project that a guest's data can live beside the APK;
        #     this one ships main.2974 AND patch.2974, 6.8 GB together, and the
        #     patch is where the UnitySubsystems manifests and the whole
        #     Addressables catalogue are. Nothing here pins "main" — the obb
        #     directory is staged wholesale, so a guest that reads a patch reads
        #     one (see stage_assets.sh).
        #   - its boot.config takes the XR SDK path (`xrsdk-pre-init-library=
        #     OculusXRPlugin`, `xr-meta-enabled=1`), which is 1.40's path and not
        #     SUPERHOT's legacy `vr-device-list=Oculus`. It also SHIPS
        #     libopenxr_loader and libMicrosoftOpenXRPlugin, which the pre-init
        #     line says it does not use — so a run that ends up in OpenXR is a
        #     run that fell out of the Oculus path, and that is worth reading as
        #     a symptom rather than as a second front door.
        # 
        # libSLZQuestNative and libRF_CNative_andr are the title's own; neither
        # is replaced, so both translate like any other guest library.
        "libs":    None,
        "srcdir":  "bonelab/lib/arm64-v8a",
        "tree":    "bonelab",
        "apk":     "bonelab.apk",
        "assets":  "bonelab/assets",
        "qtplugins": "",
        # Where this guest looks for its OBB, relative to the external-storage
        # root (kl_jni_files_dir()). Unity asks Java for it —
        # Context.getObbDirs() answers <files>/obb — so every Unity target here
        # shares one layout and only the version code in the NAME differs.
        "obb":     "obb",
        "entry":   "libmain",
        "kind":    "unity",
        "product": "KleptonBonelab",
        "display": "BONELAB",
    },
    "vrchat": {
        # VRChat — Unity 2022.3.22f2-DWR + IL2CPP, and the FIFTH target. It is
        # the first guest here that is not an Oculus title: this is the STEAM
        # FRAME build, so there is no libOVRPlugin, no libvrapi and no Oculus
        # Platform loader anywhere in it. It speaks OPENXR, through the stack
        # Unity ships for it — libopenxr_loader (the Khronos loader),
        # libUnityOpenXR (the XR SDK provider) and libUnityOpenXRHands — and
        # boot.config says so in one line: `xrsdk-pre-init-library=UnityOpenXR`,
        # where BONELAB's says OculusXRPlugin.
        # 
        # That makes it the target that joins the project's two halves: the
        # Unity/IL2CPP path (Beat Saber, SUPERHOT, BONELAB) and the OpenXR
        # runtime written for Steam Link (runtime/xr/kl_openxr.c), which until now
        # only ever had a non-Unity guest on top of it.
        # 
        # The front door is stock — com.unity3d.player.UnityPlayerActivity and
        # libmain's JNI_OnLoad — even though the manifest names an OBFUSCATED
        # Application class (aGGhd3.kN5sj1.jrtzH2.r3jyW1), which is the title's
        # own and loads libloader.so from Java. We do not run Java, so nothing
        # here depends on that library; if something ends up demanding it, that
        # is a symptom to read rather than a step to pre-empt.
        # 
        # Not a split binary: 317 MB of APK with assets/bin/Data inside it and
        # no OBB beside it. libil2cpp.so alone is 297 MB.
        "libs":    None,
        "srcdir":  "vrchat/lib/arm64-v8a",
        "tree":    "vrchat",
        "apk":     "vrchat.apk",
        "assets":  "vrchat/assets",
        "qtplugins": "",
        # Where this guest looks for its OBB, relative to the external-storage
        # root (kl_jni_files_dir()). Unity asks Java for it —
        # Context.getObbDirs() answers <files>/obb — so every Unity target here
        # shares one layout and only the version code in the NAME differs.
        "obb":     "obb",
        "entry":   "libmain",
        "kind":    "unity",
        "product": "KleptonVRChat",
        "display": "VRChat",
    },
    "openbrush": {
        # Open Brush — Unity 2022.3.62f2 + IL2CPP, and the SIXTH target. Two
        # things make it worth adding rather than a sixth copy of the Unity row:
        # 
        #  - it is the **OPENXR build**, deliberately, and not the Oculus store
        #     one. Open Brush ships both; this tree is the former, which
        #     boot.config says in the same line VRChat's does
        #     (`xrsdk-pre-init-library=UnityOpenXR`) and the manifest says again
        #     in the Khronos permissions and the runtime-broker `<queries>`.
        #     There is no libOVRPlugin, no libvrapi and no Oculus Platform
        #     loader — so it takes the Unity + kl_openxr path VRChat opened, and
        #     it is the SECOND guest on it, which is the point: until now that
        #     junction had exactly one title holding it up.
        #   - **the sources are public** (github.com/icosa-foundation/open-brush),
        #     which no other target here has. Everything else in this project is
        #     reverse-engineered from a stripped .so, so when this one does
        #     something inexplicable the answer can be READ instead of derived.
        #     Nothing in the build depends on that; it is a debugging lever.
        # 
        # The front door is stock — com.unity3d.player.UnityPlayerActivity and
        # libmain's JNI_OnLoad — with no obfuscated Application class of the kind
        # VRChat carries.
        # 
        # Not a split binary: 242 MB of APK with assets/bin/Data AND the
        # Addressables catalogue (assets/aa) inside it, so there is no OBB to
        # stage.
        # 
        # UNSETTLED and the first thing a run has to answer: the manifest
        # declares `android.hardware.vulkan.version` as REQUIRED. That is
        # BONELAB's question, and BONELAB's recipe answers it — whether the guest
        # dlopens libvulkan.so, and which EGL entry points it reaches
        # (KL_EGL_TRACE=1). Unlike BONELAB's day, either answer is now a path
        # that exists; which one this is decides whether the eye textures arrive
        # through kl_glfb or kl_vulkan.
        "libs":    None,
        "srcdir":  "openbrush/lib/arm64-v8a",
        "tree":    "openbrush",
        "apk":     "openbrush.apk",
        "assets":  "openbrush/assets",
        "qtplugins": "",
        # Where this guest looks for its OBB, relative to the external-storage
        # root (kl_jni_files_dir()). Unity asks Java for it —
        # Context.getObbDirs() answers <files>/obb — so every Unity target here
        # shares one layout and only the version code in the NAME differs.
        "obb":     "obb",
        "entry":   "libmain",
        "kind":    "unity",
        "product": "KleptonOpenBrush",
        "display": "Open Brush",
    },
    "re4": {
        # Resident Evil 4 VR — and the SEVENTH target, which is the first one
        # here that is not a Unity game and not Steam Link. It is **UNREAL
        # ENGINE 4.25.3** (branch ++VR4+VR4), a Quest exclusive, package
        # com.Armature.VR4 — the STOCK build, versionCode 201 per the manifest
        # (the earlier VRLOC.com.Armature.VR4 mod dump is gone; the package is
        # IN the OBB path UE4 builds, so this is not cosmetic).
        #
        # That is the whole reason to add it. Every engine-shaped thing this
        # project knows was learned from Unity: five of the six targets are
        # Unity + IL2CPP, and the sixth (Steam Link) is not a game engine at
        # all. So a whole half of the shim has only ever been exercised in one
        # dialect, and the parts that are genuinely Android rather than Unity
        # have never been asked a second opinion.
        # 
        # Three things differ at the door, all measured from the APK:
        # 
        #  - **the entry is a NativeActivity.** libUE4.so exports
        #     ANativeActivity_onCreate, android_main AND JNI_OnLoad; the
        #     manifest names com.epicgames.ue4.GameActivity with
        #     `android.app.lib_name = UE4`. So the guest is STARTED through the
        #     NDK's activity door and driven by callbacks it fills in, where a
        #     Unity guest is started through libmain's JNI_OnLoad and driven by
        #     calling natives it registered. Only Steam Link's VR door has ever
        #     used this path, and that is one non-Unity library rather than an
        #     engine.
        #   - **input arrives as an AInputQueue.** AInputEvent / AKeyEvent /
        #     AMotionEvent are in the unresolved set, and Beat Saber uses
        #     none of them: Unity takes its input over JNI. This is the NDK
        #     surface nothing has needed yet.
        #   - **assets come through AAssetManager**, not over JNI —
        #     AAsset_getBuffer and AAsset_openFileDescriptor are both
        #     unresolved. The same note records that Unity reads its assets
        #     through Context.getAssets() and a Java InputStream instead.
        #
        # A split application binary with TWO obbs, BONELAB's shape: main.201
        # and patch.201 (versionCode 201, per the manifest), 8.5 GB together,
        # staged wholesale.
        #
        # XR is VrApi/OVRPlugin — both libraries ship — and NEITHER is in
        # libUE4's DT_NEEDED, so both are dlopen'd and kl_ovrp claims them the
        # way it always has. `bSupportsVulkan` is true in the manifest and
        # there is no libvulkan in the link either, so which graphics API this
        # takes is a question for the first run rather than the table.
        # 
        # libfrda.so, libscript.so and libfrda.config.so are third-party
        # instrumentation libraries, not part of the application, and are not
        # loaded. libscript.so and libfrda.config.so are not ELF objects. They
        # are excluded by the Makefile's GUEST_EXCLUDED rather than here
        # (`libs: None` asks the Makefile, which is the one place those rules
        # live), and they would be inert regardless: the libraries they target
        # (ovrplatformloader, vrapi, libc) are the ones this project REPLACES.
        # The DRM line is unchanged and is kl_ovrplat's — the app's own
        # entitlement is answered on the premise the user owns the title, and
        # everything that delivers content still refuses.
        "libs":    None,
        "srcdir":  "re4/lib/arm64-v8a",
        "tree":    "re4",
        "apk":     "re4.apk",
        "assets":  "re4/assets",
        "qtplugins": "",
        # Where this guest looks for its OBB, relative to the external-storage
        # root. **UE4 builds the path ITSELF** and it is ANDROID's layout, not
        # the flat one the Unity guests use: <external>/Android/obb/<package>/
        # main.<versionCode>.<package>.obb. Nothing here calls getObbDirs(), so
        # staging into <files>/obb/ puts 8.5 GB somewhere the engine never looks
        # — and it fails SILENTLY, because a UE4 build reports a missing OBB the
        # same way it reports a missing .uproject: it carries on.
        "obb":     "Android/obb/com.Armature.VR4",
        # The library the chain starts at, and the one whose translation being
        # in the bundle proves THIS guest was embedded. There is no libmain
        # here: UE4 links its engine, its game and its plugins into one 172 MB
        # object and the manifest names it by `android.app.lib_name`.
        "entry":   "libUE4",
        "kind":    "ue4",
        "product": "KleptonRE4",
        "display": "Resident Evil 4",
        # mkicon.py's source art and back-layer colour (the brief: black).
        "icon":     "icons/RE4.png",
        "iconback": "0,0,0",
        # It models the player's hands itself, so the system's own upper-limb
        # rendering is a second pair of hands (same shape as superhot's).
        "own_hands": True,
        # KL_OVRP_EYE_ALIGN=4: UE4 4.25 quantizes its scene buffers UP to a
        # multiple of 4, so the capped 4032x3234 eye got a 4032x3236 scene and
        # every exact-extent eye-pass match in kl_vulkan.c missed (foveation
        # marked 0 of 15193 encoders). The eye is now 4032x3232 and the scene
        # buffer equals it. Structural, not a tunable — it stays baked whatever
        # foveation does. Foveation itself is not baked for this title; set
        # KL_FOVEATION in the device env file to try it.
        "env": "KL_OVRP_EYE_ALIGN=4",
    },
    "tetris": {
        # Tetris Effect: Connected — Enhance's com.enhanceexperience.tetriseffect,
        # versionCode 157 / versionName 2.0.2, and the THIRD Unreal target after
        # re4 (4.25.3) and deadpool (4.27). This one is **UE4.26.2, branch
        # ++UE4+Partner-Oculus-4.26** (manifest EngineVersion/EngineBranch),
        # project TetrisEffect, Shipping, CookedFlavors=ASTC.
        #
        # The front door is re4's and deadpool's exactly: the manifest names
        # com.epicgames.ue4.GameActivity with `android.app.lib_name = UE4`, and
        # libUE4.so (189 MB) exports ANativeActivity_onCreate, android_main and
        # JNI_OnLoad. So the entry is libUE4 and the boot is the NDK activity
        # door, not a JNI_OnLoad chain.
        #
        # **THE XR DOOR IS re4's, NOT deadpool's — VrApi/OVRPlugin.** Measured,
        # because libUE4 carries the code for BOTH plugins and the symbol table
        # alone does not decide it (198 FOpenXRHMD symbols AND 614 OculusHMD
        # ones are defined; libUE4 imports neither `vrapi_*` nor `xr*`, so
        # whichever runs is dlopen'd/dlsym'd at runtime). Three things settle
        # it, all read off this APK:
        #
        #  - **the payload decides.** lib/arm64-v8a ships libOVRPlugin.so AND
        #     libvrapi.so and there is NO libopenxr_loader.so anywhere in the
        #     tree. deadpool's evidence was the mirror image of this.
        #   - **the Java loads the legacy pair by name.** GameActivity.smali
        #     System.loadLibrary()s, in order: ovrplatformloader, vrapi,
        #     OVRPlugin, EOSSDK, ovrplatformloader, c++_shared — with the log
        #     strings "vrapi library not loaded and may be required for Oculus
        #     VR." and "OVRPlugin library not loaded and may be required..."
        #     beside them. No openxr_loader load anywhere in the class.
        #   - **the engine's own enum names it**: the only OVRPlugin-ish string
        #     in libUE4 is `EOculusXrApi::LegacyOVRPlugin` — 4.26's Oculus
        #     plugin's two-valued API selector — and 240 `ovrp_*` dlsym name
        #     strings sit beside it.
        #
        # So kl_ovrp claims both libraries the way it always has, and
        # runtime/xr/kl_openxr.c is not on this guest's path. The FOpenXRHMD
        # half of libUE4 is dead code in this build.
        #
        # GLES, not Vulkan, and stated in the manifest rather than inferred:
        # `bSupportsVulkan = false` (re4's was true and left the question open),
        # glEsVersion 0x30001, DT_NEEDED libGLESv3/libEGL, no libvulkan.
        #
        # ONE OBB, not re4's split pair: main.157.<package>.obb, 2.1 GB.
        # `bHasOBBFiles=true`, `bPackageDataInsideApk=false`,
        # `bUseExternalFilesDir=false`. assets/ is 660 KB — the UE4 command line
        # (`../../../TetrisEffect/TetrisEffect.uproject`) and Google's
        # memoryadvice json, nothing else.
        #
        # The same excluded libraries as re4 and deadpool, not loaded:
        # libfrda.so, libscript.so and libfrda.config.so (third-party
        # instrumentation; the last two are not ELF). Verified
        # against THIS tree: `make guestlibs LIBS=tetris/lib/arm64-v8a` reports
        # excluded=libfrda, "not ELF"=libfrda.config libscript, so the
        # Makefile's generic GUEST_EXCLUDED plus the ELF sniff already discover
        # none of the three and no Makefile change was needed. They would be
        # inert regardless — the libraries they target are the ones this
        # project REPLACES (guestlibs also reports replaced =
        # libovrplatformloader libOVRPlugin libvrapi). The DRM line is
        # unchanged and is kl_ovrplat's.
        #
        # 34 libraries translate, and the tail is an audio stack no target here
        # has met: Wwise (25 × libAk*), libAuro, libiZotope, libMcDSP,
        # libMasteringSuite. Plus libEOSSDK (21 MB — Epic Online Services, in
        # libUE4's DT_NEEDED), libplaycore and libtry-alloc-lib.
        "libs":    None,
        "srcdir":  "tetris/lib/arm64-v8a",
        "tree":    "tetris",
        "apk":     "tetris.apk",
        "assets":  "tetris/assets",
        "qtplugins": "",
        # UE4 builds the path ITSELF, Android's layout — see re4's note. The
        # same relative path is the SOURCE under userdata/tetris/ for a host
        # run, so main.157.com.enhanceexperience.tetriseffect.obb goes at
        # userdata/tetris/Android/obb/com.enhanceexperience.tetriseffect/.
        "obb":     "Android/obb/com.enhanceexperience.tetriseffect",
        # One 189 MB object holding engine, game and plugins, named by the
        # manifest's `android.app.lib_name`. There is no libmain.
        "entry":   "libUE4",
        "kind":    "ue4",
        "product": "KleptonTetris",
        # The game's name, not the runtime's — the re4/deadpool pattern.
        "display": "Tetris Effect",
        # mkicon.py's source art and back-layer colour. The r4 device build is
        # what retired the TODO that used to sit here: mkicon.py has NO
        # fallback for a row with no `icon`, so the first device build died in
        # step 1/5 with `KeyError: 'icon'` — the "placeholder path" the old
        # note assumed does not exist. (A missing source file is now a warning,
        # not a failure: the art lives in the gitignored icons/ directory.)
        # Black back, since the game is neon on black.
        "icon":     "icons/tetris.ico",
        "iconback": "0,0,0",
        # 4.26's GameActivity passes Build.VERSION.SDK_INT as the SECOND
        # argument of nativeSetAndroidVersionInformation, deadpool's (4.27)
        # spelling rather than re4's (4.25, five bare strings). Verified in
        # this APK's GameActivity.smali: the native is declared
        # `(Ljava/lang/String;ILjava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V`
        # and the call site passes Build.VERSION.RELEASE, the int, then
        # MANUFACTURER / MODEL / DISPLAY.
        "ue4_sdk_int": True,
        # It models the player's hands itself (same shape as re4's and
        # deadpool's) — PROVISIONAL, set from the engine family rather than
        # from a worn round, and the first in-headset round is what confirms
        # or clears it.
        "own_hands": True,
    },
    "hl2vr": {
        # Half-Life 2 VR (Standalone 0.92) — and the EIGHTH target, the first
        # one here that is neither a managed engine nor an app with a VR door
        # bolted on: this is nillerusr's **srceng** Quest port, Valve's Source
        # engine (package com.valvesoftware.source.vrtest, versionCode 920)
        # built as thirty plain C++ .so's — libengine, libserver, libclient,
        # vgui, tier0 — with no IL2CPP, no OVRPlugin and no Java glue of the
        # game's own beyond one activity.
        #
        # The front door is **SDL2**: the manifest names
        # org.libsdl.app.SDLActivity, getLibraries() answers {"SDL2",
        # "launcher"}, and getMainFunction() answers "LauncherMainAndroid".
        # So the boot is SDL's convention rather than Unity's or UE4's —
        # libSDL2's JNI_OnLoad registers the SDLActivity natives, and
        # nativeRunMain dlopens liblauncher.so and calls LauncherMainAndroid
        # the way SDL2 calls any SDL_main. The arguments do NOT arrive through
        # that call: ValveActivity2.initNatives (the game's one Java class)
        # pokes them in ahead of time through liblauncher's own exports —
        # setenv("VALVE_GAME_PATH", ...) and setArgs("-game hl2 -vr ...") —
        # which m_boot's SDL door transcribes rather than re-invents.
        #
        # XR is **OpenXR through dlopen("libopenxr_loader.so")**, claimed by
        # kl_openxr's interposition (kl_openxr_dlopen) by name — the same seam
        # VRChat and Open Brush already take, with one difference: this tree
        # SHIPS libopenxr_loader.so in lib/arm64-v8a, and it stays in the
        # translated set (libs: None asks the Makefile, whose GUEST_REPLACED
        # covers the Oculus trio only). That is deliberate and matches the
        # other OpenXR rows: the interposition wins the dlopen by name
        # regardless, so the embedded copy is inert weight, not a second door.
        #
        # NOT OBB-shaped: there is no .obb anywhere in this title's world and
        # nothing in the guest calls getObbDirs(), so the `obb` value below is
        # inert — stage_assets.sh finds no *.obb and stages none. The game
        # data is a DIRECTORY TREE at a fixed external-storage path instead,
        # CONFIRMED on-device: the port's game_cache/hl2q3vr-cache.json
        # records "headset_destination": "/sdcard/srceng".
        # `gamedata` below points at the staged tree (game_cache/srceng —
        # hl2/ and platform/, ~4.1 GB); stage_assets.sh pushes it to
        # <container>/android-files/srceng, which is where /sdcard maps on
        # device (kl_guest_extstorage_map onto the files dir, same as the
        # host's userdata mapping the recon symlink uses). Optionally
        # alyx_remix_q3_mobile_002/ (VPKs, a mod dir) can ride beside it — not
        # staged, and the game needs no argument changes without it.
        "libs":    None,
        "srcdir":  "hl2vr/lib/arm64-v8a",
        "tree":    "hl2vr",
        "apk":     "hl2vr.apk",
        "assets":  "hl2vr/assets",
        # Nothing in this guest reads a library as a FILE (that is Steam
        # Link's Qt plugin story), so no ELF goes into the container.
        "qtplugins": "",
        # Inert — see the row comment. Not "srceng": that would tell
        # getObbDirs() to answer the game-data directory, and a guest that
        # never asks should not be handed a wrong answer it never reads.
        "obb":     "obb",
        # The library SDL's nativeRunMain dlopens — the chain STARTS at
        # libSDL2 (its JNI_OnLoad is the SDL2 entry), but liblauncher is the
        # one whose translation being in the bundle proves THIS guest was
        # embedded, and the one the entry function is dlsym'd from.
        "entry":   "liblauncher",
        # ...and the function nativeRunMain dlsym's out of it — ValveActivity2
        # overrides SDLActivity.getMainFunction() to name it. The second SDL
        # guest (simpsons) overrides nothing and takes SDL's own "SDL_main",
        # which is why this is a table field rather than a constant in the door.
        "sdl_main": "LauncherMainAndroid",
        # The first guest of its kind: an SDL2 front door (see kl_target.h's
        # KL_GUEST_SDL) rather than a Unity JNI_OnLoad chain or UE4's
        # NativeActivity.
        "kind":    "sdl",
        "product": "KleptonHL2VR",
        "display": "Half-Life 2 VR",
        # mkicon.py's source art and back-layer colour (the brief: black).
        "icon":     "icons/hl2vr.ico",
        "iconback": "0,0,0",
        # The /sdcard/srceng game tree comes from
        # userdata/hl2vr/srceng (stage_assets.sh's default for a row with a
        # `gamedest`); `gamedata` or KL_GAMEDATA_SRC names another directory.
        # ...and the directory name it lands under on the guest's side of
        # /sdcard. Was a literal inside stage_assets.sh until the second
        # gamedata target arrived; the guest hardcodes it (ValveActivity2's
        # smali), so the table is where the two staging paths agree.
        "gamedest": "srceng",
        # Source draws an opaque world the player stands inside — .mixed would
        # composite the room through it — and the VR mod models the player's
        # hands and weapons itself, so the system's own upper-limb rendering
        # is a second pair (same shape as superhot's).
        "immersion": "full",
        "own_hands": True,
    },
    "simpsons": {
        # The Simpsons: Hit & Run VR — kote2345's Quest port of the SHAR
        # decompilation (package com.simpsonsHitAndRun.vr, versionCode 93 /
        # versionName 0.93, APK named 1.2), and the SECOND guest on the SDL2
        # door after hl2vr. It is here because it is the door's first
        # confirmation that the kind names the DOOR and not the engine: nothing
        # about Radical's 2003 engine resembles Source, and the boot sequence is
        # byte-for-byte SDLActivity's all the same.
        #
        # THE FRONT DOOR, read off this APK's own smali:
        # com.simpsonsHitAndRun.vr.SimpsonsActivity extends
        # org.libsdl.app.SDLActivity and overrides exactly two methods —
        # onCreate (super, then a MANAGE_EXTERNAL_STORAGE permission prompt) and
        # onDestroy (Process.killProcess). It does NOT override getLibraries(),
        # getMainFunction() or getArguments(), so all three are stock SDL:
        # {"SDL2", "main"}, "SDL_main", and an EMPTY argument array. That is the
        # whole difference from hl2vr, whose ValveActivity2 overrides the lot —
        # hence `entry: libmain` and `sdl_main: SDL_main` here against
        # liblauncher/LauncherMainAndroid there, and no env and no setArgs at
        # all on this row (kl_sdl.c pokes ValveActivity2's environment only for
        # the guest whose Java has it).
        #
        # XR is **OpenXR through dlopen("libopenxr_loader.so")**, hl2vr's seam
        # exactly, and this tree ships the loader in lib/arm64-v8a the same way:
        # kl_openxr_dlopen claims the dlopen by name and kl_sdl_load drops the
        # embedded copy from the roster. The guest asks for a SMALL extension
        # set — XR_KHR_android_create_instance, XR_KHR_opengl_es_enable,
        # XR_FB_color_space, XR_FB_display_refresh_rate — which is the whole
        # list `strings` finds in libmain.
        #
        # GLES3, stated in the manifest (`glEsVersion 0x30002`), and libmain
        # carries a **GL_OVR_multiview2 renderer** (SharOpenXR::BeginMultiview,
        # IsMultiviewAvailable, PrepareMultiviewCamera, and a shader source with
        # `#extension GL_OVR_multiview2 : require`). ANGLE-on-Metal has no
        # multiview, so the path should never engage — the guest gates it on its
        # own IsMultiviewAvailable() — but hl2vr's mono world is what that gate
        # failing looks like, so it is the first thing to read on launch #1.
        #
        # NOT OBB-shaped and not APK-shaped: the game DATA is the PC release's
        # own directory tree, and libmain names the path itself, three times, as
        # a literal — `/storage/emulated/0/SimpsonsHitRun`. So `gamedest` is
        # `SimpsonsHitRun` (one of the three external-storage spellings
        # kl_guest_path maps) and `gamedata` points at the PC folder on this
        # machine; the 50 MB APK's own assets/ is two touch-control PNGs and a
        # splash, nothing the VR build reads.
        "libs":    None,
        "srcdir":  "simpsons/lib/arm64-v8a",
        "tree":    "simpsons",
        "apk":     "simpsons.apk",
        "assets":  "simpsons/assets",
        # Nothing here reads a library as a FILE (Steam Link's Qt story).
        "qtplugins": "",
        # Inert, hl2vr's reason: nothing in this guest calls getObbDirs(), and
        # naming the game directory here would answer a question it never asks
        # with a path it would then be wrong about.
        "obb":     "obb",
        # SDLActivity.getLibraries()'s second entry, spelled for the loader:
        # "main" -> libmain.so, which is also the 8.5 MB object that IS the game
        # (engine, renderer and the OpenXR layer, one library).
        "entry":   "libmain",
        # ...and the function nativeRunMain dlsym's out of it. Stock SDL's,
        # because SimpsonsActivity overrides nothing. libmain exports it.
        "sdl_main": "SDL_main",
        "kind":    "sdl",
        "product": "KleptonSimpsons",
        "display": "Simpsons Hit & Run",
        # Custom artwork, which replaced the APK extraction. The .ico carries
        # six sizes and mkicon.py asks for the largest
        # by name — plain Image.open() on an .ico yields whichever entry is
        # first, which is usually the 16x16.
        "icon":     "icons/simpsons.ico",
        # ...and the ground is an IMAGE rather than a colour, which is what
        # `iconback` grew the ability to take for this row. The poster art
        # The tester supplied (`simpsons-bg.png`, 600x900) is a blue vertical
        # gradient — (17,104,171) at the top to (47,191,255) at the bottom —
        # with the H&R logo already drawn on it. The logo is the FRONT layer's
        # job, so the ground here is `simpsons-back.png`: that same gradient
        # alone, built by stretching the poster's own untouched left column to
        # 1024 square, so the mark is not drawn twice. A flat mid-blue was the
        # alternative and reads as a sticker where the gradient reads as a
        # badge.
        "iconback": "icons/simpsons-back.png",
        # The PC game files, which the user owns and supplies. 1.9 GB: art/,
        # movies/, scripts/, sound/, the .rcf archives and simpsons.ini. The
        # port's README says to put exactly this at <storage root>/SimpsonsHitRun.
        # Staged from userdata/simpsons/SimpsonsHitRun by default (see
        # `gamedest`); `gamedata` or KL_GAMEDATA_SRC names another directory.
        # ...where it lands on the guest's side. See the row comment.
        "gamedest": "SimpsonsHitRun",
        # A driving game in a solid Springfield: .mixed would composite the room
        # through the world.
        "immersion": "full",
        # TRUE, and the name undersells what the field does: it is the scene's
        # `.upperLimbVisibility`, i.e. whether the SYSTEM draws the wearer's
        # real arms over the guest's picture (KleptonApp.swift's
        # Immersive.upperLimbs — `own_hands` non-zero means `.hidden`).
        #
        # It was False for rounds 1-4 on the reasoning that this port models no
        # hands of its own, so the real ones would be harmless. The tester, launch
        # #4: they are not. The player is INSIDE Springfield — driving, or on
        # foot in a first-person body — and a pair of real arms composited over
        # an opaque world is the same defect `immersion: full` exists to
        # prevent, just at limb scale rather than room scale. A guest drawing no
        # hands is a reason the system's hands have nothing to collide WITH, not
        # a reason to show them.
        #
        # KL_HANDS=1 puts them back for a round if the guest ever grows hands
        # worth comparing against.
        "own_hands": True,
    },
    "redmatter2": {
        # Red Matter 2 (Vertical Robot), package com.VerticalRobot.RedMatter2 —
        # UE4 through com.epicgames.ue4.GameActivity, RE4/wrath2's shape. Audio
        # is the Oculus spatializer alone (libovraudio64 in DT_NEEDED, no FMOD,
        # no Wwise), which the optional-if-absent chain handles by skipping the
        # FMOD members. No excluded libraries; `libs: None` finds nothing to strip.
        "libs":    None,
        "srcdir":  "redmatter2/lib/arm64-v8a",
        "tree":    "redmatter2",
        "apk":     "redmatter2.apk",
        "assets":  "redmatter2/assets",
        "qtplugins": "",
        # UE4 builds <external>/Android/obb/<package>/ itself — RE4's layout.
        "obb":     "Android/obb/com.VerticalRobot.RedMatter2",
        "entry":   "libUE4",
        "kind":    "ue4",
        "product": "KleptonRedMatter2",
        "display": "Red Matter 2",
        # UE 4.26+: nativeSetAndroidVersionInformation takes the int
        # TargetSDKVersion (upstream defaulted every UE4 title but re4 to it).
        "ue4_sdk_int": True,
        # Upstream answered com.oculus.vulkan.version (KL_UE4_VULKAN) with
        # VK_MAKE_VERSION(1,1,0) for every UE title and this row ran that way;
        # the runtime's default for a UE4 guest is now 1.0.3 (1.1.128 for UE5),
        # so the row keeps the value it was run with.
        "env": "KL_UE4_VULKAN=0x401000",
    },
    "zix": {
        # ZIX (Hidden Gem), package com.HiddenGemIO.ZIX — Unity + IL2CPP, the
        # libmain front door. Two things worth knowing before its first run:
        # audio is Wwise (libAkUnitySoundEngine), and XR ships BOTH stacks —
        # libOVRPlugin next to libUnityOpenXR/libopenxr_loader — so which path
        # the boot takes is worth reading out of the log rather than assuming.
        # EOS (libEOSSDK) is aboard for the online half.
        "libs":    None,
        "srcdir":  "zix/lib/arm64-v8a",
        "tree":    "zix",
        "apk":     "zix.apk",
        "assets":  "zix/assets",
        "qtplugins": "",
        # Flat Unity layout: Context.getObbDirs() answers <files>/obb.
        "obb":     "obb",
        "entry":   "libmain",
        "kind":    "unity",
        "product": "KleptonZIX",
        "display": "ZIX",
    },
    "vampire": {
        # Vampire: The Masquerade — Justice (Fast Travel Games), package
        # com.fasttravelgames.lollipop (the codename, not a typo) — UE4,
        # GameActivity door, wrath2's shape exactly: FMOD audio plus the Oculus
        # spatializer, playcore for the OBB half. libbhaptics-native-lib is a
        # vest-haptics SDK; nothing here needs to speak it for the game to run.
        "libs":    None,
        "srcdir":  "vampire/lib/arm64-v8a",
        "tree":    "vampire",
        "apk":     "vampire.apk",
        "assets":  "vampire/assets",
        "qtplugins": "",
        "obb":     "Android/obb/com.fasttravelgames.lollipop",
        "entry":   "libUE4",
        "kind":    "ue4",
        "product": "KleptonVampire",
        "display": "Vampire: The Masquerade - Justice",
        # UE 4.26+: nativeSetAndroidVersionInformation takes the int
        # TargetSDKVersion (upstream defaulted every UE4 title but re4 to it).
        "ue4_sdk_int": True,
        # Upstream answered com.oculus.vulkan.version (KL_UE4_VULKAN) with
        # VK_MAKE_VERSION(1,1,0) for every UE title and this row ran that way;
        # the runtime's default for a UE4 guest is now 1.0.3 (1.1.128 for UE5),
        # so the row keeps the value it was run with.
        "env": "KL_UE4_VULKAN=0x401000",
    },
    "vicecity": {
        # GTA Vice City VR (GTAVCVR) — a reVC/re3 (open-source re-implementation)
        # VR port. A plain Android NativeActivity + OpenXR native app: the entry is
        # ANativeActivity_onCreate in libmiamivr.so ("Miami" = Vice City) and the
        # game runs its own loop on the android_main thread, so it is the new
        # "native" kind, not Unity/UE4. No OBB and no assets/ inside the APK — the
        # whole data set is the loose files/gamedata tree (gta3.img, cuts.img,
        # txd, TEXT, Audio, reVC.ini, vr_settings.ini), staged through files/.
        "libs":    None,
        "srcdir":  "vicecity/lib/arm64-v8a",
        "tree":    "vicecity",
        "apk":     "vicecity.apk",
        # The APK ships no assets/; stage_assets.sh stages it present-or-absent.
        "assets":  "vicecity/assets",
        "qtplugins": "",
        # No OBB — the shared "nothing here" answer, as jkxr uses.
        "obb":     "obb",
        # The game data: userdata/vicecity/files/gamedata -> android-files/files.
        "stage_files": "files",
        "entry":   "libmiamivr",
        "kind":    "native",
        "product": "KleptonViceCity",
        "display": "Vice City",
        # In-world title: its menu is a full stereo scene too, so .mixed leaves
        # it a window floating in passthrough. (Upstream: kl_app_target_wants_full.)
        "immersion": "full",
        # It REQUIRES XR_KHR_vulkan_enable2 (it loops on the extension list without
        # it), which this runtime offers only on request — see kl_openxr.c.
        "env": "KL_XR_VULKAN2=1",
    },
    "intotheradius": {
        # Into the Radius (CM Games), package com.CMGames.IntoTheRadius — UE4
        # through com.epicgames.ue4.GameActivity, RE4/wrath2's shape. Split
        # binary (a DownloaderActivity fronts the OBB). Ships bhaptics haptics
        # and Sentry crash reporting (libsentry/libsentry-android) beside the
        # engine; neither is needed to run, and Sentry's native uploader is the
        # kind of thing to watch if a run dies right after it initialises.
        "libs":    None,
        "srcdir":  "intotheradius/lib/arm64-v8a",
        "tree":    "intotheradius",
        "apk":     "intotheradius.apk",
        "assets":  "intotheradius/assets",
        "qtplugins": "",
        # UE4 builds <external>/Android/obb/<package>/ itself — the Android
        # layout, like RE4/wrath2.
        "obb":     "Android/obb/com.CMGames.IntoTheRadius",
        "entry":   "libUE4",
        "kind":    "ue4",
        "product": "KleptonIntoTheRadius",
        "display": "Into the Radius",
        # UE 4.26+: nativeSetAndroidVersionInformation takes the int
        # TargetSDKVersion (upstream defaulted every UE4 title but re4 to it).
        "ue4_sdk_int": True,
        # Upstream answered com.oculus.vulkan.version (KL_UE4_VULKAN) with
        # VK_MAKE_VERSION(1,1,0) for every UE title and this row ran that way;
        # the runtime's default for a UE4 guest is now 1.0.3 (1.1.128 for UE5),
        # so the row keeps the value it was run with.
        "env": "KL_UE4_VULKAN=0x401000",
    },
    # ---- JKXR: ONE APK, TWO GAMES, TWO ROWS ----
    #
    # JKXR is Team Beef's VR port of Jedi Knight II: Jedi Outcast and Jedi
    # Knight: Jedi Academy, built on OpenJK. It is the EIGHTH target and the
    # smallest surface any of them has presented: ten guest libraries, 22 MB of
    # engine, and NINE unresolved imports across the whole tree (eight libc
    # names and one that resolves inside the chain).
    #
    # It is here because it is a THIRD engine and a second shape of front door,
    # and both are cheap:
    #
    #  - **the natives are static exports.** The manifest names
    #     com.drbeef.jkxr.GLES3JNIActivity, a plain Activity that is also a
    #     SurfaceHolder.Callback, and its nine natives are declared on
    #     com.drbeef.jkxr.GLES3JNILib and exported by both engines as
    #     `Java_com_drbeef_jkxr_GLES3JNILib_*`. So there is no RegisterNatives
    #     census to read and no NativeActivity glue: the driver stands in for
    #     the Java by calling those exports in the order the Activity calls
    #     them, and the handle onCreate returns is the argument to every one
    #     after it.
    #   - **OpenXR over GLES**, which is the pairing this project has only ever
    #     had under Steam Link. The engine's forty-odd xr* imports bind at
    #     relocation time through kl_shim's OpenXR tier, and the pixels go
    #     through libgl4es — a GL 1.x-to-GLES translator the port carries
    #     because the Quake 3 renderer is fixed-function. That makes gl4es, not
    #     the game, the thing on the far side of kl_egl.
    #
    # TWO ROWS OVER ONE TREE, which no other target here does. The APK ships
    # BOTH engines and both games' VR pk3s; the Java picks between them by
    # reading `/sdcard/JKXR/commandline.txt` and appending the result to
    # "openjk_", so which library is the entry is a property of a FILE rather
    # than of the APK. Splitting it into two rows is what gives each game its
    # own userdata directory, bundle id, .app and translated-guest directory —
    # which matters here more than usual, because the two games' data
    # directories are DIFFERENT (JK3/base vs JK2/base) and each holds retail
    # assets the other cannot read. Everything a row differs in is derived from
    # `entry`: kl_jkxr takes the token after "libopenjk_" and it names the
    # renderer, the game module, the data directory and the command line, so
    # the row and the file the guest reads cannot disagree.
    #
    # One consequence worth knowing: the two rows share a libdir, and
    # kl_target_resolve takes the FIRST row matching a path, so the path form
    # (`jkxr/lib/arm64-v8a`, which is what the Makefile gates pass) means
    # Academy. Name the target — `TARGET=jkxr_jo` — to get the other one.
    #
    # THE ASSETS ARE NOT IN THE APK and are not an OBB either. This is an engine
    # port, so it ships the VR-specific pk3s (28 MB, in assets/) and expects the
    # RETAIL GAME DATA beside them — assets0.pk3 and up, which the user owns and
    # supplies. The Java copies its own pk3s to <ext>/JKXR/JK3/base on first
    # run; kl_jkxr does that half, and KL_JKXR_DATA points once at an existing
    # install so it can link the retail half in beside them. Nothing here can
    # fabricate them, and the engine reports them missing the way Quake 3 always
    # has: it refuses to start.
    "jkxr_ja": {
        "libs":    None,
        "srcdir":  "jkxr/lib/arm64-v8a",
        "tree":    "jkxr",
        "apk":     "jkxr.apk",
        "assets":  "jkxr/assets",
        "qtplugins": "",
        # No OBB. The field is not optional and "obb" is what every non-UE4
        # target says, so it stays the shared answer rather than becoming a
        # third layout that means "there is nothing here".
        "obb":     "obb",
        # Jedi Academy. The token after "libopenjk_" is the whole of what this
        # row differs in — see the block comment above.
        "entry":   "libopenjk_ja",
        "kind":    "jkxr",
        "product": "KleptonJKXRAcademy",
        "display": "JK Academy",
    },
    "jkxr_jo": {
        "libs":    None,
        "srcdir":  "jkxr/lib/arm64-v8a",
        "tree":    "jkxr",
        "apk":     "jkxr.apk",
        "assets":  "jkxr/assets",
        "qtplugins": "",
        "obb":     "obb",
        # Jedi Outcast.
        "entry":   "libopenjk_jo",
        "kind":    "jkxr",
        "product": "KleptonJKXROutcast",
        "display": "JK Outcast",
    },
    "gorn2": {
        # GORN 2 (com.CortopiaStudios.Graves, v1.6.3/5089) — Unity 2022.3.60f1 +
        # IL2CPP + OVRPlugin 1.106.0, and the tree's THIRD Vulkan target (after
        # bonelab and walkabout). Vulkan is not a build-time flag here, same as
        # those two: the guest dlopens libvulkan.so, kl_vulkan.c stands MoltenVK
        # up, kl_vulkan_guest_active() flips the eye path over, and the only
        # per-target work is this row (immersion:full, own_hands, the fove
        # env). `mains/m_gorn.c` is the host front door; `gorn2/STATUS.md` is
        # the bring-up log.
        #
        # The library set is PINNED (not None auto-discovery) for the same class
        # of reason walkabout's is:
        #   lib_burst_generated.so — linked p_align=0x1000 and packs its RX/RW
        #     PT_LOAD segments into one 16 KB Apple page klepton-ld cannot lay
        #     out as two segments (old-gorn2 phase-7 measurement, GORN2-15). A
        #     LEAF (no core guest library DT_NEEDEDs it, reached only by
        #     findLibrary); dropping it costs Burst AOT and nothing else — Unity
        #     falls back to IL2CPP managed execution.
        #   libOVRPlugin.so / libovrplatformloader.so — replaced outright by the
        #     runtime (kl_ovrp.c, kl_ovrplat.c), as on every Unity guest.
        #   libopenxr_loader.so — shipped but unlinked (nothing DT_NEEDEDs it;
        #     the only binary mentioning it is libOVRPlugin.so, which we
        #     replace) — this title is OVRPlugin-mediated, not standalone OpenXR.
        #   libfrda.so / libfrda.config.so / libscript.so — third-party
        #     instrumentation, not part of the application; never loaded.
        # libfmod/libfmodstudio/libresonanceaudio ARE referenced from managed
        # code, so they ride along.
        "libs":    "libmain libfmod libfmodstudio "
                   "libresonanceaudio libOculusXRPlugin libunity libil2cpp",
        "srcdir":  "gorn2/lib/arm64-v8a",
        "tree":    "gorn2",
        "apk":     "gorn2.apk",
        "assets":  "gorn2/assets",
        "obb":     "obb",
        "qtplugins": "",
        "entry":   "libmain",
        "kind":    "unity",
        "product": "KleptonGorn2",
        "display": "GORN 2",
        # The tester's key art, laid on the Front/top parallax layer over a black
        # back (the game's own blood-on-black aesthetic) — same treatment as
        # beatsaber/superhot/walkabout.
        "icon":     "icons/gorn 2.png",
        "iconback": "0,0,0",
        # An arena the player stands inside — the same argument superhot's and
        # walkabout's entries make. `immersion: full` is also what sets the
        # runtime's opaque_world (resolve() derives it).
        "immersion": "full",
        # GORN 2 draws the player's own hands (gloved, gripping weapons), so the
        # system must not draw the wearer's real ones over the arena
        # (Immersive.upperLimbs -> .hidden). This used to be a NAME entry in
        # kl_app.c (`strcmp(t, "gorn2")` beside superhot); driver moved
        # own_hands into this row and the gorn2 row came across without it, so
        # launch 1 of the 2026-10-09 crossing showed the tester their real hands.
        # KL_HANDS=1 in the env file puts them back for a comparison.
        "own_hands": True,
        # Foveation (docs/FOVEATION.md), confirmed on the headset: EYE PASS
        # marked (MSAA x4 eye pair resolved into the runtime eye image), map
        # centres 2544,1476 = W/2, H*0.456 on both layers, refused 0 /
        # UNMAPPED 0, fove high 22%, no near miss. The caller's environment
        # still wins (kl_target_env_apply): KL_FOVEATION=off in the env file
        # reverts it.
        # KL_VK_LAZY_TRANSIENT=1 (2026-10-09 launch 2, the tester "seemed good"):
        # the six eye-size transient 4x MSAA attachments (3495 MiB, store
        # DONT_CARE, no input attachments read) go memoryless; [hb] peak
        # 6402M -> 2755M, no [mem] low memory, total GPU p90 9.0 -> 7.7 ms.
        # KL_VK_LAZY_TRANSIENT=0 in the env file reverts it.
        "env": "KL_FOVEATION=high KL_FOVE_BOX=0.30 KL_FOVE_FLOOR=0.08 KL_VK_LAZY_TRANSIENT=1",
    },
    "batman": {
        # BATMAN: ARKHAM SHADOW — Unity **2022.3.45f1** + IL2CPP, package
        # ru.camouflaj.manta, versionCode 350908 (1.4.0-350908). Read off the
        # APK rather than guessed, and all three answers put it on the OLDEST
        # path in this table rather than a new one:
        #
        #   - **classic front door.** The manifest names
        #     com.unity3d.player.UnityPlayerActivity (no GameActivity, no
        #     libgame.so in the APK at all), so this is the libmain ->
        #     NativeLoader.load door Beat Saber and SUPERHOT already drive.
        #     `entry`/`kind` are the plain Unity pair, not hardbullet's
        #     unity6ga.
        #   - **OVRPlugin, not OpenXR.** boot.config's deciding line is
        #     `xrsdk-pre-init-library=OculusXRPlugin` with `xr-meta-enabled=1`
        #     — the opposite of hardbullet's UnityOpenXR/0. libopenxr_loader.so
        #     ships anyway (Unity always packs the loader) and is expected to
        #     go unused; libovrplatformloader.so is the PLATFORM SDK half, as
        #     everywhere else.
        #   - **Vulkan is REQUIRED** (`android.hardware.vulkan.version`,
        #     required=true) and boot.config asks for the fragment density map
        #     by name. That makes this the first target pairing the OVRPlugin
        #     display path with kl_vulkan rather than kl_glfb, and it is a
        #     symptom to read on the first run rather than a step to pre-empt.
        #
        # SPLIT APPLICATION BINARY, like Beat Saber 1.40: the APK carries a
        # ~2.9 MB assets/bin/Data stub and main.350908.ru.camouflaj.manta.obb
        # (623 MB) sits beside it in the Quest dump. The guest builds that
        # filename from apktool.yml's versionCode and the manifest's package,
        # so those two files staging beside assets/ is as load-bearing as the
        # OBB.
        #
        # ...and then 18 GB MORE beside it, which is the Oculus asset-delivery
        # shape hardbullet established: ~60 Addressables `*_assets_all.bundle`
        # / `*_scenes_all.bundle` files, the per-language `Audio_*.obb` voice
        # packs named by `localized-asset-manifest.txt`, and a set of .mp4
        # videos (the DRM*.mp4 are the game's own cutscene reels, not a
        # protection layer). Those loose files are staged with
        # KL_OBB_EXTRA_DIR=<dir> (stage_assets.sh): the non-.obb files go
        # through the extra-files path and every *.obb through the OBB path,
        # and for a Unity target both land in the same <files>/obb.
        #
        # It carries the same excluded libraries RE4 and hardbullet do —
        # libfrda.so, libscript.so and libfrda.config.so (third-party
        # instrumentation, not part of the application). GUEST_EXCLUDED drops
        # them and they would be inert regardless: the libraries they target
        # are the ones this project replaces.
        #
        # Also present and deliberately NOT listed: assets/lib/arm64-v8a/*.so,
        # a folly/perflogger/zstd set that is Meta's telemetry plumbing loaded
        # (if at all) by path out of assets/. Nothing DT_NEEDEDs them; if a
        # dlopen ever asks, that is the signal to add a `qtplugins`-style
        # entry, not before.
        "libs":    None,
        "srcdir":  "batman/lib/arm64-v8a",
        "tree":    "batman",
        "apk":     "batman.apk",
        "assets":  "batman/assets",
        "qtplugins": "",
        # Nothing here reads a library as a FILE. See steamlink-vr.
        #
        # The flat Unity layout: this guest asks Java (Context.getObbDirs() ->
        # <files>/obb) rather than building an Android path itself, and no
        # absolute /sdcard path appears anywhere in libil2cpp.
        "obb":     "obb",
        "entry":   "libmain",
        "kind":    "unity",
        "product": "KleptonBatman",
        "display": "Arkham Shadow",
        "icon":     "icons/batman.png",
        "iconback": "0,0,0",
        # An opaque Gotham the player stands inside, with the player's own
        # gauntlets modelled and fists in them.
        "immersion": "full",
        "own_hands": True,
    },
    "itr": {
        # INTO THE RADIUS (CM Games, com.CMGames.IntoTheRadius, versionCode
        # 350002 / versionName 2.7.2) — **UE4.27.1, branch
        # ++UE4+Partner-Oculus-4.27** (manifest EngineVersion/EngineBranch),
        # project IntoTheRadius, Shipping, CookedFlavors=ASTC. That makes it
        # the fourth Unreal target after re4 (4.25.3), deadpool (4.27) and
        # tetris (4.26.2), and it is deadpool's engine version on tetris's XR
        # door — so nothing here is a new shape, which is the point of writing
        # the evidence down rather than the conclusion.
        #
        # The front door is the NDK activity one every UE4 target here uses:
        # the manifest names com.epicgames.ue4.GameActivity with
        # `android.app.lib_name = UE4`, there is no libmain, and libUE4.so
        # (212 MB) is the whole engine + game + plugins.
        #
        # **THE XR DOOR IS VrApi/OVRPlugin — measured, not assumed.** 4.27's
        # EOculusXrApi has THREE values and all three name-strings are in
        # libUE4 (`LegacyOVRPlugin`, `NativeOpenXR`, `OVRPluginOpenXR`), so the
        # enum decides nothing by itself. Three readings off this APK settle
        # it, and they agree:
        #
        #   - **the payload.** lib/arm64-v8a ships libOVRPlugin.so AND
        #     libvrapi.so; there is NO libopenxr_loader.so anywhere in the
        #     tree, so NativeOpenXR cannot run at all.
        #   - **the symbols.** libOVRPlugin defines 480 `ovrp_*` and imports
        #     58 `vrapi_*` (DT_NEEDED libvrapi.so). Neither library defines or
        #     imports a single `xr*`. libUE4 imports no `vrapi_*` and no
        #     `ovrp_*` — it dlsym's the 253 `ovrp_*` name strings it carries,
        #     the way 4.26 and 4.27 both do.
        #   - **the Java loads the legacy pair by name.** GameActivity's
        #     <clinit> System.loadLibrary()s, in order: ovrplatformloader,
        #     UE4, vrapi (in a try/catch on UnsatisfiedLinkError),
        #     OVRPlugin (same), ovrplatformloader again, c++_shared. No
        #     openxr_loader load anywhere in the class.
        #
        # So kl_ovrp claims both libraries as usual and runtime/xr/kl_openxr.c
        # is not on this guest's path.
        #
        # **VULKAN, and the DT_NEEDED says the opposite — believe the run.**
        # This one cost the first host boot, so it is written down as the
        # mistake it was: libUE4 DT_NEEDEDs libGLESv3 + libEGL and NOTHING
        # names libvulkan, the manifest asks only for `glEsVersion 0x30000`
        # and carries no `android.hardware.vulkan.version` feature, and the
        # cook is ASTC. Every static signal reads GLES. All of them are noise:
        # UE4's Android build ALWAYS links libGLESv3/libEGL and ALWAYS reaches
        # Vulkan by `dlopen("libvulkan.so")`, so the DT_NEEDED cannot decide
        # this and neither can the manifest — RE4 is the same shape, and
        # batman's `required=true` was the exception rather than the rule.
        #
        # What decides it is the guest's own message box, which round 1 got
        # verbatim on a worktree with no MoltenVK vendored:
        #
        #   "This device does not support Vulkan but the app was not packaged
        #    with ES 3.1 support."
        #
        # That is RE4's line exactly (DEBUG_ENV_VARS, `KL_UE4_VULKAN=0`), and
        # it means the title ships Vulkan shaders ONLY. So `make mvk` is a
        # prerequisite for this target, not an option, and the display path is
        # kl_vulkan — the second target here to pair the OVRPlugin door with
        # it, after batman. `KL_UE4_VULKAN=0` reproduces the refusal on
        # demand and is the A/B for anything that suspects the API choice.
        #
        # **THE PLATFORM SDK IS A LINK-TIME DEPENDENCY HERE, which is new.**
        # libUE4 DT_NEEDEDs libovrplatformloader.so directly and leaves 137
        # `ovr_*` symbols undefined — tetris dlsym'd them. Among them:
        # `ovr_PlatformInitializeAndroid`, `ovr_Entitlement_GetIsViewerEntitled`,
        # the whole Message pump, and `ovr_IAP_GetViewerPurchases` /
        # `ovr_Purchase_GetSKU`. So kl_ovrplat is not optional plumbing on this
        # guest, it is part of the load. Expect the entitlement check on the
        # way to the first frame (KL_PLAT_INIT), and expect the IAP query
        # after it.
        #
        # TWO OBBS, main.350002 + patch.350002 (3.3 GB + 1.2 GB).
        # `bHasOBBFiles=true`, `bPackageDataInsideApk=false`,
        # `bVerifyOBBOnStartUp=true` (so a truncated stage is a startup
        # failure, not a missing-content one), and — unlike tetris —
        # `bUseExternalFilesDir=true`. assets/ is small: the UE4 command line
        # (`../../../IntoTheRadius/IntoTheRadius.uproject`), Google's
        # memoryadvice json, and the VR splash.
        #
        # The same excluded libraries as re4/deadpool/tetris/batman, not
        # loaded: libfrda.so, libscript.so and libfrda.config.so (third-party
        # instrumentation; the last two are not ELF).
        # `make guestlibs LIBS=itr/lib/arm64-v8a` reports excluded=libfrda and
        # "not ELF"=libfrda.config libscript, so the generic GUEST_EXCLUDED
        # plus the ELF sniff already discover none of the three — no Makefile
        # change. They would be inert regardless: what they target
        # (libovrplatformloader, libvrapi, libc) is replaced by this project.
        #
        # Only SIX libraries translate, the shortest list of any target here:
        # libUE4, libc++_shared, libbhaptics-native-lib (bHaptics vest
        # support, loaded from com.bhaptics.core.CoreWrapper — Java, so on
        # this runtime it never loads and the vest is a no-op), libsentry +
        # libsentry-android (crash reporting; the manifest sets
        # `io.sentry.auto-init=false`, so it starts only if the game asks) and
        # libtry-alloc-lib (5.8 KB — Google's memoryadvice TryAllocTester,
        # loaded from its own Java class; assets/memoryadvice is its config).
        # Three of the six are therefore Java-gated and expected to be dead
        # weight; they translate because they are ELF, not because anything
        # needs them.
        "libs":    None,
        "srcdir":  "itr/lib/arm64-v8a",
        "tree":    "itr",
        "apk":     "itr.apk",
        "assets":  "itr/assets",
        "qtplugins": "",
        # UE4 builds the path ITSELF, Android's layout — see re4's and
        # tetris's notes. The same relative path is the SOURCE under
        # userdata/itr/ for a host run, so both obbs go at
        # userdata/itr/Android/obb/com.CMGames.IntoTheRadius/.
        "obb":     "Android/obb/com.CMGames.IntoTheRadius",
        # One 212 MB object named by the manifest's `android.app.lib_name`.
        # There is no libmain.
        "entry":   "libUE4",
        "kind":    "ue4",
        "product": "KleptonITR",
        # The game's name, not the runtime's — the re4/deadpool/tetris pattern.
        "display": "Into the Radius",
        "icon":     "icons/radius.png",
        "iconback": "0,0,0",
        # An opaque, oppressive Pechorsk the player stands inside — .mixed
        # would composite the room through a sky it does not have.
        "immersion": "full",
        # 4.27's GameActivity passes Build.VERSION.SDK_INT as the SECOND
        # argument of nativeSetAndroidVersionInformation — deadpool's and
        # tetris's spelling, not re4's five bare strings. Verified in THIS
        # APK's GameActivity.smali: the native is declared
        # `(Ljava/lang/String;ILjava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V`
        # and the call site passes Build.VERSION.RELEASE, the int, then
        # MANUFACTURER / MODEL / DISPLAY.
        "ue4_sdk_int": True,
        # It models the player's hands itself, and puts a rifle in them —
        # PROVISIONAL, set from the engine family the way tetris's was, and
        # the first in-headset round is what confirms or clears it.
        "own_hands": True,
        # **BAKED ENVIRONMENT, and this row is the first to need one.**
        #
        # `KL_PLAT_USER` answers `ovr_GetLoggedInUserID` with an ovrID. Without
        # it OnlineSubsystemOculus refuses to Init, puts up its own message box
        # — *"Online Subsystem Oculus not logged in. LoginError: Unable to get a
        # valid UserID."* — and aborts (round 2, signal 6). On the host that was
        # answered by typing the variable in front of `build/m_boot`. **On the
        # device there is nowhere to type it**: the wearer taps an icon in the
        # Home View, so a knob a target cannot boot without has to live in the
        # table or the target cannot ship. That is what `env` is for, and it is
        # generic — any row may bake any variable, nothing here is ITR-shaped.
        #
        # **The value is an INVENTED ovrID**, and that is stated rather than
        # hidden: there is no Oculus account behind this build and the truthful
        # answer to "who is logged in" is nobody. `runtime/xr/kl_ovrplat.c` says
        # so out loud on every run that reads it, and this baking does not
        # silence that line — it only removes the need to type it. It grants
        # nothing: `ovr_IAP_GetViewerPurchases` still answers an EMPTY array,
        # `ovr_Purchase_GetSKU` is still REFUSED, and every call that could buy
        # or deliver still aborts through the DRM marker. A user id is an
        # identity, not an entitlement.
        #
        # The environment still wins over this, so `KL_PLAT_USER=0 ./build/m_boot
        # itr` reproduces the abort exactly as before.
        #
        # **`KL_OVRP_UNIFY_FRUSTUM=1` — round 10, and it is the duplication.**
        #
        # The tester's measurement is what this is built on: with ONE EYE CLOSED the
        # duplication drops to a single copy, and the two copies sit side by side
        # like a TV showing two pictures. So each eye's picture is individually
        # correct and the two are placed too far apart to fuse — an inter-eye
        # geometry fault, not a sampling or crop one (rounds 8 and 9 closed those).
        #
        # THE CAUSE, measured on the host at the device's own geometry and
        # corroborated against the device's own log: **UE4's Oculus plugin does
        # not render the asymmetric per-eye cones we hand it.** It takes the union
        # — max(L0,L1) / max(R0,R1), i.e. a symmetric +/-60 deg cone — renders
        # BOTH eyes with it, and widens its own eye texture by the union ratio to
        # keep the angular density. That widening is the fingerprint and it is in
        # the device log: we describe the eye layer as 4032x3234 per eye and the
        # guest allocates **5088x3234** (later 6784x3588), a factor of 1.2619 that
        # appears ONLY when the cones are asymmetric — the same run with symmetric
        # cones allocates 4032 exactly. `KL_OVRP_EYE_TAN=vision` on the host
        # reproduces the headset's eye chain exactly (5088x3234 -> 10176x3234 ->
        # 13568x3588, identical to the device), which closes round 9's 1.2619 gap
        # and makes the host a faithful instrument for this class of bug.
        #
        # THE PROOF IT IS THE UNION AND NOT THE PER-EYE CONE: read the eye texture
        # back at that geometry and measure the same feature in the two halves. If
        # the guest had honoured the per-eye cones the halves would differ by
        # (l-r)/(l+r) = 26.8% of a half's width — **1817 px**. Measured shift:
        # **0 px**. Both halves carry one cone.
        #
        # THE CONSEQUENCE: kl_reproject builds eye 0's quad across tan
        # -1.732..+1.000 and eye 1's across -1.000..+1.732, so the centre of a
        # picture that was drawn straight ahead is placed **20.1 deg left in one
        # eye and 20.1 deg right in the other** — ~40 deg apart, against a fusion
        # limit of a degree or two. That is two side-by-side copies that will not
        # fuse, each correct on its own, and it is also why content sits off to one
        # side and is stretched ~27% horizontally. One cause, both symptoms.
        #
        # THE FIX IS THIS KNOB AND NO CODE. `klovrp_eye_tan` is the single read
        # side for what the guest is TOLD and for the tangents in the frame record
        # the composite builds its quad from, so unifying there makes the two
        # agree: the guest renders the union cone (which it was doing anyway) and
        # the composite now places the union cone. Verified on the host — the
        # widening disappears (4032 per eye, 8064x3234 -> 10752x3588) because the
        # guest's own union ratio becomes 1.
        #
        # THE COST, stated rather than buried: the guest keeps the un-widened
        # width, so the union cone lands on ~79% of the horizontal pixels it had
        # (6784 -> 5376 per eye). That is resolution traded for correct geometry,
        # recoverable later by raising the eye cap — a separate one-variable
        # change, not this one.
        #
        # `KL_OVRP_UNIFY_FRUSTUM=0` in the environment restores the old behaviour,
        # so the A/B is one variable.
        #
        # **`KL_REPROJECT_DEPTH` IS BACK AT ITS DEFAULT — round 12's theory is
        # REFUTED, on the tester's own launch #5.**
        #
        # Round 12 shipped `KL_REPROJECT_DEPTH=3.0` here on the strength of two
        # measurements, and it was diagnostic by construction: it predicted the
        # near field would improve and the far field would acquire a new swim.
        # Neither happened. His verdict on launch #5, verbatim: "no change. in
        # fact, now that i take another look. if i keep my head very still, i
        # still see some objects flicker in and out of visibility. but it
        # happens a lot more objects when i move my head."
        #
        # That second sentence CORRECTS his earlier answer, which was "stops
        # when still" and which is what selected round 12's whole hypothesis
        # class. It still flickers at rest, with fewer objects, and head motion
        # amplifies it. A defect that is non-zero at rest is not a function of
        # (pose at display - pose at render) alone, so the reprojection cannot
        # be the whole of it — and the device's own [rp] translation census on
        # launch #5 puts the residual at 0.247 deg WORST on content at 1.5 m,
        # which is a sub-pixel swim and not an object appearing and vanishing.
        #
        # So the knob comes off. It is refuted, it costs far-field accuracy for
        # nothing, and this port ships one variable at a time. The correction
        # itself stays in the code and stays inert at the 500 m default; the
        # instrument that measured it stays on. Set `KL_REPROJECT_DEPTH=3.0` in
        # the environment to put round 12's build back.
        # **`KL_OVRP_STAGES=4` IS BACKED OUT — round 15.** Round 13 shipped it
        # as launch #6 on the stage-recycling theory: the eye swapchain has
        # three stages, the guest claims one at BeginFrame and draws into it,
        # and nothing prevented it from starting frame N+3 in a stage the
        # compositor's GPU pass was still reading frame N out of. The
        # arithmetic from launch #5's log was real (33.0 ms reclaim against a
        # 17.8-22.2 ms pose age and a 9-11 ms slot) and a fourth stage did move
        # the headroom from ~11-15 ms to ~22-26 ms.
        #
        # **IT BOUGHT NOTHING, AND THE CENSUS THAT RODE WITH IT SAYS WHY.**
        # `[ovrp] stage margin` on launch #6 reports 117 at-risk presents of
        # 2412 — and that 117 is STATIC across the run (111, 111, 117, 117).
        # It accrued during bring-up and load and then stopped moving. So even
        # at THREE stages, steady-state gameplay had essentially zero at-risk
        # presents: the mechanism was never firing where the tester sees the bug.
        # At four stages there are zero proven collisions and he reports the
        # picture "largely looks the same". The theory is refuted by the
        # instrument built to test it, which is the outcome that census was
        # shaped for.
        #
        # So the knob comes off and the row returns to KLOVRP_STAGES_DEFAULT.
        # It is not a regression — it is a reversion that also returns ~101 MiB
        # of eye image, priced from the device's own log. Set
        # `KL_OVRP_STAGES=4` in the environment to put launch #6's build back.
        #
        # ONE STALE CLAIM IN THE OLD COMMENT IS RETRACTED WITH IT: it argued the
        # menu is clean because "a menu is a static panel over a static skybox".
        # Round 13 measured that and it is FALSE — the menu is a full 3D world
        # with birds, a rotating debris ring and rippling water, and it runs
        # FASTER than the level. The menu/level split is still unexplained by
        # anything except software occlusion culling (see STATUS.md).
        #
        # **`MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS=0` — LAUNCH #7's ONE
        # BEHAVIOURAL CHANGE, round 18.** Round 17 proved the per-object binding
        # path does not pass through Klepton at all: not one of
        # vkCmdBindDescriptorSets / vkUpdateDescriptorSets / vkCmdPushConstants /
        # vkCmdBindVertexBuffers / vkCmdBindPipeline / vkCreateBuffer /
        # vkBindBufferMemory / vkAllocateDescriptorSets / vkCreateDescriptorPool
        # / vkResetDescriptorPool is intercepted. So a rock drawn with another
        # rock's transform has no mechanism in our layer, and the search moves
        # into stock MoltenVK 1.4.2.
        #
        # THE KNOB WAS GATED BEFORE IT WAS SHIPPED, because this port lost eight
        # rounds to a knob that silently did nothing. All three answers were
        # read out of the RUNNING library, not out of release notes:
        #
        #  1. The effective default is ON. vkGetMoltenVKConfigurationMVK on
        #     vendor-moltenvk/out/macos/libMoltenVK.dylib reports
        #     useMetalArgumentBuffers=1 with the variable unset, and 0 with it
        #     set to 0. Setting 0 is NOT a no-op.
        #  2. The ordering is right. MoltenVK is dlopened LAZILY by vk_init()
        #     (kl_vulkan.c), and this row is applied by kl_target_use() at the
        #     moment the run commits to a target. In the host log the two
        #     `[target] itr:` lines are at line 1-2 and `MoltenVK loaded` is at
        #     line 236. MoltenVK also latches its config lazily — the probe gets
        #     the same answer setting the variable AFTER dlopen — so there is
        #     margin on top of the ordering rather than reliance on it. The
        #     guest never enables VK_EXT_layer_settings (no such string in
        #     libUE4.so), so nothing overrides it at vkCreateInstance either.
        #  3. It changes observable behaviour on the host, in the MSL MoltenVK
        #     generates, and it changes it exactly on the per-primitive path.
        #     MVK_CONFIG_SHADER_DUMP_DIR over two host runs, same shader hash
        #     (shader-vs-73f1d106582e3e1f):
        #       ON : struct spvDescriptorSetBuffer0 { ... const device
        #            View_PrimitiveSceneData_BUFFER* m_74 [[id(3)]]; ... } bound
        #            as one argument buffer at [[buffer(0)]], with UE4's dynamic
        #            uniform-buffer offsets applied INSIDE the shader out of a
        #            separate spvDynamicOffsets[] buffer at [[buffer(25)]].
        #       OFF: each resource is its own discrete binding —
        #            _74 [[buffer(2)]], _613 [[buffer(3)]], _30/_14 [[buffer(0/1)]]
        #            — and the dynamic offset is applied by Metal's own
        #            setBuffer:offset:, not by shader arithmetic.
        #     View_PrimitiveSceneData IS the per-primitive transform buffer. So
        #     the knob moves precisely the data whose corruption would look like
        #     "the rock is rotated and placed slightly differently".
        #
        # COST, stated so it is not a surprise: with argument buffers off the
        # MSL is specialised per pipeline layout rather than layout-independent,
        # so MoltenVK generates many more distinct shaders — the same host run
        # dumped 10 distinct .metal files ON and 1614 OFF. Expect a LONGER
        # first-run shader warm and a bigger pipeline cache. Steady-state draw
        # cost should be similar or slightly higher (more discrete binding
        # calls). Set `MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS=1` in the
        # environment to put the default back.
        #
        # **`KL_OVRP_STAGES=4` IS BACK, FOR THIS LAUNCH ONLY.** Round 15 backed
        # it out (see the retraction above) and the backout is still correct —
        # but launch #6, the build the tester last saw, shipped WITH it. Restoring
        # it here keeps launch #7 a single-variable A/B against launch #6
        # instead of two changes at once. It costs ~101 MiB of eye image against
        # launch #6's `[vk] guest memory: 1940 MiB live, peak 3052`, nowhere
        # near the jetsam line. **IT REVERTS TO 3 ON THE FOLLOWING BUILD** —
        # recorded under STATUS.md Open questions so it is not forgotten.
        # ------------------------------------------------------------------
        # **ROUND 20 / LAUNCH #8 CARRIES NO BEHAVIOURAL KNOB AT ALL.** The row
        # is launch #6's environment — the last build the tester saw — plus the
        # round-19 GEOMETRY CENSUS, which is log only.
        # `MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS=0` COMES OFF (refuted by launch
        # #7, and it cost 1614 shader variants against 10 — that is the
        # multi-minute first-launch warm the tester felt). So launch #8 is a REVERT
        # to a seen configuration carrying one new instrument, which is exactly
        # what one-change-per-launch wants.
        #
        # **`MVK_CONFIG_USE_MTLHEAP=0` (candidate R1) WAS GATED AND IT FAILED
        # THE GATE — it is BLOCKED, not refuted, and it must not be shipped.**
        # A 200 s host run with it set dies in ~40 s of guest time:
        #
        #   [mvk-error] VK_ERROR_FEATURE_NOT_PRESENT: vkCreateImage() : MTLHeap
        #   must be enabled to create 2D-on-3D or block texel view compatible
        #   images.
        #   [jni] AndroidThunkJava_ForceQuit() ... request #1
        #   [klepton] fault: signal 11 at 0x20, pc
        #     MVKImage::getMemoryRequirements(VkMemoryRequirements*, unsigned char)+0x0
        #     lr = libMoltenVK.dylib`vkGetImageMemoryRequirements+0x40
        #
        # MVKImage.mm:1395-1401 REFUSES any image carrying
        # VK_IMAGE_CREATE_2D_VIEW_COMPATIBLE_BIT_EXT,
        # VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT or
        # VK_IMAGE_CREATE_BLOCK_TEXEL_VIEW_COMPATIBLE_BIT when placement heaps
        # are off ("These features require placement heaps to alias textures"),
        # UE4 does not check the VkResult, and the null VkImage it then hands to
        # vkGetImageMemoryRequirements is the segfault. The knob is unreachable
        # for THIS guest by environment alone. Two ways to revive R1 later, both
        # code: strip those flags in Klepton's own vkCreateImage interception
        # (and accept whatever the guest wanted 2D-on-3D views for), or leave it.
        # vendor-moltenvk is never patched.
        #
        # **`MVK_CONFIG_LIVE_CHECK_ALL_RESOURCES=1` (candidate R2) IS A NO-OP ON
        # THIS CONFIGURATION, so it must not be shipped either.** Its effective
        # value is 0 by default and 1 when set (probe below), but every consumer
        # is gated away here: MVKDevice.mm:5225 computes
        # `needsLiveTrackingForEncode = !hasResidencySet() && (liveCheckAllResources
        # || descriptorBindingPartiallyBound)`, and MVKQueue.mm:489 has the same
        # `!hasResidencySet()` guard. MVKDevice.mm:5451-5460 creates the
        # residency set whenever `_isUsingMetalArgumentBuffers &&
        # _metalFeatures.residencySets`, and :2580 sets residencySets =
        # mvkOSVersionIsAtLeast(15.0, 18.0, 2.0) — visionOS 27 clears that by a
        # wide margin, argument buffers are back ON in launch #8, and the xros
        # slice provably contains the path ("Primary residency set",
        # "newResidencySetWithDescriptor:error:" are in its strings). The only
        # ungated consumer, MVKPipeline.mm:283, applies solely when
        # `layout->argBufMode() == MVKArgumentBufferMode::Off`. So R2 would be
        # the KL_OVRP_EYE_FORCE mistake again — a knob that silently does
        # nothing — and this port has already paid eight rounds for that one.
        #
        # THE THEORY (docs/r19-desk-analysis.md, candidate R1). MoltenVK creates
        # one placement MTLHeap per VkDeviceMemory (MVKDeviceMemory.mm:220-229,
        # hazardTrackingMode = Tracked at :226, [buf makeAliasable] at :255) and
        # then gives EVERY VkBuffer its OWN aliasing MTLBuffer placed at its
        # offset in that heap (MVKBuffer.mm:157-159; getMTLBufferOffset() then
        # returns 0, MVKBuffer.h:74). **Metal's automatic hazard tracking is
        # per-MTLResource** — two MTLBuffers aliasing the same heap bytes are
        # NOT hazarded against each other. UE4's Vulkan RHI suballocates many
        # VkBuffers out of one VkDeviceMemory and recycles ranges continuously
        # (its buffer allocator, its ring buffers, FPrimitiveIdVertexBufferPool),
        # and with r.Mobile.SupportGPUScene=1 the per-primitive transform
        # arrives as View_PrimitiveSceneData_BUFFER[PrimitiveId] with PrimitiveId
        # bound as an instance-rate vertex stream. So a GPUScene range WRITTEN
        # through MTLBuffer A and READ through MTLBuffer B has no ordering at
        # all. With the knob at 0 there is ONE MTLBuffer per VkDeviceMemory
        # (MVKDeviceMemory.mm:265) and every VkBuffer is an offset into it, so
        # Metal tracks the single resource conservatively and the hole closes.
        #
        # It also explains the constraint that has survived every round:
        # skeletal meshes (hands, the held gun) do NOT use the PrimitiveId
        # stream — FSkeletalMeshSceneProxy takes a per-draw primitive uniform
        # buffer — so no defect on this path can ever reach them. That is a
        # STRUCTURAL account of "his hands never flicker", not a probabilistic
        # one.
        #
        # GATED THREE WAYS BEFORE IT SHIPPED, same method as round 18:
        #  1. THE EFFECTIVE DEFAULT IS ON, so 0 is not a no-op. A probe that
        #     dlopens vendor-moltenvk/out/macos/libMoltenVK.dylib and calls
        #     vkGetMoltenVKConfigurationMVK reports useMTLHeap=1 unset, 0 with
        #     the variable at 0, 2 with it at 2 (cfgsize 184/184, private API
        #     v43). The default is MVK_CONFIG_USE_MTLHEAP_WHERE_SAFE with NO
        #     platform #ifdef (MVKEnvironment.h:274-280), and WHERE_SAFE means
        #     ALWAYS on anything that is not an AMD GPU (MVKDevice.mm:2461-2466).
        #     AND THE DEVICE BINARY WAS CHECKED DIRECTLY rather than inferred:
        #     disassembling vendor-moltenvk/out/xros/MoltenVK.framework/MoltenVK
        #     (arm64, 1.4.2, same build) shows at 0x5f758
        #         adrp x0, "MVK_CONFIG_USE_MTLHEAP"
        #         fmov d0, #1.00000000
        #         bl   mvkGetEnvVarNumber
        #     — the visionOS slice reads this exact variable by name and its
        #     compiled-in default is literally 1.
        #  2. THE ORDERING IS RIGHT. MoltenVK is dlopened LAZILY by vk_init()
        #     (kl_vulkan.c) and this row is applied by kl_target_use() the
        #     moment the run commits to a target, so the `[target] itr:` lines
        #     print in the first handful of lines and `[vk] MoltenVK loaded`
        #     hundreds of lines later. MoltenVK also latches its global config
        #     lazily, so there is margin on top. The guest cannot override it:
        #     VK_EXT_layer_settings appears nowhere in libUE4.so.
        #  3. IT CHANGES OBSERVABLE BEHAVIOUR, read back through Vulkan itself.
        #     MVKDevice.mm:2466 sets _metalFeatures.placementHeaps from this
        #     config, and :741-742 reports that same bool straight back out as
        #     VK_EXT_image_2d_view_of_3d's image2DViewOf3D / sampler2DViewOf3D.
        #     vkGetPhysicalDeviceFeatures2 on the running library:
        #       unset -> image2DViewOf3D=1 sampler2DViewOf3D=1  (heaps ON)
        #       =0    -> image2DViewOf3D=0 sampler2DViewOf3D=0  (heaps OFF)
        #       =2    -> 1 / 1                                  (heaps ON)
        #     That is the effective placement-heap decision, per physical
        #     device, not the raw config value — i.e. the very flag that selects
        #     MVKBuffer.mm:45's aliasing-MTLBuffer path.
        #
        # COST: a frame-rate cost from over-conservative serialisation is
        # EXPECTED and is not a finding. Unlike launch #7 there should be **no
        # shader warm-up change at all** — this touches allocation, not MSL — so
        # a slow first launch would itself be a surprise worth reporting.
        # Set `MVK_CONFIG_USE_MTLHEAP=1` in the environment to put the default
        # back; `MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS=0` to re-create launch #7.
        #
        # **`KL_VK_GEOM_CENSUS=1`** is the round-19 per-frame INDEX TOTAL, log
        # only (it is on by default; named explicitly so the device log is
        # self-describing about what produced the numbers). In the menu at rest
        # it reads ONE distinct value — 1156216 indices/frame over 3391 frames,
        # 0 flaps — so if it reads one value in the LEVEL too while the tester sees
        # objects blink, the loss is downstream of the guest, and if it flaps,
        # the guest is dropping them. That is the fork this port has been stuck
        # on for six rounds.
        #
        # **`KL_VK_DRAW_DIFF=1`** is round 21's PER-DRAW FINGERPRINT DIFF, log
        # only. The geometry census above cannot see one wall go missing — a
        # dozen-plank wall is ~400 of 420000 indices, a tenth of one percent,
        # under its 1% flap threshold — and a missing wall is exactly what
        # The tester photographed (docs/shots/launch8-missing-wall.png). This diffs
        # the frame's MULTISET of draw fingerprints against the previous
        # frame's. Read the COARSE line: on the host, in the menu at rest, it
        # reads EVERY FRAME IDENTICAL, 0 vanished, over five windows, and under
        # a +/-10 deg pitch sweep it reads 1-6 vanished in ~25% of frames. So
        # a non-zero coarse count in a LEVEL at rest means the guest itself
        # stopped submitting something.
        #
        # **`MVK_CONFIG_USE_MTLHEAP=0`** is R1, the round-19 lead candidate, and
        # it is THE ONE BEHAVIOURAL VARIABLE IN THIS BUILD. With placement heaps
        # off there is one MTLBuffer per VkDeviceMemory instead of an aliasing
        # MTLBuffer per VkBuffer, so Metal's per-resource hazard tracking covers
        # the GPUScene/PrimitiveId traffic it currently cannot see. Round 20
        # could not ship it: MVKImage.mm:1395-1401 refuses 2D-view/2D-array/
        # block-texel-view compatible images when heaps are off and UE4 does not
        # check the VkResult. Round 21 measured that this guest creates 12 such
        # images, ALL of them 3D and ALL carrying only 2D_ARRAY_COMPATIBLE, and
        # that NOT ONE of the 12 views built on them is a 2D view of a 3D image
        # — so the flags are decoration here and kl_vulkan.c strips them when
        # this variable is 0. The device log says which way it went: look for
        # "MoltenVK PLACEMENT HEAPS are OFF".
        #
        # **`KL_OVRP_STAGES=4` STILL RIDES ALONG, and only to keep the A/B
        # single-variable against launch #6** — the build the tester last saw with
        # STAGES=4. It is proven inert by its own margin census. IT REVERTS TO 3
        # ON THE FOLLOWING BUILD; recorded under STATUS.md Open questions.
        #
        # **`KL_UE4_MULTIVIEW=1` — ROUND 25, AND IT IS THE ONE VARIABLE IN
        # LAUNCH #11.** The game ships `vr.MobileMultiView=True`, and Klepton's
        # `ue4_no_multiview` (runtime/guest/kl_ue4.c:580) has been clearing
        # `GSupportsMobileMultiView` on every launch so far — for an argument
        # about ANGLE and `GL_OVR_multiview2` that CANNOT APPLY to a Vulkan-only
        # guest. So launches #1-#10 all ran a two-sequential-view DoubleWide
        # stereo path the title never shipped with. This knob leaves the guest's
        # own decision alone. Round 19 called it a trap on the claim that
        # MoltenVK emulates multiview by multiplying the instance count and
        # would collide with GPUScene's instance-rate PrimitiveId stream; the
        # mechanism is real (MVKCmdDraw.mm:323-324, :611-612, MVKDevice.h:430 —
        # and v1.4.2 has NO vertex-amplification path at all), but MoltenVK
        # compensates by scaling per-instance step rates by the view count
        # (MVKPipeline.mm:1876-1884) and compiles a pipeline per view count
        # (:1105-1126). Verified in source, not assumed — docs/r25-multiview.md.
        #
        # Host, four interleaved 230 s runs at device geometry: the eye layer
        # becomes ovrpLayout 3 (Array), 2944x2664 x2 ARRAY LAYERS instead of one
        # 5888x2664 DoubleWide — PER-EYE RESOLUTION UNCHANGED — draws/frame
        # 229 -> 124, indices/frame 1156216 -> 578417, COARSE draw diff still
        # every frame IDENTICAL, 0 mvk-errors, 0 DOUBLED composite faults, and
        # MORE frames in the same wall clock, not fewer.
        # ------------------------------------------------------------------
        # ROUND 26 — launch #12. Multiview is CONFIRMED as the flicker fix
        # (The tester, launch #11: "you fixed it!"), so the diagnostic scaffolding
        # comes off and the resolution it cost us goes back on, in one build at
        # The tester's direction:
        #   - KL_OVRP_STAGES=4 REMOVED (back to the default 3).
        #   - MVK_CONFIG_USE_MTLHEAP=0 REMOVED — refuted in round 21; the image
        #     flag strip shim stays in code but is inert without the env.
        #   - KL_VK_GEOM_CENSUS / KL_VK_DRAW_DIFF REMOVED — log-only, their
        #     question is answered.
        #   - KL_OVRP_EYE_MAX=5088 ADDED. Launch #11 ran 4032x3234 per slice
        #     because kl_app.c bakes `setenv("KL_OVRP_EYE_MAX","4032",0)`
        #     app-wide; the row is applied by kl_target_use() FIRST, so an
        #     explicit value here wins (overwrite=0). 5088 is the panel's own
        #     per-eye width (~5088x4081), i.e. "no cap" for this display.
        # ------------------------------------------------------------------
        # ROUND 27 — `KL_UE4_MULTIVIEW=1` IS GONE FROM THIS ROW, AND THAT IS
        # THE POINT OF THE CROSSING, NOT A REGRESSION. The clear it was
        # suppressing is now gated on `kl_vulkan_guest_active()` inside
        # `ue4_no_multiview` itself, so every Vulkan UE4 guest gets the right
        # answer without a row saying so, and GL UE4 guests (tetris, re4,
        # deadpool) still get the clear that keeps ANGLE from ForceQuitting on
        # `layout(num_views = 2)`. The knob survives as a TRI-STATE override —
        # `=1` never clear, `=0` always clear — documented in DEBUG_ENV_VARS.md.
        # If the flicker ever returns on this title, `KL_UE4_MULTIVIEW=1` back
        # in this string is the one-token way back.
        "env": "KL_PLAT_USER=1000000000000001 KL_OVRP_UNIFY_FRUSTUM=1 "
               "KL_OVRP_EYE_MAX=5088",
    },
    "deadpool": {
        # Deadpool VR — the NINTH title here (the tenth row: jkxr is one tree
        # with two), the SECOND Unreal one and the
        # first UE4.27: Twisted Pixel's com.twistedpixelgames.PILO, versionCode
        # 8742, built from branch ++UE4+Partner-Oculus-4.27 (project PILO,
        # Shipping, ASTC). RE4 was 4.25.3, so every engine-dialect answer the
        # re4 target taught this project now gets asked a second time against
        # a different engine generation.
        #
        # The front door is re4's exactly: a NativeActivity,
        # com.epicgames.ue4.GameActivity with `android.app.lib_name = UE4`, so
        # the entry is libUE4 and the boot is the NDK activity door, not a
        # JNI_OnLoad chain.
        #
        # What is NOT re4's is the XR door. This is an OpenXR title:
        # libopenxr_loader.so ships, FOpenXRHMD symbols are in libUE4, and
        # there is no libvrapi.so anywhere in the tree — where RE4 was
        # VrApi/OVRPlugin and let kl_ovrp claim both. The interposition this
        # lands on is runtime/xr/kl_openxr.c, the synthetic runtime written for
        # Steam Link's libvrlink_scene and since exercised by two Unity guests
        # and JKXR; no UE4 guest has ever walked it, so which of its
        # refuse-by-name entry points PILO reaches first is a first-boot
        # measurement, not a table fact.
        #
        # The same excluded libraries as RE4, not loaded: libfrda.so,
        # libscript.so and libfrda.config.so (third-party instrumentation;
        # the last two are NOT ELF). The Makefile's GUEST_EXCLUDED is generic (by basename, not per
        # target) and already names libfrda and libscript; the config and the
        # script additionally fail the ELF sniff, so `libs: None` discovers
        # none of the three. No Makefile change was needed. They would be INERT
        # regardless — the libraries they target are the ones this project
        # REPLACES.
        #
        # Content is where the staging differs from every target here: no OBB
        # files at all despite `bHasOBBFiles=true` in the manifest — the game
        # data is 54 LOOSE .pak files, ~35 GB, flat, plus one .lang file. UE4.27 with
        # bUseExternalFilesDir reads <external>/Android/obb/<package>/, so the
        # obb key below names that path and stage_assets.sh stages *.pak for
        # this target where every other stages *.obb. The mount point IS
        # obb/: first boot opened every pak there (KL_TRACE_FS, r30 log) and
        # the engine never looked under data/<pkg>/files/.
        "libs":    None,
        "srcdir":  "deadpool/lib/arm64-v8a",
        "tree":    "deadpool",
        "apk":     "deadpool.apk",
        "assets":  "deadpool/assets",
        "qtplugins": "",
        # UE4 builds the path ITSELF, Android's layout — see re4. The value is
        # the same relative path stage_assets.sh reads the SOURCE from (under
        # userdata/deadpool/), which for this target holds the loose paks.
        "obb":     "Android/obb/com.twistedpixelgames.PILO",
        # libUE4 again — one 270 MB object holding engine, game and plugins
        # (re4's was 172 MB), named by the manifest's `android.app.lib_name`.
        "entry":   "libUE4",
        "kind":    "ue4",
        "product": "KleptonDeadpool",
        # Plain "Deadpool", the RE4/Beat Saber pattern: the Home View shows
        # the game's name, not the runtime's (the tester's call, 2026-08-19).
        "display": "Deadpool",
        # mkicon.py's source art and back-layer colour, from the gitignored
        # icons/ directory (a missing source is not a failure). The art wants
        # its background keyed to alpha — mkicon.py's layers NEED alpha: an
        # opaque source trims to the whole canvas and the Middle bloom then
        # frames the icon in a dark square seam. iconback is the keyed-out
        # background colour (the back layer must be opaque edge-to-edge).
        "icon":     "icons/deadpool.png",
        "iconback": "248,195,22",
        # It models the player's hands itself, so the system's own upper-limb
        # rendering is a second pair of hands (same shape as re4's).
        "own_hands": True,
        # 4.27's GameActivity passes Build.VERSION.SDK_INT as the second
        # argument of nativeSetAndroidVersionInformation where 4.25's (re4)
        # passes five bare strings — kl_ue4.c transcribes either spelling and
        # the row says which. Verified against this APK's GameActivity.smali.
        "ue4_sdk_int": True,
        # The stock platform gate. PILO's boot calls the SYNCHRONOUS
        # ovr_PlatformInitializeAndroid and then ovr_GetLoggedInUserID, and
        # ForceQuits when the user id comes back 0 — UE4.27's
        # OnlineSubsystemOculus reports the platform as unusable and the
        # game's own shutdown follows (measured: the quit lands right after
        # the user-id answer). This key says the synthetic platform reports
        # itself UP for this target and answers its user question with a
        # declared local user — the same carve-out shape as the app's own
        # entitlement (kl_ovrplat.c): the user owns the title, so the
        # platform being "up" describes that copy running; everything that
        # DELIVERS content (IAP, purchases, AssetFile_Download*) still
        # refuses. Every other target keeps the truthful-failure default.
        #
        # CROSSED ONTO DRIVER 2026-10-09: this was a `plat_init` table column on
        # the deadpool branch; driver carries per-target policy as the row's
        # space-separated `env`, applied by kl_target_use (the caller's own
        # environment still wins). KL_PLAT_INIT=1 is the same switch, read in
        # kl_ovrplat.c — the name PORTING.md already gave the bake.
        #
        # MULTIVIEW CHANGES AT THE CROSSING, deliberately. Deadpool is a VULKAN
        # UE4 guest (the itr row's note above lists it among the GL ones; it is
        # not — its eye layer is a VkImage and the RHI writes a
        # VulkanProgramBinaryCache). On the deadpool branch kl_ue4.c cleared
        # UE4's mobile-multiview flags unconditionally, so all seven device
        # rounds ran UE4's two-sequential-view DoubleWide stereo. Driver's
        # ue4_no_multiview leaves them alone for a Vulkan guest (ITR's eleven
        # flicker launches, docs/r25-multiview.md), so this build renders the
        # path the title SHIPPED with — which is also the layered shape kl_fove
        # foveates (a one-slice pass over a 2-layer eye pair is left
        # unfoveated). If the picture regresses, `KL_UE4_MULTIVIEW=0` (env file
        # or here) restores the old clear exactly.
        #
        # Foveation baked, confirmed on the headset (no seam, smooth): the
        # transient MSAA scene and the sub-rect re-centre of kl_fove
        # (docs/FOVEATION.md §5.2, §5.3); 22% of fragments, high box 0.30 /
        # floor 0.08. Resolution stays at the default cap 4032 (asked
        # 4032x3234 -> UE4 widens to the symmetric 5088x3236 intermediate):
        # peak 6343M with guest GPU p90 9.8 ms already over the 120 Hz period,
        # so 4032 is the ceiling. KL_FOVE_INTERMEDIATE=0 in the env file is the
        # off switch.
        "env": "KL_PLAT_INIT=1 KL_FOVEATION=high KL_FOVE_BOX=0.30 "
               "KL_FOVE_FLOOR=0.08",
    },
    "fnaf": {
        # Five Nights at Freddy's: Help Wanted 2 — the ELEVENTH title, and the
        # SECOND UE4.27 one. Steel Wool's com.SteelWool.HelpWanted2, versionCode
        # 148113100 (versionName 1.148113.100), engine 4.27.2 on branch
        # ++UE4+Partner-Oculus-4.27 — the SAME branch deadpool is built from, so
        # this row is deadpool's answers asked a second time rather than a new
        # engine generation. Shipping, ASTC, minSdk/targetSdk 32.
        #
        # ...and the UE4 PROJECT is called "Thumper", which is not a typo and is
        # not the Drool game: assets/UE4CommandLine.txt says
        # `../../../Thumper/Thumper.uproject`, every gameplay class is
        # AThumper*/UThumper*, and the engine's own log categories are
        # LogThumper*. So the paks inside the OBBs, the Saved/ directory and any
        # engine log this ever produces are named Thumper, not HelpWanted2.
        #
        # The front door is deadpool's exactly: com.epicgames.ue4.GameActivity
        # with `android.app.lib_name = UE4`, so the entry is libUE4 and the boot
        # is the NDK activity door. libUE4's undefined set is the same NDK
        # surface re4 introduced — AInputQueue/AKeyEvent/AMotionEvent, the
        # AAssetManager family, ALooper — plus Play Core's AssetPackManager
        # (libplaycore.so is DT_NEEDED; nothing on a Quest install uses it).
        #
        # THE XR DOOR IS OVRPlugin, with the OpenXR loader beside it and BELOW
        # it. Both libraries ship, both are System.loadLibrary'd by
        # GameActivity's static initialiser (`openxr_loader` then `OVRPlugin`),
        # and NEITHER is in libUE4's DT_NEEDED — they are dlopen'd/dlsym'd, so
        # libUE4 has zero undefined `ovrp_`/`xr` symbols. What decides the path
        # is UE4.27's OculusHMD plugin setting EOculusXrApi, and this build
        # carries only two of the three arms as strings —
        # `EOculusXrApi::OVRPluginOpenXR` and `EOculusXrApi::NativeOpenXR` —
        # with FOculusOpenXRHMD present as the loader shim. In OVRPluginOpenXR
        # (the 4.27 default, and what deadpool measured on this same branch) the
        # GAME still speaks the `ovrp_*` C API and OpenXR is an implementation
        # detail INSIDE libOVRPlugin — which this project replaces wholesale, so
        # kl_ovrp.c is the interposition and runtime/xr/kl_openxr.c is expected
        # to see nothing. That is deadpool's measured shape (its rounds are full
        # of `[ovrp] SetupLayer`), so it is the expectation here; the first boot
        # log is what falsifies it, and if the guest walks NativeOpenXR instead
        # the `xr*` refusals will name themselves.
        #
        # The same excluded libraries RE4 and deadpool carry, not loaded:
        # libfrda.so, libfrda.config.so and libscript.so (third-party
        # instrumentation; the last two are not ELF). The Makefile's GUEST_EXCLUDED is by basename and already
        # names libfrda and libscript; the config and the script also fail the
        # ELF sniff. `libs: None` therefore discovers none of the three, and they
        # would be inert regardless — the libraries they target are the ones
        # this project REPLACES.
        #
        # Content: THREE OBB FILES, ~8.3 GB, which is a layout no target here
        # has had. re4 had main+patch; deadpool had 54 loose paks and no OBB at
        # all. This one is main + overflow1 + overflow2 —
        # `bAllowOverflowOBBFiles`, the UE4.27 Oculus-branch extension that
        # lifts the two-file Play limit. libUE4 builds all six names itself
        # (`main.%d.%s.obb`, `patch.%d.%s.obb`, `overflow1..4.%d.%s.obb`) under
        # `/Android/obb/<package>/` and says which it mounted
        # (`Mounted overflow1 OBB: %s`), so the `obb` key below is that
        # directory. UE4 builds it from the APK's PACKAGE name, so the key must
        # match the package of the APK being staged.
        "libs":    None,
        "srcdir":  "fnaf/lib/arm64-v8a",
        "tree":    "fnaf",
        "apk":     "fnaf.apk",
        "assets":  "fnaf/assets",
        "qtplugins": "",
        # UE4 builds the path ITSELF, Android's layout — see re4 and deadpool.
        "obb":     "Android/obb/com.SteelWool.HelpWanted2",
        # One 201 MB object holding engine, game and plugins, named by the
        # manifest's `android.app.lib_name`.
        "entry":   "libUE4",
        "kind":    "ue4",
        "product": "KleptonFnaf",
        # The game's name, not the runtime's (the RE4/deadpool pattern). Short,
        # because the Home View label is small.
        "display": "FNAF HW2",
        # mkicon.py's source art and back-layer colour (icons/, gitignored): a
        # transparent-cornered logo, which is the shape mkicon's layers need
        # (see deadpool's note about opaque sources).
        "icon":     "icons/fivenights2.png",
        "iconback": "0,0,0",
        # It models the player's hands itself, like every other VR title here.
        "own_hands": True,
        # 4.27's GameActivity passes Build.VERSION.SDK_INT as the second
        # argument of nativeSetAndroidVersionInformation. Verified against THIS
        # APK's GameActivity.smali: the descriptor is
        # (Ljava/lang/String;ILjava/lang/String;...)V, byte-identical to
        # deadpool's, so kl_ue4.c takes the same spelling.
        "ue4_sdk_int": True,
        # Inherited by CONSTRUCTION from deadpool, not measured here: same
        # engine branch, same OnlineSubsystemOculus, and the same two calls are
        # in libUE4's undefined set — the SYNCHRONOUS ovr_PlatformInitializeAndroid
        # followed by ovr_GetLoggedInUserID, with `FOnlineSubsystemOculus::
        # InitWithAndroidPlatform()` and "Unable to get a valid UserID. Check
        # that Oculus is running, you have an Entitlement…" both in the string
        # table. deadpool ForceQuit on the 0 answer; this says the synthetic
        # platform reports itself UP and answers with a declared local user, the
        # same carve-out as the app's own entitlement (kl_ovrplat.c). Everything
        # that DELIVERS content still refuses. If a first-boot log shows the
        # game never asks, this key can come back off.
        #
        # DRIVER CROSSING 2026-10-09: driver has no `plat_init` column; the
        # knob is the row env `KL_PLAT_INIT=1`, read once by kl_ovrplat.c with
        # the identical behaviour (deadpool's crossing). Round 2 measured it
        # INERT here (OnlineSubsystem is GooglePlay -> NULL, Oculus never
        # starts) — it stays only because the confirmed 08-24 build ran with
        # it.
        #
        # MULTIVIEW CHANGES AT THE CROSSING, deliberately (deadpool's note
        # applies word for word: same engine branch, VULKAN guest). The fnaf
        # branch cleared UE4's mobile-multiview flags unconditionally, so all
        # four device rounds ran DoubleWide; driver leaves them alone for a
        # Vulkan guest, so this build renders the path the title shipped with,
        # which is the layered shape kl_fove foveates. `KL_UE4_MULTIVIEW=0`
        # (env file) restores the old clear.
        #
        # EYE ALIGN was first left unset: the eye is 4032x3234 (3234 is not a
        # multiple of 4) and UE4's scene target is 5088x3236 — but that is
        # deadpool's exact shape (same branch, same overlapping-viewport
        # packing, the scene is WIDER than the eye regardless), and kl_fove's
        # +0..16 px intermediate slack is what engaged foveation on deadpool
        # with no align.
        #
        # ...but without the align foveation did NOT engage here —
        # `fove high 0%` in all 171 [perf] windows, `encoders marked 0`, `eye
        # LOADs left unfoveated` 28738. The pass was recognised (`intermediate
        # CONFIRMED … via subpass`, `EYE PASS (marked)`, 3 subpasses, attachment
        # #1 IS the runtime eye image) but its render AREA is 5088x3234 over
        # 5088x3236 attachments (UE4 rounds the scene up to a multiple of 4, the
        # area follows the 3234-row viewport). MoltenVK only CLEARs an
        # attachment when the area covers it whole; otherwise it opens the
        # encoder with LOAD and clears by draw — so subpass 0's encoder LOADs
        # the eye with nothing foveated pending (kl_fove's `late` rule) and the
        # whole chain stays unfoveated. RE4's fix applies: KL_OVRP_EYE_ALIGN=4
        # makes the eye 4032x3232, UE4's scene 5088x3232, area == extent.
        # Foveation high 0.30/0.08 (as on deadpool, batman, pistolwhip) baked
        # with it. Revert from the env file: KL_FOVEATION=off and/or
        # KL_OVRP_EYE_ALIGN=1.
        #
        # KL_OVRP_EYE_MAX=4352, confirmed on the headset: eye 4352x3488, map
        # 5472x3488, fove high 22%, refused 0. Guest GPU p50/p99 6.5/7.9 ms (4032: 6.8/
        # 8.7) against the guest's own 13.9 ms (72 Hz) period, total p50/p90
        # 11.2/11.6 ms, panel held 120 Hz with no demotion, [hb] peak 4583M
        # (avail low 3608M) under the 6144 budget. Revert from the env file:
        # KL_OVRP_EYE_MAX=4032.
        "env": "KL_PLAT_INIT=1 KL_OVRP_EYE_ALIGN=4 KL_OVRP_EYE_MAX=4352 KL_FOVEATION=high KL_FOVE_BOX=0.30 KL_FOVE_FLOOR=0.08",
    },
    "pinballfx": {
        # Pinball FX VR — Zen Studios' com.zenstudios.PFXVRQuest, versionCode
        # 6048 / versionName 1.8, project PinballFX_VR, Shipping, ASTC.
        #
        # **The first UNREAL 5 title here, and that is the whole story of the
        # row.** The manifest says EngineVersion 5.3.2 / EngineBranch UE5, and
        # the library carries `C:/svn/YUP_engine/UE5_3_2_MetaFork/…` paths —
        # a Meta fork of 5.3.2, not stock Epic. Between UE4.27 (deadpool) and
        # UE5, Epic renamed the Java package: every class the guest asks for by
        # name is `com.epicgames.unreal.*` where deadpool's and re4's are
        # `com.epicgames.ue4.*`, the entry library is `libUnreal.so` rather
        # than `libUE4.so`, the exported natives are
        # `Java_com_epicgames_unreal_GameActivity_native*`, and the manifest
        # meta-data keys are `com.epicgames.unreal.GameActivity.*`. That is a
        # RENAME, not a new door: the NativeActivity boot, the onCreate/onResume
        # transcription and every AndroidThunkJava_* name are unchanged.
        #
        # So the `kind` stays `ue4` deliberately. It names the DOOR (kl_ue4.c's
        # NativeActivity sequence), not the engine's marketing number, and
        # every behaviour keyed on KL_GUEST_UE4 — the KL_EXIT_ON_BACKGROUND=0
        # bake in kl_app.c above all, which cost deadpool a whole round — is
        # wanted here verbatim. A KL_GUEST_UE5 kind would have silently opted
        # out of all of it. The package dialect is DETECTED in kl_ue4.c from
        # which native prefix the entry library actually exports, and the JNI
        # surface folds `com/epicgames/unreal/` onto `com/epicgames/ue4/` at
        # lookup, so the four JNI files keep one spelling of each table row.
        "libs":    None,
        "srcdir":  "pinballfx/lib/arm64-v8a",
        "tree":    "pinballfx",
        "apk":     "pinballfx.apk",
        "assets":  "pinballfx/assets",
        "qtplugins": "",
        # bUseExternalFilesDir=true, bPackageDataInsideApk=false,
        # bHasOBBFiles=true — and unlike deadpool this title HAS its OBBs:
        # main.com.zenstudios.PFXVRQuest.obb (4.0 GB) and
        # overflow1.com.zenstudios.PFXVRQuest.obb (837 MB), plus 28 loose
        # pakchunk*-Android_ASTC.pak files (~3.0 GB) in the SAME directory.
        # 7.8 GB in total. Note the OBB names carry NO version code where every
        # other target's do (`main.1661.com.beatgames…`) — the Meta store's
        # spelling, and a thing UE5's own obb search may not match.
        "obb":     "Android/obb/com.zenstudios.PFXVRQuest",
        # 119 MB, named by the manifest's `android.app.lib_name = Unreal`.
        "entry":   "libUnreal",
        "kind":    "ue4",
        "product": "KleptonPinballFX",
        # Plain game name, the convention (deadpool/re4/Beat Saber).
        "display": "Pinball FX",
        # It models the player's hands (and the plunger/flipper grips) itself.
        "own_hands": True,
        # mkicon.py's source art and back-layer colour (icons/, gitignored).
        # Unlike deadpool's, this source needs NO keying pass: it is already a
        # 512x512 disc on transparency, so content_bbox trims to the disc and
        # the safe box insets it inside the Home View's circular mask with the
        # back layer showing as a rim.
        #
        # iconback is 29,29,29 — the dark bevel immediately inside the ring,
        # sampled from the art itself (20 px down from top centre). The
        # corner-sample rule deadpool's row describes does not apply here:
        # this source's corners are ALREADY transparent, so there is no keyed
        # -out field to take the colour from. Charcoal rather than black, so
        # the ring's own grey (106,106,106) still separates from the disc.
        "icon":     "icons/pinball.png",
        "iconback": "29,29,29",
        # **`full`, and the tester's own report is what settled it.** This row
        # shipped `mixed` (the default) through rounds 3-6 deliberately, so the
        # first launches could answer the question rather than have it answered
        # for them. Round 7 got the answer, on 2026-08-21: the loading screen
        # came up with "intermittent flashes of a distant room, higher in my
        # view". That is the room — PORTING.md's own warning, verbatim
        # (".mixed immersion invites the room into the picture"). Pinball FX VR
        # is a fully enclosed arcade; there is nothing for passthrough to be
        # part of, and a layer that does not cover the whole view has the
        # kitchen behind it. `full` is the house style for every other opaque
        # guest here and it is what this one wanted all along.
        #
        # Deliberately NOT `own_hands`: this guest models a table, not a pair
        # of gloves, and nothing in any round's log or report says the system's
        # upper limbs were a second pair of anything.
        "immersion": "full",
        # UE5.3's GameActivity keeps 4.27's spelling: the smali declares
        # nativeSetAndroidVersionInformation as
        # (Ljava/lang/String;ILjava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V
        # — String, int SDK_INT, then four Strings. Verified against THIS APK's
        # com/epicgames/unreal/GameActivity.smali, line 15099.
        "ue4_sdk_int": True,
        # Same stock platform gate as deadpool's, and asserted from the same
        # evidence rather than from a boot: libUnreal DT_NEEDEDs
        # libovrplatformloader.so directly, carries all 512 `ovr_*` platform
        # entry points, and exports AndroidThunkJava_ForceQuit. That is
        # UE's OnlineSubsystemOculus in its stock shape — platform init, then
        # ovr_GetLoggedInUserID, then quit when the id is 0. If a boot ever
        # shows this title does NOT gate, the honest move is to drop the key,
        # not to keep it because it is harmless.
        #
        # CROSSED ONTO DRIVER 2026-10-09 (foveation round 1): `plat_init` was a
        # table column on the deadpool/pinballfx branches; driver carries
        # per-target policy as the row's space-separated `env`, applied by
        # kl_target_use (the caller's own environment and the device env file
        # still win). The pinballfx-only setenv bakes that lived in kl_app.c
        # moved here with it, unchanged in value:
        #   KL_PLAT_INIT=1              the platform gate (was plat_init)
        #   KL_OVRP_EYE_MAX=4640        round 12: union wide enough that each
        #                               canted eye lands on the panel's ~3660
        #   KL_OVRP_EYE_CAP_ALLOC=1     round 9: the cap bounds the ALLOCATION
        #                               (asked width x union widening)
        #   KL_OVRP_LAYER_REC_DISPLAY=1 round 9: recommended resolution from
        #                               the display, post-cap
        #   KL_VK_MAX_SAMPLES=1         round 7/8: single-sampled framebuffers,
        #   KL_VK_SUBPASS_MS_DEMOTE=1   the pair that makes the cooked MS
        #                               subpass-depth fetch compile on Metal
        # DROPPED as driver defaults/superseded: KL_OVRP_DROP_NO_EYE=1 (driver
        # default on), KL_OVRP_QUAD_SIZE_OFF=0xbc (driver probes 0xb0 then 0xbc
        # for plausibility — deadpool's fix, which finds this title's 0xbc).
        #
        # Multiview: driver leaves UE's mobile multiview alone for a Vulkan
        # guest; this title has always rendered multiview (an Array eye layer,
        # gl_ViewIndex in its cooked shaders). KL_UE4_MULTIVIEW=0 is the
        # regression knob.
        #
        # Foveation baked (docs/FOVEATION.md) —
        #   KL_FOVEATION=high KL_FOVE_BOX=0.30 KL_FOVE_FLOOR=0.08
        # as on Pistol Whip / AS2 / Batman / Deadpool. This title needed two
        # runtime fixes to engage at all: UE5's entry-point names in the MSL
        # patch, and the render-area snap for a 1-row-short render area.
        # Revert without a build: KL_FOVEATION=off in the device env file (it
        # wins).
        # KL_OVRP_EYE_MAX stays 4640 — the measured ceiling (memory floor
        # 986 MiB under 6144 at 4640; each eye already renders 3633 across a
        # ~3660 panel).
        #
        # KL_WARP=system — visionOS does the
        # one exact warp (Simpsons r17, Pistol Whip, PTTR r21). Launch 2 had a
        # warm pipeline cache, so the panel stayed at 120 Hz (launch 1's cold
        # compile storm had demoted it to 60, where the guest filled 300 of
        # 300 presents and nothing was ever held). At 120 the guest delivered
        # 178-283 of every 300 (total GPU p50 9.1 ms > 8.33; host side perfect:
        # `1.00 periods`, `0 past deadline`), so 6-40% of slots showed a HELD
        # frame through our rotation-only flat-proxy pass — no translation
        # correction, and a pinball table at ~1 m is the worst case for that
        # residue (`[rp]` worst 4.4 mm eye travel = ~0.17 deg at 1.5 m, more
        # nearer). Fresh/held alternation = the tester's "slight jitter when I look
        # left or right, in either eye equally"; foveation (22%) is incidental.
        # Revert without a build: KL_WARP=ours in the device env file.
        #
        # KL_GUEST_FPS_DIV=2 — an even 60 fps
        # grid on the 120 Hz panel. Launches 3/4 proved the jitter was 120 Hz
        # PACING, not foveation or the warp: under KL_WARP=system, foveation on and off,
        # the guest ran 84-107 fps on a ragged 1x/2x hold beat (11-40% held),
        # and a frame's AGE alternating 1/2 slots is what showed on head turns.
        # KL_DISPLAY_HZ cannot pace a UE guest (it only changes the rate the
        # guest is TOLD; ovrp_WaitToBeginFrame is released every panel slot);
        # this publish gate is the knob. Launch 5 (env file): `pace grid 2
        # slots`, ~80 s at a table with `holds 2x:150` only, 0-1 overran, 0
        # valve, 60.0 fps, anchor misses 0 — the tester: "pinball. fixed."
        # Guest GPU at a table 8.9/9.2/9.6 ms p50/p90/p99 against 16.67.
        # Revert without a build: KL_GUEST_FPS_DIV=off in the device env file.
        #
        # KL_OVRP_EYE_MAX 4640 -> 4992. The pacing run
        # showed the game renders the FULL per-eye share at a table (no DRS
        # dip) while the compositor asks 6888x5525 per view, so a cap bump is
        # real centre density. Launch 6 (env file): eye texture 4900x3160,
        # table viewport 3860x3158 (78.8%, DRS settled at the full share),
        # ~130 s at 60.0 fps, holds 2x with 0-5 overran per 150, valve 0,
        # anchor misses 0; guest GPU 9.0/9.4/9.7 ms p50/p90/p99, busy 38-42%,
        # longest CB p99 14.4-15.7 of 16.67; peak 5251 MiB (893 under 6144).
        # The tester: "pinball was very smooth. probably bake it." 5376 does NOT
        # fit: the longest-CB tail would cross the 16.67 ms grid.
        # Revert without a build: KL_OVRP_EYE_MAX=4640 in the device env file.
        #
        # DRIVER PROMOTION 2026-10-11: KL_OVRP_USER_EYE_HEIGHT=standing keeps
        # this title's ovrp_GetUserEyeHeight2 answer (the standing height under
        # every origin, the round-10/11 lobby fix). Driver's default is the
        # fnaf lineage's origin-relative answer (0 under EyeLevel).
        "env": "KL_PLAT_INIT=1 KL_OVRP_EYE_MAX=4992 KL_OVRP_EYE_CAP_ALLOC=1 "
               "KL_OVRP_LAYER_REC_DISPLAY=1 KL_VK_MAX_SAMPLES=1 "
               "KL_VK_SUBPASS_MS_DEMOTE=1 "
               "KL_FOVEATION=high KL_FOVE_BOX=0.30 KL_FOVE_FLOOR=0.08 "
               "KL_WARP=system KL_GUEST_FPS_DIV=2 "
               "KL_OVRP_USER_EYE_HEIGHT=standing",
    },
    "pttr": {
        # PAINT THE TOWN RED VR — Unity **2021.3.31f1** + IL2CPP, package
        # com.southeastgames.paintthetownredvr, versionCode 61 (1.1.1). Read
        # off the APK, and every answer puts it on the same road The Last
        # Clockwinder took:
        #
        #   - classic com.unity3d.player.UnityPlayerActivity, no libgame.so:
        #     entry libmain, kind unity.
        #   - OVRPlugin, not OpenXR: boot.config says
        #     xrsdk-pre-init-library=OculusXRPlugin, xr-meta-enabled=1, and
        #     libOculusXRPlugin.so + libOVRPlugin.so both ship.
        #     libopenxr_loader.so ships too and is expected to go unused —
        #     the same shape as clockwinder, so believe boot.config, not the
        #     library list.
        #   - The manifest declares `android.hardware.vulkan.version` with no
        #     `required` attribute. That is NOT a Vulkan game: Unity emits the
        #     bare feature tag for the GLES build too, boot.config names no
        #     Vulkan pre-init, and the activity is even
        #     hardwareAccelerated="false". kl_glfb display path until the
        #     guest itself says otherwise.
        #   - No handtracking feature at all — controller-only (this is a
        #     brawler; both hands hold weapons). own_hands anyway, because the
        #     player's arms are modelled and drawn.
        #
        # Split application binary: a 41 MB assets/bin/Data stub in the APK and
        # main.61.com.southeastgames.paintthetownredvr.obb (1.85 GB) beside it,
        # named from apktool.yml's versionCode + the package. Nothing beyond
        # the OBB to stage.
        #
        # Audio is FMOD Studio (libfmod, libfmodstudio) + libresonanceaudio —
        # the same stack clockwinder brought up (System.loadLibrary by its real
        # name, and the FMOD object-vs-type JNI lookup, are both already fixed).
        #
        # Riders left to translate by default, watched rather than pre-empted:
        # libbhaptics-native-lib (bHaptics vest SDK — the manifest even
        # `queries` com.bhaptics.player, which is not installed, so the Java
        # side should decline on its own), libRF_CNative_andr, libUnityMockHMD,
        # lib_burst_generated, the Pico multi-store set
        # (libPicoAmbisonicDecoder, libPicoSpatializer, libpxrplatformloader)
        # and libovrplatformloader. assets/lib/arm64-v8a additionally carries
        # Meta's perf logger (libxplat_perflogger_*, libdouble-conversion) —
        # those are under assets/, not lib/, so the Makefile never sees them;
        # they load, if at all, through the guest's own dlopen of the extracted
        # path.
        #
        # Same excluded libraries as RE4/hardbullet/batman/clockwinder
        # (libfrda, libscript, libfrda.config.so); GUEST_EXCLUDED drops them.
        "libs":    None,
        "srcdir":  "pttr/lib/arm64-v8a",
        "tree":    "pttr",
        "apk":     "pttr.apk",
        "assets":  "pttr/assets",
        "qtplugins": "",
        "obb":     "obb",
        "entry":   "libmain",
        "kind":    "unity",
        "product": "KleptonPTTR",
        "display": "Paint the Town Red",
        "icon":     "icons/painttown.png",
        "iconback": "0,0,0",
        # A bar/arena the player stands inside; own arms and weapons modelled.
        "immersion": "full",
        "own_hands": True,
        # The shipping config, as ROW ENV (the 2026-10-09 driver crossing; it
        # was per-target setenv in kl_app.c before, and driver took the
        # MECHANISMS of the 08-26 promotion but not pttr's defaults):
        #   KL_VK_LAZY_TRANSIENT=1  transient attachments onto the memoryless
        #                           type (+ the INPUT_ATTACHMENT drop, same
        #                           gate) — what buys the 4032 rung's memory.
        #   KL_DISPLAY_HZ=72        the rate the guest asked for (round 18).
        #   KL_WARP=system          visionOS does the one exact warp (round 21).
        # Already driver defaults, so NOT repeated here:
        # KL_OVRP_POSE_BOUNDARY=cycle, CoreHaptics off the frame thread.
        # KL_GUEST_FPS_DIV stays unset (round 23 defaulted it off).
        #   KL_FOVEATION=high KL_FOVE_BOX=0.30 KL_FOVE_FLOOR=0.08
        #                           foveation, confirmed on the headset: EYE
        #                           PASS marked, fove high 22%, refused 0 /
        #                           UNMAPPED 0. KL_FOVEATION=off in the env
        #                           file reverts.
        #   KL_FOVE_INTERMEDIATE=0  levels (Disco, Prison: Unity's HDR/LDR
        #                           eye-size intermediates) render UNFOVEATED;
        #                           the hub's direct eye pass stays foveated.
        #                           Unfoveated levels looked better on the
        #                           headset than foveated ones at floor 0.08;
        #                           a gentler map may bring level foveation
        #                           back. KL_FOVE_INTERMEDIATE=1 in the env
        #                           file is the A/B.
        #   KL_OVRP_EYE_SCALE_MIN=1.0  floor under Unity's per-level render
        #                           scale (Prison asks 0.87): the eye swapchain
        #                           keeps 4032x3234 across level loads, so the
        #                           Prison load REUSES the eye textures instead
        #                           of overlapping a RETIRED generation. Launch 7
        #                           (2026-10-10, hub/Disco/hub/Prison, tester: no
        #                           problem): Prison-load peak 7836 -> 6366 MB,
        #                           Prison sharper (+32% px). KL_OVRP_EYE_SCALE_MIN=0
        #                           in the env file reverts.
        #   KL_OVRP_EYE_MAX=4352    one rung over the driver default 4032
        #                           (this row overrides it): 4352x3491/eye,
        #                           +16.5% px. Launch 8 (2026-10-10, hub +
        #                           BikerBar, tester: "yes, much better"):
        #                           peak 6350 MB / avail low 1841 of 8191,
        #                           every SetupLayer after the first two
        #                           reused, no RETIRED; BikerBar guest ~20.5 ms
        #                           / ~36 fps unfoveated. 4608 is NOT a rung
        #                           for this title: Prison combat projects
        #                           ~39 ms / ~20 fps. KL_OVRP_EYE_MAX=4032 in
        #                           the env file reverts.
        "env": "KL_VK_LAZY_TRANSIENT=1 KL_DISPLAY_HZ=72 KL_WARP=system "
               "KL_FOVEATION=high KL_FOVE_BOX=0.30 KL_FOVE_FLOOR=0.08 "
               "KL_FOVE_INTERMEDIATE=0 KL_OVRP_EYE_SCALE_MIN=1.0 "
               "KL_OVRP_EYE_MAX=4352",
    },
    "steamlink-vr": {
        # BOTH front doors, because the app runs both: the 2D shell pairs in a
        # WindowGroup and hands off to the OpenXR half in an ImmersiveSpace, in
        # one process (an app bundle cannot re-exec the way `build/m_boot` does).
        # 
        # Three groups, and the third is the one that is easy to leave out:
        #  libvrlink_scene   the VR door. ONE library — its DT_NEEDED is
        #                     entirely Android system libraries we shim.
        #   the shell chain   fourteen, dependencies first, off libshell's own
        #                     DT_NEEDED (runtime/guest/kl_slink.c's CHAIN_SHELL).
        #   the Qt plugins    six, in NOBODY's DT_NEEDED. Qt dlopens them by
        #                     path at runtime — the platform QPA first, and
        #                     libshell aborts without it. A dlopen that finds no
        #                     translation falls through to the mmap ELF loader,
        #                     which is exactly the RWX-from-an-unsigned-file
        #                     shape AMFI exists to refuse.
        "libs":    "libvrlink_scene "
                   "libc++_shared libSDL3 libSDL3_image libSDL3_mixer libSDL3_ttf "
                   "libQt6Core_arm64-v8a libQt6Network_arm64-v8a libQt6Gui_arm64-v8a "
                   "libQt6Widgets_arm64-v8a libQt6Svg_arm64-v8a "
                   "libh264bitstream libhevcbitstream libsteamwebrtc "
                   "libshell_arm64-v8a "
                   "libplugins_platforms_qvirtual_arm64-v8a "
                   "libplugins_iconengines_qsvgicon_arm64-v8a "
                   "libplugins_imageformats_qgif_arm64-v8a "
                   "libplugins_imageformats_qico_arm64-v8a "
                   "libplugins_imageformats_qjpeg_arm64-v8a "
                   "libplugins_imageformats_qsvg_arm64-v8a",
        "srcdir":  "steamlink-vr/lib/arm64-v8a",
        "tree":    "steamlink-vr",
        "apk":     "steamlink-vr.apk",
        "assets":  "steamlink-vr/assets",
        # The six plugin .so files ALSO go into the container, as ELF, and not
        # as a loader path — kl_load_auto still resolves each of them to its
        # signed framework by basename, so nothing maps guest text from here.
        # 
        # **Qt reads a plugin as a FILE before it will load it.** libQt6Core's
        # search is a glob (`libplugins_%1_*.so`), so it lists the directory and
        # then parses each candidate's ELF metadata for the IID and the Qt
        # version. A directory of names the loader resolves is enough for
        # everything else in this project and is not enough for that: with no
        # real files nothing is ever a candidate, and libshell aborts with
        # `Could not find the Qt platform plugin "virtual"`.
        # 
        # Six files, 544 KB — not the whole 75 MB tree, because these are the
        # only libraries anything reads rather than loads.
        "qtplugins": "steamlink-vr/lib/arm64-v8a",
        # The VR door's library. The shell's fourteen load on top of it in the
        # same process; this is the one whose translation being in the bundle
        # says the Steam Link guest was embedded.
        # Where this guest looks for its OBB, relative to the external-storage
        # root (kl_jni_files_dir()). Unity asks Java for it —
        # Context.getObbDirs() answers <files>/obb — so every Unity target here
        # shares one layout and only the version code in the NAME differs.
        "obb":     "obb",
        "entry":   "libvrlink_scene",
        "kind":    "steamlink",
        # The userdata KEY, which is the row name everywhere else. It is spelled
        # out here because this directory holds the PAIRING CREDENTIAL: it is
        # named "steamlink" on disk, and keying it on the row instead would point
        # a working install at an empty directory — which presents as the Steam
        # host having deauthorized us and costs a re-pair to disbelieve.
        "userdata": "steamlink",
        "product": "KleptonSteamLink",
        "display": "Klepton Steam Link",
        "icon":     "icons/steam link.png",
        "iconback": "0,0,0",
    },


    # ---- SDL2 + OpenXR VR engine ports (SCAFFOLD) ----
    #
    # Four targets over two engines that Klepton has not run before, added as
    # rows so each BUILDS, stages and boots to a first log; the engine drivers
    # (kl_xash / kl_source) are not written yet, so the "xash"/"source" kinds
    # currently share the NativeActivity harness (kl_native) — enough to load the
    # guest libraries and print DT_NEEDED, the shim gap and where the real entry
    # is (SDL_main / drbeef's GLES3JNILib), which is what the drivers get built
    # against. None boots the game yet; that is the per-engine work these rows set
    # up. No OBB (the shared "obb" = nothing-here answer); the whole data set is
    # the loose tree the user staged under userdata/<name>/<stage_files>.
    #
    #   Xash3D / GoldSrc:
    "cs1": {
        # Counter-Strike VR (com.lvonasek.csvr) — Xash3D + SDL2, entry libxash
        # (SDL_main). Data: userdata/cs1/xash (valve + cstrike) -> android-files/xash.
        "libs":    None,
        "srcdir":  "cs1/lib/arm64-v8a",
        "tree":    "cs1",
        "apk":     "cs1.apk",
        "assets":  "cs1/assets",
        "qtplugins": "",
        "obb":     "obb",
        "stage_files": "xash",
        "entry":   "libxash",
        "kind":    "sdl2",
        "product": "KleptonCSVR",
        "display": "Counter Strike",
        # In-world title: its menu is a full stereo scene too, so .mixed leaves
        # it a window floating in passthrough. (Upstream: kl_app_target_wants_full.)
        "immersion": "full",
    },
    "hl1": {
        # Half-Life VR / Lambda1VR (com.drbeef.lambda1vr) — Xash3D, drbeef's
        # GLES3JNILib exports (same shape as jkxr), entry libxash. Data:
        # userdata/hl1/xash (valve + HLGOLD + commandline.txt) -> android-files/xash.
        "libs":    None,
        "srcdir":  "hl1/lib/arm64-v8a",
        "tree":    "hl1",
        "apk":     "hl1.apk",
        "assets":  "hl1/assets",
        "qtplugins": "",
        "obb":     "obb",
        "stage_files": "xash",
        "entry":   "libxash",
        "kind":    "gles3jni",
        "product": "KleptonHalfLifeVR",
        "display": "Half Life",
        # In-world title: its menu is a full stereo scene too, so .mixed leaves
        # it a window floating in passthrough. (Upstream: kl_app_target_wants_full.)
        "immersion": "full",
    },
    #   Source engine:
    "hl2": {
        # Half-Life 2 VR (com.valvesoftware.source.vrtest) — Source + SDL2
        # (org.libsdl.app.SDLActivity), entry liblauncher (SDL_main), libsourcevr
        # for the XR seam. Data: userdata/hl2/srceng (hl2, ep2, episodic, platform)
        # -> android-files/srceng.
        "libs":    None,
        "srcdir":  "hl2/lib/arm64-v8a",
        "tree":    "hl2",
        "apk":     "hl2.apk",
        "assets":  "hl2/assets",
        "qtplugins": "",
        "obb":     "obb",
        "stage_files": "srceng",
        "entry":   "liblauncher",
        "kind":    "sdl2",
        "product": "KleptonHalfLife2VR",
        "display": "Half Life 2",
        # In-world title: its menu is a full stereo scene too, so .mixed leaves
        # it a window floating in passthrough. (Upstream: kl_app_target_wants_full.)
        "immersion": "full",
    },
    "portal": {
        # Portal VR (com.portalVR.source) — Source + SDL2, entry liblauncher.
        # Data: userdata/portal/Source (portal, hl2, platform) -> android-files/Source.
        "libs":    None,
        "srcdir":  "portal/lib/arm64-v8a",
        "tree":    "portal",
        "apk":     "portal.apk",
        "assets":  "portal/assets",
        "qtplugins": "",
        "obb":     "obb",
        "stage_files": "Source",
        "entry":   "liblauncher",
        "kind":    "sdl2",
        "product": "KleptonPortalVR",
        "display": "Portal",
        # In-world title: its menu is a full stereo scene too, so .mixed leaves
        # it a window floating in passthrough. (Upstream: kl_app_target_wants_full.)
        "immersion": "full",
    },
}

# What a target that says nothing gets: the shape every target had before this
# key existed, so Beat Saber and Steam Link are untouched by it.
IMMERSION_DEFAULT = "mixed"

DEFAULT = "beatsaber"


        # `"libs": None` means "whatever this tree would translate for that srcdir",
        # which the Makefile already decides — the .so files that are really ELF, minus
# the ones we REPLACE (libOVRPlugin, libovrplatformloader, libvrapi) and the
# ones that are not part of the application (libfrda, libscript). Asking it
# rather than restating it keeps one answer: a second copy of those rules here
# would drift on the next guest, and the failure mode is a bundle missing a
# library, which on device is a dlopen that cannot fall back to anything.
# 
# It is a hard failure rather than a fallback to a pinned list, for the same
# reason: a quietly incomplete bundle is worse than a build that stops.
def libs_for(srcdir):
    import subprocess, os
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    out = subprocess.run(["make", "-s", "guestlibs-list", f"LIBS={srcdir}"],
                         cwd=root, capture_output=True, text=True)
    libs = out.stdout.strip()
    if out.returncode != 0 or not libs:
        print(f"!! could not discover the guest libraries in {srcdir!r} "
              f"(make guestlibs-list said: {out.stderr.strip() or 'nothing'})",
              file=sys.stderr)
        sys.exit(1)
    return libs


# A bundle id is GLOBAL, so the one in the table is not usable as it stands.
# 
# An App ID can be registered to exactly one team, and automatic signing tries to
# register one on the first build. So the second person to build this tree does
# not get a warning, they get a signing failure that names neither the cause nor
# the fix:
# 
#   error: Failed Registering Bundle Identifier: the app identifier
#         "dev.klepton.app" cannot be registered to your development team
# 
# ...and because both memory entitlements need an EXPLICIT App ID, that failure
# takes them down with it — which is the thing that stopped the
# loading-transition kills on device (see ENTITLEMENTS in gen_xcodeproj.py). One
# person's registration is enough to block everyone else's build of the same
# commit.
# 
# So the account building it supplies the front of the id. $USER is the right key
# because it is per-machine-account, always set in a login shell, and needs no
# configuration at all to be different for different people.
# 
# The rest is DERIVED from the target name rather than stored per target:
#
#     <user>.dev.klepton.target.<target>
#
# One shape for every target, so a new one needs no id invented for it and the
# table cannot hold an id that disagrees with the key above it. (The three that
# used to be there did exactly that — `dev.klepton.app`, `dev.klepton.steamlink`
# and `dev.klepton.target.superhot` were three conventions for three targets.)
# 
# Sanitised, because a bundle id may hold only alphanumerics, hyphens and
# periods: anything else becomes a hyphen, and a scope that sanitises to nothing
# (or an unset USER, as in some CI) leaves the id unscoped rather than emitting
# a leading dot. KLEPTON_BUNDLE_SCOPE overrides the key; KLEPTON_BUNDLE_ID still
# overrides the whole id, everywhere, and is how an ALREADY-INSTALLED app's
# identity is kept — changing this orphans the container the assets were staged
# into, so the first build after it re-stages (run.sh's stamp is keyed on the id,
# which is what makes that automatic rather than a silent empty container).
BUNDLE_PREFIX = "dev.klepton.target"


def bundle_for(name):
    import os, re
    scope = os.environ.get("KLEPTON_BUNDLE_SCOPE", os.environ.get("USER", ""))
    scope = re.sub(r"[^A-Za-z0-9-]", "-", scope).strip("-").lower()
    ident = re.sub(r"[^A-Za-z0-9.-]", "-", f"{BUNDLE_PREFIX}.{name}")
    return f"{scope}.{ident}" if scope else ident


def resolve(name):
    if name not in TARGETS:
        print(f"!! unknown target {name!r} — one of: {', '.join(sorted(TARGETS))}",
              file=sys.stderr)
        sys.exit(1)
    t = dict(TARGETS[name])
    t["name"] = name
    # Derived from the target name, EXCEPT where a row states its own — and a
    # row that states one means it. An id is not a naming preference: it is the
    # identity iOS installs against, so changing it does not rename an app, it
    # installs a SECOND one beside the first.
    t["bundle"] = TARGETS[name].get("bundle") or bundle_for(name)
    if t["libs"] is None:
        t["libs"] = libs_for(t["srcdir"])
    t.setdefault("immersion", IMMERSION_DEFAULT)
    t.setdefault("own_hands", False)
    # Empty rather than absent, so every consumer sees the same keys whether or
    # not the row bothered to state them — a shell that evals this table cannot
    # tell a missing assignment from an empty one, and `set -u` can.
    t.setdefault("obb_extra", "")
    # Same treatment: absent and empty are indistinguishable to a shell that
    # evals this table under `set -u`.
    t.setdefault("gamedata", "")
    # ...and WHERE that tree lands on the guest's side of /sdcard. It used to be
    # the literal "srceng" inside stage_assets.sh, which was the table in a
    # fifth place: Simpsons Hit & Run hardcodes /storage/emulated/0/SimpsonsHitRun
    # (libmain's own strings) and would have been staged into hl2vr's directory
    # name and reported "missing game files" with the 1.9 GB sitting right there.
    t.setdefault("gamedest", "")
    # SDL door only — see kl_target.h. Absent on every other kind.
    t.setdefault("sdl_main", "")
    # ...and the environment the row bakes in — see `env` below and in
    # kl_target.h. Empty for almost every row.
    t.setdefault("env", "")
    # The device bundle's guest set, when it is narrower than `libs` (a row
    # whose device build keeps fewer libraries than its host/simulator one;
    # gen_xcodeproj.py applies it). Empty = the device embeds `libs`.
    t.setdefault("device_libs", "")
    # CFBundleShortVersionString. Per-target because the targets ship
    # independently; the default is the "1.0" every row carried when this was
    # a constant in gen_xcodeproj.py, so no existing target moves.
    t.setdefault("version", "1.0")
    # The scene manifest's immersion style and the app's runtime half of the
    # same decision (opaque_world in kl_target.h) cannot see each other —
    # one is read by the build, the other by the app. Deriving the runtime
    # half here is what keeps them in step.
    t["opaque_world"] = t["immersion"] == "full"
    return t


# The runtime's copy of the table, as an X-macro list. Only the fields C needs
# are here: the visionOS product/bundle/display are the BUILD's business and the
# app already knows which one it is by the time it runs, and `libs` is discovered
# per build rather than being a property anything at runtime should believe.
# 
# The DEFAULT is emitted too, so `build/m_boot` with no argument and an app built
# with no -DKL_TARGET_DEFAULT agree about which guest that means.
def c_table():
    out = [
        "// GENERATED by visionos/targets.py — do not edit. Regenerate: make targets",
        "//",
        "// The target table, as the runtime sees it. visionos/targets.py is the",
        "// authority (it also carries the bundle id, product and display name, which",
        "// only the build needs); this is its C half, generated so that the two",
        "// cannot disagree about which tree, which APK or which boot sequence a",
        "// target means. See runtime/kl_target.h for the struct these expand into.",
        "",
        "// KL_TARGET_ROW(name, tree, apk, assets, libdir, entry_lib, userdata, obb,",
        "//                kind, own_hands, opaque_world, ue4_sdk_int, sdl_main, env)",
        "#ifndef KL_TARGET_ROW",
        "#error \"include this through runtime/kl_target.c\"",
        "#endif",
        "",
    ]
    for name in sorted(TARGETS):
        t = dict(TARGETS[name])
        t.setdefault("immersion", IMMERSION_DEFAULT)
        t.setdefault("own_hands", False)
        t.setdefault("ue4_sdk_int", False)
        t.setdefault("env", "")
        kind = "KL_GUEST_" + t["kind"].upper()
        # `userdata` defaults to the row name — one profile per target is the
        # rule, and a row only states it to KEEP a directory a previous naming
        # created (steamlink-vr).
        # NULL rather than "" for a row with no SDL door: the field is a
        # pointer the door dereferences only on its own kind, and an empty
        # string there would read as "the guest exports a function called
        # nothing" rather than as "this row has no opinion".
        sdlmain = ('"%s"' % t["sdl_main"]) if t.get("sdl_main") else "NULL"
        if any(c in t["env"] for c in '"\\'):
            raise SystemExit(f"!! {name}: env {t['env']!r} has a quote or a "
                             "backslash in it — it is emitted as a C string "
                             "literal, so that would not survive the trip")
        out.append('KL_TARGET_ROW("%s", "%s", "%s", "%s", "%s", "%s", "%s", "%s", %s, %d, %d, %d, %s, "%s")'
                   % (name, t["tree"], t["apk"], t["assets"], t["srcdir"],
                      t["entry"], t.get("userdata", name), t["obb"], kind,
                      1 if t["own_hands"] else 0,
                      1 if t["immersion"] == "full" else 0,
                      1 if t["ue4_sdk_int"] else 0,
                      sdlmain,
                      t["env"]))
    out += ["", '#define KL_TARGET_DEFAULT_NAME "%s"' % DEFAULT, ""]
    return "\n".join(out)


def main(argv):
    if len(argv) > 1 and argv[1] == "--list":
        for k in sorted(TARGETS):
            print(k)
        return 0
    if len(argv) > 1 and argv[1] == "--c-table":
        print(c_table())
        return 0
    name = argv[1] if len(argv) > 1 and argv[1] else DEFAULT
    t = resolve(name)
    if len(argv) > 2:
        if argv[2] not in t:
            print(f"!! no field {argv[2]!r}", file=sys.stderr)
            return 1
        print(t[argv[2]])
        return 0
# Shell-sourceable. shlex.quote so a value with spaces (`libs`) or a shell
# metacharacter (an apostrophe in a display name like "Assassin's Creed Nexus")
# survives `eval` intact instead of splitting the assignment or leaving an
# unbalanced quote.
    for k, v in t.items():
        print(f"KLT_{k.upper()}={shlex.quote(str(v))}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
