# Quest 3 GLES renderer

The `ccr-f26da023-1l9zer` OpenXR integration remains responsible for tracking,
late eye poses, controllers, swapchains, foveation, the HUD overlay, and frame
submission. When OpenXR is active, `r_questRenderer` defaults to the new mobile
lighting pipeline. Flat rendering continues to use the existing backend.

## Frame work

Before either eye replay, `MobileLightingStage` walks the recorded views once:

1. Evaluate the existing material interaction groups, including conditions,
   animated textures, vertex colors, texture transforms, normal/specular maps,
   ambient cubemaps, and parallax settings.
2. Build surface batches containing up to two light contributions. Use the
   surface's original mesh when it can be identified unambiguously, and clip
   light volumes in the shader; this avoids requiring identical per-light
   triangle subsets. Fall back to the exact interaction mesh for ambiguous
   shared vertex allocations. Translucent interaction order is retained;
   subtractive stages are not batched or reordered across LDR clamps.
3. Allocate all visible shadow-casting lights in a bounded depth atlas. Every
   view has its own light entries, preserving view-specific shadow suppression.
   A view's entries are reused by both eyes, including mirrors and portal views.
4. Render global and local shadow casters into separate regions of that atlas.
   Empty local-caster layers consume no atlas pages or shadow samples.
   Each caster has a conservative cube-face mask computed from frame-safe
   bounds, so irrelevant faces never receive its draw. Cube-face projection uses
   ordinary GLES clipping, without geometry shaders or clip-distance extensions.
   Directional lights use a fitted orthographic projection.

Each eye then uses its existing depth pass and current OpenXR view matrices to
shade the prepared batches. Material texture fetches and parallax displacement
are shared between the two lights in a batch. Specular/view-dependent shading,
SSAO, soft particles, fog, volumetrics, and post-processing remain per eye.
Volumetrics read the same shared shadow atlas. UI rendering and the lightgem
PBO/readback protocol are unchanged; lightgem lighting retains hard shadows and
its gamma convention.

This removes the second eye's shadow generation and material packet preparation.
A two-light surface batch replaces two interaction draws and two sets of material
fetches with one. Surface shading still runs for each eye; this is not a claim
that all stereo GPU work is eliminated. No multiview extension is required.

## Quality and memory

The default atlas is 2048 x 2048, with 24-bit depth (up to 16 MiB with a 32-bit
physical depth allocation). This replaces the legacy 6144 x 6144 default atlas
for the mobile lighting path. Shadow faces start at up to 256 pixels, with
smaller pages for lights of smaller projected coverage and for the lightgem.
When necessary, packing reduces face resolution instead of dropping lights or
silently disabling shadows. Each light reserves separate global/local caster
regions so `noselfshadow` receivers work without modifying the atlas between
eye renders. Alpha-tested caster materials are evaluated in the depth shader.

Shadow filtering is bounded four-tap PCF, rather than the desktop renderer's
variable-cost blocker search/contact-hardening filter. `r_softShadowsQuality 0`
still requests hard shadows. The desktop soft-shadow radius/sample-count controls
do not select the mobile filter kernel. Very crowded scenes can have coarser
shadows; `r_questShadowAtlasSize` (512–4096) and `r_questShadowFaceSize` (32–512)
are launch-time quality controls. The defaults prioritize standalone frame time
without lowering eye-buffer resolution or removing material effects.

World/depth calculations and shadow samplers retain high precision, including
when `harm_r_useMediumPrecision` is set. This includes the separate depth prepass:
lowering only that pass makes the forward pass fail its `GL_EQUAL` tests and
leave black patches. Depth and interaction vertex programs declare invariant
positions to preserve their agreement across driver compiler optimizations.
Directional shadow comparisons use a geometric receiver-plane bias scaled to
the atlas sampling footprint; a fixed epsilon causes sloped surfaces to shadow
themselves at mobile resolutions. Every active light sampler has a complete
texture binding, including unused second-light slots.
Android depth attachment upload types
are valid GLES types. The Android Release target explicitly uses `-O2`; NDK r23's
legacy CMake setup previously allowed a Release build with no optimization flag.

