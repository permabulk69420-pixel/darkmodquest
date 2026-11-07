# Renderer research and independently operated validation

The performance process needs a fixed workload, an identified cost, a single
controlled change and repeated measurements. Android compilation, linked shader
status and operation counters are correctness checks, not proof of an FPS gain.

## Findings from The Dark Mod and its PC VR port

Sources inspected on 2026-11-07:

The PC VR project's README targets TDM 2.10 and describes development as on hold.
Its notes are historical guidance, not a benchmark of TDM 2.14, current PC
hardware or Quest. Compare the implementation before transferring techniques.

- [The Dark Mod VR source](https://github.com/fholger/thedarkmodvr), especially
  `renderer/vr/OpenXRBackend.cpp` and `VRFoveatedRendering.cpp`. The port builds
  visibility once, updates eye matrices for replay, keeps the UI in a separate
  layer and updates shadow maps on one eye. Its current source still loops over
  two eyes. Its NVIDIA shading-rate image and radial-density reconstruction
  paths are different APIs and techniques from Quest's EXT/QCOM shading rates.
- [PC VR mission notes](https://github.com/fholger/thedarkmodvr/wiki/Notes-on-missions-(compatibility,-performance,-issues))
  explicitly name **Briarwood Manor's exterior** as a severe performance case,
  reporting reprojection even on powerful PC hardware. The built-in missions are
  described as more manageable. Briarwood is a useful stress case, but should be
  accompanied by a more typical scene.
- [PC VR performance tips](https://github.com/fholger/thedarkmodvr/wiki/Performance-tips)
  recommend mapped shadows, disabled soft shadows and disabled SSAO. The reported
  Quest settings already follow those recommendations. The guide's optional
  resolution reduction is outside this task's requirements.
- [Light scissors issue #9](https://github.com/fholger/thedarkmodvr/issues/9) and
  [visibility issue #38](https://github.com/fholger/thedarkmodvr/issues/38) show why
  tighter scissors and stereo visibility need corner/near-plane correctness
  tests. [Lightgem issue #29](https://github.com/fholger/thedarkmodvr/issues/29)
  also demonstrates that visual similarity alone does not establish gameplay
  lightgem correctness.
- [Hidden-area mesh issue #8](https://github.com/fholger/thedarkmodvr/issues/8)
  reports minimal benefit in the author's tested stencil-shadow path. This is
  not proof that the technique is useless on Quest's mapped-shadow pipeline,
  and is not evidence to promise another performance gain.
- [TDM performance documentation](https://wiki.thedarkmod.com/index.php?title=Performance_Tweaks)
  describes frontend model-job parallelism, including TDM 2.14's chunking mode.
  This checkout already defaults to `r_useParallelAddModels 2`, with SMP enabled.
  Merely setting these values again is not an optimization.
- [Profiling](https://wiki.thedarkmod.com/index.php?title=Profiling) and
  [Tracy timeline profiling](https://wiki.thedarkmod.com/index.php?title=Tracy:_timeline_profiler)
  explain the frontend/backend/worker timeline. `WaitForFrontend` and
  `Frontend::Wait` distinguish which CPU stage waits for the other. Nested zones,
  parallel jobs and GPU timings must not be added together as independent frame
  costs. This Android build disables Tracy with `_NO_TRACY`; the stock Linux
  release includes it.

## What runs in this cloud environment

The environment has no exposed GPU device, KVM, USB device or connected ADB
headset. The retained Android emulator boots using CPU emulation and software
graphics. Its earlier logs advertise GLES 3.0/3.1; the game requires GLES 3.2.
That emulator is not a valid Quest GPU performance benchmark.

The stock **TDM 2.14/64 #11208 Linux x86-64** engine runs real missions using
Mesa llvmpipe. Its automation connection and matching **Tracy 0.10.0** capture
and statistics tools have been exercised here. This supplies real-world scene
and CPU-workload investigation. It is a reference engine, not the Android APK or
its mobile lighting backend; its FPS and driver timings cannot establish Quest
performance or validate Meta's OpenXR compositor.

The existing GLES tests execute the shipping mobile/multiview shaders and
production framebuffer/scheduling code separately. They remain necessary for
those paths, but are not full-mission or Qualcomm performance tests.

## Pinned Briarwood scene

The mission package was downloaded from the game's official mission index:

- Mission: `briarwood_manor`, index version 17 (package description version 1.93).
- Author: Chris (NeonsStyle) Kilgariff.
- Map: `briarwood`.
- Package SHA-256:
  `0bc991696886a66617eb40c87744ad2a1f3ee549ba246e618078fa3c3f5c81a4`.
- Fixed player position and angles: `-1834 -1354 -26.5 0 90 0`.
- Reference output size: 1280x720; this is the local PC window, not a change to
  the Quest eye-buffer resolution.

Runtime downloads, profiles and screenshots are stored under
`/tmp/renderer-tools/research/`. No game files or profiling SDKs are committed.

## Automation operated by the developer

`bin/tdm_automation.py` implements the engine's existing framed TCP protocol.
It checks framing, reply sequence numbers and message sizes. It supports console
commands, status queries and the existing mission/input actions. No new engine
API, controller binding or player overlay was added.

For this prepared environment, a reproducible reference launch is:

```bash
LD_LIBRARY_PATH=/tmp/renderer-tools/root/usr/lib/x86_64-linux-gnu \
PATH=/tmp/renderer-tools/root/usr/bin:$PATH \
LIBGL_ALWAYS_SOFTWARE=1 ALSOFT_DRIVERS=null \
xvfb-run -a -s '-screen 0 1280x720x24' \
  /tmp/renderer-tools/tdm-game/thedarkmod.x64 \
  +set fs_basepath /tmp/renderer-tools/tdm-game \
  +set fs_savepath /tmp/renderer-tools/research/game-saves \
  +set fs_modSavePath /tmp/renderer-tools/research/game-saves/fms \
  +set fs_currentfm briarwood_manor \
  +set r_fullscreen 0 +set r_mode -1 \
  +set r_customWidth 1280 +set r_customHeight 720 \
  +set r_multiSamples 0 +set r_swapInterval 0 \
  +set s_noSound 1 +set com_automation 1 +set com_automation_port 3881 \
  +set tdm_player_wait_until_ready 0 +set logFile 2 +map briarwood
```

The isolated save tree must contain the mission under `fms/briarwood_manor`;
it can link to the downloaded package directory. Keep log output in a directory
on the game's search path. The stock engine's `GetConsoleContents` assumes it
can reopen its log; a mismatched save/search path caused console requests to
terminate the reference process during setup.

After loading:

```bash
python3 bin/tdm_automation.py --port 3881 --query status
python3 bin/tdm_automation.py --port 3881 \
  --command 'setviewpos -1834 -1354 -26.5 0 90 0' \
  --command 'r_shadows 2' --command 'r_softShadowsQuality 0' \
  --command 'r_ssao 0' --command 'r_volumetricEnable 0' \
  --command 'com_enableTracing 1'
```

Use an X11 window capture for the local reference image. The engine's screenshot
command produced empty images with this software-driver configuration; those
files must not be accepted as valid visual captures.

`bin/profile_tdm_scene.py` captures baseline/candidate/candidate/baseline (ABBA)
runs through the same game process, with two seconds of settling per run. It
verifies the changed cvar, records graphics/scene/camera/settings metadata,
checks that the camera and scene stay fixed, exports raw zone statistics and
restores the original settings. It refuses to overwrite an existing output
directory. Each comparison changes one existing numeric runtime cvar.

`--frontend-only` explicitly skips the rendering backend. This isolates CPU
frontend investigation from llvmpipe rendering and is not a full-frame benchmark.
The capture/statistics tools must match the engine's Tracy version.

```bash
LD_LIBRARY_PATH=/tmp/renderer-tools/research/capstone/unpacked/capstone/lib \
python3 bin/profile_tdm_scene.py --port 3881 \
  --capture /tmp/renderer-tools/research/tracy-0.10/capture/build/unix/capture-release \
  --export /tmp/renderer-tools/research/tracy-0.10/csvexport/build/unix/csvexport-release \
  --output /tmp/renderer-tools/research/briarwood-frontend-abba-next \
  --setting r_useParallelAddModels --baseline 2 --candidate 0 \
  --seconds 8 --frontend-only
```

## Measurement check completed

A fixed-view ABBA comparison on 2026-11-07 captured four eight-second CPU-only
runs, with 486–488 frame markers in each. Camera and scene checks passed, and
all test cvars restored successfully.

| Mode | First run: R_RenderView | Second run: R_RenderView |
| --- | ---: | ---: |
| Existing parallel chunking (`2`) | 1.5318 ms/call | 1.5276 ms/call |
| Serial model processing (`0`) | 1.8957 ms/call | 1.9504 ms/call |

These are instrumented **per-view CPU zone means**, not frame times, FPS,
un-instrumented timings or Quest measurements. There are about three view calls
per frame, including subviews/lightgem. Do not multiply or sum nested/overlapping
zones to manufacture a frame time. The local result supports retaining the
existing parallel setting; it does not justify changing Quest defaults.

The first baseline profile recorded over 800,000 `AddActiveInteraction` calls
and over 460,000 `R_AddSingleModel` calls in eight seconds. Those are CPU function
invocations, not GL draw calls. They provide a concrete workload for investigating
frontend interaction processing, instead of assuming every slowdown comes from
fragment shading.

## Evidence required for another performance change

Keep the mission version, view, output dimensions, active settings and source
revision with every result. Start from a known baseline, warm the same scene,
change one mechanism, repeat both orders and inspect distributions and variance.
Use representative missions as well as Briarwood's stress case. Preserve shader,
stereo, scissor, depth, shadow and gameplay-lightgem correctness checks.

CPU algorithm changes can be investigated with real scene profiles and tested
with their production logic. Framebuffer bandwidth, FP16 throughput, effective
Quest shading rates, thermal behavior and delivered OpenXR frame timing require
an actual suitable device/driver. No such device is connected to this executor.
Software-driver results cannot certify those gains. Access to a test device
would allow developer-operated automation/capture; asking the player to collect
and hand over logs is not the validation workflow implemented here.