## Validation

The native check uses the repository's NDK 23.2.8568313 and Android 21 ARM64
configuration, with the extracted `org.khronos.openxr:openxr_loader_for_android:1.0.34`
AAR's `prefab` directory:

```sh
python3 bin/check_quest_renderer.py \
  --ndk /path/to/android-ndk-r23c \
  --openxr-prefab /path/to/extracted-openxr-aar/prefab
```

Use an existing checkout; cloud tasks already have an isolated environment.
Use a fresh build directory when changing NDK, ABI, or Android API level so that
CMake does not reuse incompatible feature checks from dependencies. The helper
requires OpenXR compile definitions and an optimized ARM64 engine linked against
`libopenxr_loader.so`; a build of the stub backend cannot pass.

Host regression checks:

```sh
g++ -std=c++14 -O2 -I. tests/renderer/mobile_plan_test.cpp -o /tmp/mobile-plan-test
/tmp/mobile-plan-test
EGL_PLATFORM=surfaceless python3 tests/renderer/mobile_gles_test.py
python3 bin/pack_tdm_shaders.py --check
python3 tests/renderer/shader_install_test.py
```

The GLES tests require EGL and a GLES 3.2 driver (Mesa software rendering works).
They compile/link the actual shipping shaders and render small offscreen targets.
They check two-light accumulation, independent light textures, light-volume and
mirror clipping, eye-specific specular shading, all six shadow faces, directional
shadows (including self-shadow rejection on sloped receivers), local/global
caster masks, cutout shadows, and volumetric shader linking. The depth/forward
integration check uses `GL_EQUAL` with nonintegral near/distant geometry and
asymmetric eye projections.
They do not emulate the headset compositor or measure Quest frame times.

The install check requires a JDK and compiles the production Java ZIP extraction
and file-copy method bodies, with only Android asset access and logging stubbed.
It verifies a fresh installation, an upgrade with the old shader directory
layout, and a ZIP without explicit directory entries. This catches a packaging
failure that isolated shader compilation cannot: missing parent directories
previously stopped extraction before the new mobile shaders were installed.
The APK workflow runs this check before building. Set `SHADER_INSTALL_APK` to
an APK path to check its bundled archive. `TDM_SHADER_INSTALL_DIR` can retain the
fresh install; set `TDM_SHADER_SOURCE_DIR` to its `glslprogs` directory when
running the GLES checks to render from installed files.

The mobile frame preparation requires both mobile programs to link before any
lighting draws. A missing or failed shader reports an error instead of replaying
draws with the depth program still active and producing black surfaces.

`bin/pack_tdm_shaders.py` regenerates the Android shader archive deterministically.
The shader patch version was bumped so existing installations extract the new
sources. The Quest APK CI verifies that this archive matches all source shaders.
When changing shader sources in future, regenerate the archive and bump the
shader patch version in `Q3EGameConstants.java` before shipping.

## On-device performance

For the 72 Hz target the application frame budget is **13.89 ms**. The existing
`vr_showPerf` HUD and OpenXR CPU/GPU timer now include shared preparation and
shadow rendering. `r_questShowRenderStats 1` additionally reports view preparation,
light packets versus batches, shared shadow draws, and actual lighting draws per
frame. To compare the old path, launch with `+set r_questRenderer 0`; it is an
initialization setting, so comparisons require a restart. Existing resolution,
foveation, refresh-rate preferences, and controls are retained.

Stable 72 FPS, thermal behavior, visual parity in missions, mirrors, and stealth
lightgem values require a Quest 3 run. Native build checks and offscreen rendering
checks establish build/shader correctness, not a measured headset speedup. No
headset measurement or APK installation is claimed by these host checks.
