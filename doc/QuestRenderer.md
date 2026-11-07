# Quest 3 GLES renderer

The `ccr-f26da023-1l9zer` OpenXR integration remains responsible for tracking,
late eye poses, controllers, swapchains, the HUD overlay, and frame
submission. When OpenXR is active, `r_questRenderer` defaults to the new mobile
lighting pipeline. Flat rendering continues to use the existing backend.

`r_questMultiview` defaults to 1. On `GL_OVR_multiview2` hardware, the scene uses
two-layer color/depth-stencil targets. A single depth submission per surface and
a single lighting submission per batch produce both eyes, using `gl_ViewID_OVR`
to select the late OpenXR eye matrices and eye-dependent shading. The command
loop advances both eyes together rather than replaying the complete scene twice.
OpenXR still acquires/submits its existing eye swapchains and HUD layer.

The multiview vertex and fragment sources explicitly declare
`#pragma tdm_define "QUEST_MULTIVIEW"`. The engine resolves supplied defines only
at those declarations; passing a dictionary entry alone does not define it.
Earlier APKs omitted these declarations, linked ordinary one-eye shaders and
then attempted two-eye matrix uploads. Shader-link success and the submitted
multiview draw count could therefore accompany missing geometry and lighting.
The installed shader version is now 12, forcing replacement of older sources.
Before selecting the multiview path, the renderer verifies second-eye modelview,
projection and lighting-origin uniform locations for every program. Incompatible
programs retain separate-eye rendering. Checks are cached by shader generation
and repeated after reload, without adding per-draw GL queries.

Selecting a multiview scene updates both the GL binding and
`FrameBufferManager::currentRenderFbo`. Lighting stages bind that recorded target
on entry. Previously, binding only the layered GL FBO left the recorded target on
the last single-eye layer, so lighting immediately switched to that layer and
lost the intended stereo output. `MobileMultiviewTargets::BindScene` owns this
transition for the initial clear, depth and lighting. Selecting an individual eye
restores that eye's render target before material/effect passes.

This path requires MSAA off (0 or 1 sample). Foveation can remain enabled with
multiview. Requested MSAA, unsupported multiview extensions,
cutscenes and render-dependent textures used directly in lighting interactions
retain the existing rendering path. The existing diagnostics pipeline row shows
actual multiview versus separate-eye lighting passes; a setting alone does not
claim multiview is running. Mirror, camera and portal-sky capture images have
separate storage per eye, including their dimensions, format and swizzle. Reloads,
resizing and retirement invalidate that storage. SSAO remains computed per eye,
then copied into matching array layers for multiview lighting. Ordinary material,
refraction, fog, volumetric and post-processing passes retain eye-specific draws;
this does not claim every rendering pass has been converted to multiview.

## Tile memory, copies and precision

With SSAO off, shared scene depth and lighting are consecutive draws to the same
layered framebuffer. Per-eye depth capture now runs after lighting, immediately
before the eye's material/effect passes. Soft particles and refraction still
receive current-view depth. With SSAO enabled, depth capture and AO stay before
lighting because ambient lighting needs their results. Captures and subview
ordering remain unchanged.

After each eye's material/effect passes finish, the scheduler invalidates only
that eye's primary depth and stencil attachments, before leaving the target.
The resolved depth texture remains intact for readers, and colour remains valid
for captures, bloom and tonemapping. The next world view clears its own depth.
The GLES core loader now exposes and loads framebuffer invalidation independently
of the desktop ARB extension. Real GLES checks discard one eye's depth while the
other eye still needs it, verifying both resulting colour images.

At matching scene/output dimensions, tonemapping can sample an eye layer directly
from the existing colour array into the OpenXR eye swapchain. The intermediate
primary-to-GUI colour blit is removed. Both sampler types explicitly use high
precision. Sharpening, compression, gamma and dithering use the same shader body.
Bloom-produced images retain their existing path; explicit FBO scaling retains
the original resampling/CAS order. Stale or failed array programs fall back to
the 2D path, with interface checks cached by program generation. Forced black
output needs no scene copy. HUD rendering remains in its separate XR layer.

The scene BRDF uses mediump only for bounded normalized directions, diffuse
angular terms, Fresnel/rim factors and vertex-colour lighting weights. Direction
normalization, glossy half-vector dot/power, positions, transforms, shadow
coordinates/tests, parallax, HDR/material colours and light accumulation retain
high precision. The blanket mobile/depth precision exemption remains in place.
The gameplay lightgem selects dedicated full-precision variants using the
original interaction equations, hard-shadow policy and gamma handling.

`mobile_bandwidth_gles_test.py` compares direct array tonemapping against the
actual colour-blit/2D path for both eyes and postprocess settings. Its BRDF checks
compare the mixed shader, original equations and an additional variant with
explicit half-float assignment rounding. Across the fixed lighting/view/normal
cases, the maximum observed colour difference is 1/255. This does not guarantee
identical Quest driver precision or establish GPU cycle savings. Production
scheduler fixtures check capture/effect/discard order with SSAO on/off, direct
output, bloom, scaling, shader rejection and forced-black output. Android ARM64
link checks and real GLES multiview/depth/material tests pass. On-headset FPS
improvement remains unmeasured; eye resolution is unchanged.

## Fixed foveation

The launcher's **Foveated lighting** levels now use hardware variable-rate
shading (VRS), without changing eye-buffer resolution, depth/stencil sampling,
shadow resolution or UI detail. Off remains the default; Medium is the starting
point for comparison. It no longer disables multiview.

On drivers exposing `GL_EXT_fragment_shading_rate` and its attachment extension,
lighting uses an immutable R8UI spatial rate map. Each tile takes the finest rate
required by either eye's optical centre, derived from the late asymmetric eye
projections. Coordinates include the framebuffer viewport offset. Map generation
is cached by dimensions, viewport, level, sample count and projection centres.
Supported rates are queried for the active sample count. Low requests 1x2 outside
the protected centre; Medium requests 2x2; High permits 4x2 at the outer corners.
Unsupported rates retain a finer supported rate. Depth/stencil coverage and tests
remain per pixel. Map binding/combining is limited to the main lighting pass.

On Quest drivers exposing only `GL_QCOM_shading_rate`, a conservative draw-rate
path uses the intersection of projected geometry bounds and existing light
scissors. Any rectangle touching either eye's protected centre stays at 1x1.
Bounds are cached per geometry for each view replay. This adds no draws, texture
fetches or shader branches. Wide surfaces crossing the centre retain full rate,
so savings are more limited than a spatial map and depend on the scene. The
driver may choose a finer effective rate than requested. Mirror, camera, portal
sky, lightgem, shadow, material/effect and UI passes retain full-rate shading.
The controller resets the hardware rate and detaches maps before leaving lighting.

This replaces the old `GL_QCOM_texture_foveated` path completely. That extension
scales depth/stencil along with colour and warns that resolved depth can cause
artifacts when reused. This renderer copies depth and switches targets between
depth, lighting and per-eye effects; simply foveating the primary colour texture
therefore cannot guarantee consistent depth across passes. Its old incomplete-FBO
fallback also tried to clear an irreversible foveation-enable texture bit. The
new path uses ordinary colour/depth targets throughout; unsupported hardware or
rejected rate maps retain full-rate rendering. This addresses those failure
mechanisms; it does not establish the exact cause of every reported black bar.

The existing diagnostics overlay reports OFF, UNAVAILABLE, SPATIAL VRS, DRAW VRS
or idle. For QCOM draw rates it also reports coarse/total lighting draw requests
and their percentage; spatial-map drivers report map passes instead. Draw
fractions are not pixel coverage: occlusion and screen area affect the savings.
A low coarse-draw fraction can explain a small benefit even at High, because
large meshes crossing the centre remain at full rate. CPU-limited scenes can
also show little FPS improvement from less fragment shading.

A one-time startup probe requests 2x2 shading and reads the GPU's own
`gl_FragSizeEXT` or `gl_ShadingRateEXT` into a four-byte result. The HUD reports
`2x2 request -> 2x2`, a finer response such as `1x1`, or `unverified` if the GLSL
interface cannot be measured. This uses a 16x16 single-sample temporary target;
it demonstrates the effective rate for that test, not the per-fragment rate of
every scene program or framebuffer configuration. A driver may select different
rates under actual scene conditions. The probe restores physical FBO bindings,
program, VAO, renderbuffer, viewport, scissor, colour masks, GL enables, pixel-pack
settings and PBO binding, leaving engine caches consistent. It runs once when
foveation initializes, adds no per-frame readback and exports no files.

`mobile_foveation_test.py` compiles the production controller with recorded
vendor entry points and real GLES target/map allocation. It checks both-eye
centre protection, offsets, map encoding, capability rejection, renderer restart,
draw percentages and GL state restoration. `mobile_vrs_probe_test.py` checks the
production probe's actual GLES execution and state restoration using a constant
encoded-result shader fixture. It separately runs the embedded vendor shader;
Mesa without the required interface must report unverified, not invented coarse
shading support. These checks do not establish Qualcomm hardware results. Mesa
multiview image tests cover the unchanged depth, lighting and material handoff.
Actual Quest rate results, visual quality and FPS gains remain device measurements.

## Frame work

Before either eye replay, `MobileLightingStage` walks the recorded views once:

1. Evaluate the existing material interaction groups, including conditions,
   animated textures, vertex colors, texture transforms, normal/specular maps,
   ambient cubemaps, and parallax settings.
2. Build surface batches containing up to eight light contributions. Lights
   share at most two projection/falloff/ambient texture tuples, retaining the
   sixteen-sampler GLES limit. Every light keeps independent matrices, color,
   origin and shadow parameters. The frontend records each interaction's exact
   original mesh, including distinct surfaces that share a vertex allocation.
   Fused draws use that mesh with shader light-volume clipping; single-light
   draws retain their smaller per-light triangle subsets. Missing or stale source
   index buffers retain the interaction mesh. Translucent interaction order is retained;
   subtractive stages are not batched or reordered across LDR clamps. Positive
   compatible stages on one mesh are grouped even when originally interleaved.
   Batches reference a contiguous packet list instead of copying eight full
   material interactions into every batch.
3. Allocate visible shadow-casting lights in persistent atlas slots. Retain
   slots under camera movement; fill holes for new lights and repack only under
   pressure. A view's entries are reused by both eyes. Distinct views also reuse
   an entry when their complete light/caster signatures match; differing caster
   suppression and the lightgem retain separate entries.
4. Retain the final shadow depth per face in a single atlas. Unchanged faces
   require no drawing or copying. When a caster changes or leaves, clear the
   affected face and redraw its current casters. This avoids a second depth
   atlas, depth blits and render-target switches between static and dynamic
   layers on the tile-based GPU. Caster membership, geometry, world transforms,
   enabled alpha stages, alpha thresholds, texture matrices/reloads, light
   projection, shader reloads and buffer/storage recreation invalidate the
   corresponding cache immediately. Dynamic vertex buffers and animated images
   force a refresh; there is no movement classification or grace period.
   Clipped per-light index arrays carry allocation revisions through index
   references and frame-safe draw-surface snapshots, so temporary GPU reuploads
   do not defeat reuse and recycled index-array addresses do not create hits.
   Render global and local casters into separate regions of the atlas.
   Empty local-caster layers consume no atlas pages or shadow samples.
   Each caster has a conservative cube-face mask computed from frame-safe
   bounds, so irrelevant faces never receive its draw. Cube-face projection uses
   ordinary GLES clipping, without geometry shaders or clip-distance extensions.
   Directional lights use a fitted orthographic projection.

Multiview depth and lighting use the same CPU-composed modelview matrices and
invariant position calculations as the ordinary path, preserving `GL_EQUAL`.
Material texture fetches and parallax displacement
are shared between up to eight lights in a batch. Specular/view-dependent shading,
SSAO, soft particles, fog, volumetrics, and post-processing remain per eye.
Lighting scissors are rebuilt from immutable light bounds and each eye's late
view/projection matrices. They do not reuse the combined-view frontend scissors
disabled by the OpenXR replay. Near-plane intersections are clipped before
perspective division, and pixel bounds are rounded outward. Each batch uses the
union of its visible lights; offscreen lights are removed before state uploads,
and batches with no visible lights are skipped. Separated lights are split into
tight individual draws when their union exceeds the sum of their separate
rectangles, avoiding extra shading across a large gap between lights.
Overlapping lights retain shared material sampling.
Draws select a specialized one-, two-, four- or eight-light program, with constant
light-array indices. A one-light program eliminates the second texture bank and
unused light uniforms. Positive opaque batches are grouped by program capacity;
translucent and subtractive ordering is preserved. Uniform values are cached
inside these private program groups and reset with program reloads. Already
bound textures skip active-unit selection as well as the texture bind. These
changes remove repeated driver work without caching across unrelated programs.
Blend lights use the same eye-specific projection. Volumetric scissors are
scaled to each work FBO, with an extra border for every blur sample. Lighting
also requests early depth tests before material/parallax/shadow calculations.
Volumetrics read the same shared shadow atlas. UI rendering and the lightgem
PBO/readback protocol are unchanged; lightgem lighting retains hard shadows and
its gamma convention.

This removes the second eye's shadow generation, material packet preparation,
depth draw submissions and lighting draw submissions on the multiview path.
An eight-light batch can replace four former two-light draws with one. A warm
unchanged shadow face needs neither shadow draws nor depth copies. Surface
shading still runs for each eye in the GPU; multiview eliminates duplicate CPU
submissions without making the two eye images identical.

## Quality and memory

The default atlas is 2048 x 2048, with 24-bit depth (up to 16 MiB with a 32-bit
physical depth allocation). There is no second static-depth atlas.
This replaces the legacy 6144 x 6144 default atlas
for the mobile lighting path. Shadow faces start at up to 256 pixels, with
smaller pages for lights of smaller projected coverage and for the lightgem.
When necessary, packing reduces face resolution instead of dropping lights or
silently disabling shadows. Each light reserves separate global/local caster
regions so `noselfshadow` receivers work without modifying the atlas between
eye renders. Alpha-tested caster materials are evaluated in the depth shader.
`r_questShadowCache` defaults to 1; 0 forces fresh rendering for comparison.

Captured camera views whose pose does not follow the headset are rendered once
per frame when followed immediately by their unique image copy. The right eye
uses that same captured image. Mirror/portal-sky/player views, CPU/PBO copies,
ambiguous command ranges and multiply written images keep their stereo replays.
This does not change controller bindings or the OpenXR tracking/submission path.

Shadow filtering is bounded four-tap PCF, rather than the desktop renderer's
variable-cost blocker search/contact-hardening filter. `r_softShadowsQuality 0`
still requests hard shadows. The desktop soft-shadow radius/sample-count controls
do not select the mobile filter kernel. Very crowded scenes can have coarser
shadows; `r_questShadowAtlasSize` (512–4096) and `r_questShadowFaceSize` (32–512)
are launch-time quality controls. The defaults prioritize standalone frame time
without lowering eye-buffer resolution or removing material effects.

Quest VR atmospheric fog defaults to off (`vr_fog 0`). The launcher
has a **Fog and mist (slower)** checkbox to restore it on the next
game launch. This includes fog lights, atmospheric blend lights, fog sheets, and
fog particle stages (including Saint Lucia's `tdm_dustfog3`). Classification is
cached when content loads; generic smoke materials are not globally disabled.
Particle geometry retains its fog identity through model and `func_fx` emission
and is rejected per view before GPU uploads, so a shared lightgem/model cache
cannot reintroduce it or lose its original geometry. Disabling fog
also disables fog-based portal occlusion, so scenery beyond removed fog is still
drawn. Surface lighting, shadows, and the lightgem's fog rendering are retained.
Flat rendering keeps its existing fog settings. Weapon effects, flames, ordinary
smoke, opaque skyboxes and surface lighting stay enabled. The launcher always
passes the checkbox value, including the off default, over archived game config.
Volumetric lighting is independent: `r_volumetricEnable` and its existing game
menu option still control light shafts. Fog-off removes fog-classified volumes
but leaves ordinary lamps' volumetric lighting and their frustum buffers intact.

**Show renderer diagnostics overlay** is a separate launcher checkbox, off by
default. It adds a readable panel to the existing VR HUD, with no controller
binding. It reports the last completed stereo frame, actual eye resolution and
runtime refresh period, frame/GPU timing, mobile/legacy pipeline, loaded shader
status (including the first failed program), shadow build/draw counts, lighting
batch counts, and separate fog/volumetric/SSAO/soft-particle activity. Foveation
reports the backend's accepted state, and MSAA reports effective and requested
samples. Settings and observed work are labeled separately; an empty scene is
idle, not a failure. Fog rejection counts belong to individual frontend entity
jobs, so parallel frontend work does not race on a shared counter.

With diagnostics enabled, mobile scissors additionally sample `GL_SCISSOR_TEST`
and `GL_SCISSOR_BOX` once per rendered eye/view and compare the actual rectangle
with the one used for that lighting draw. `VERIFIED` requires a successful GL
sample; a cvar alone never produces that status. Rectangle area, culled batches,
and separated-light batch splits show whether scissoring is reducing work.
This is state/activity verification, not a claim that all scene visuals are
correct. The panel is published under a mutex; the HUD never reads live backend
counters or queries GL from the frontend. Switch it off for timing comparisons
without the panel's draw and state-query overhead.
The existing panel also shows retained/redrawn shadow faces and confirms the
absence of depth copies, prepared view types, reused camera captures, game and renderer-frontend
time from the same immutable command buffer, XR waiting, and separate render-CPU
preparation/depth/lighting costs. CPU scopes include driver stalls and are not
GPU stage timings. Frontend/backend time may overlap with SMP; do not sum it.

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
python3 tests/renderer/shader_preprocess_test.py
python3 tests/renderer/multiview_program_test.py
python3 tests/renderer/fog_policy_test.py
python3 tests/renderer/mobile_batch_test.py
python3 tests/renderer/mobile_submission_test.py
python3 tests/renderer/mobile_multiview_commands_test.py
python3 tests/renderer/mobile_multiview_targets_test.py
python3 tests/renderer/mobile_multiview_pipeline_test.py
g++ -std=c++14 -O2 -I Q3E/src/main/jni/darkmod tests/renderer/mobile_shadow_cache_test.cpp -o /tmp/quest-cache-test
/tmp/quest-cache-test
```

The GLES tests require EGL and a GLES 3.2 driver (Mesa software rendering works).
They compile/link the actual shipping shaders and render small offscreen targets.
Shader preparation compiles the production `FindNextPragmaInText`,
`ResolveIncludes`, `ResolveDefines` and token-splitting implementations; file I/O
and the define dictionary use host fixtures. Tests supply the same define map as
the engine, and do not inject multiview preprocessor lines into the GLSL. The
source-variant check fails for the previously released APK's shaders. A GLES
check verifies actual second-eye uniform locations in the linked depth program
and all four lighting capacities. The runtime-validation fixture checks mono,
missing and failed programs, reload invalidation and cached query counts.
The multiview regression runs `glFramebufferTextureMultiviewOVR` on a real driver
and compares both array layers against separate-eye depth/lighting renders for
all four light capacities, asymmetric eye poses, distinct SSAO, cutout rejection
and clipping. The target lifecycle test compiles the production stereo target
implementation, attachment helpers and image allocation method against real GLES,
checking mutable captures, independent eye layers, resize, reload, retirement and
FBO state restoration. The command test compiles the actual OpenXR scheduler and
checks single shared submissions, per-eye captures/backgrounds/effects, a single
lightgem execution and fallback views. These fixtures do not run the game or the
Meta compositor and do not measure Quest frame times.
The pipeline regression also compiles the production lighting-stage entry and
target implementation against real GLES. It reproduces missing eye lighting with
the old logical target, then compares the corrected images after layered depth,
separate-eye depth copies, shared lighting and later single-eye material draws.
It checks both colour and depth continuity rather than shader programs alone.
The eleven-frame cached global/local shadow sequence compares every output pixel
against fresh rendering through movement, disappearance, cutout changes and
storage invalidation: 49 reference draws become 35, with identical pixels and
no depth copies. This fixture establishes correctness, not comparative GPU time.
Specialized one/two/four/eight-light programs are compared against the full
program for both eye origins; the one-light program must eliminate bank-two
samplers and unused array slots. Submission fixtures run the production uniform
cache, texture binding and interaction preparation code with recorded GL calls,
checking state changes, deferred loads, statistics, reloads and exact source meshes.
Eight-light tests compare one combined draw with eight reference draws across
two texture tuples and both eye origins, allowing only framebuffer rounding.
CPU tests exercise the shipping batch rules, temporary index-buffer rotation,
cache invalidation and 1,000 changing atlas light sets. These are regression
checks, not on-headset frame-rate measurements.
They check two/eight-light accumulation, independent light textures, light-volume and
mirror clipping, eye-specific specular shading, all six shadow faces, directional
shadows (including self-shadow rejection on sloped receivers), local/global
caster masks, cutout shadows, and volumetric shader linking. The depth/forward
integration check uses `GL_EQUAL` with nonintegral near/distant geometry and
asymmetric eye projections.
Scissor tests compare complete rendered images against unscissored lighting for
both eyes, near-plane crossings, a camera inside a light, mirrored transforms,
and peripheral lights visible to only one eye. The CPU check independently
samples 10,000 transformed boxes to ensure visible points are never clipped.
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

The mobile frame preparation requires all four lighting programs and the shadow program to link before any
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
`r_questLightScissor` defaults to 1 and can be set to 0 for a comparison.
`r_questShowRenderStats 1` reports lighting scissor area, offscreen batch skips
and separated-light batch splits. Area is a bound on allowed fragment coverage,
not a measured GPU-time or FPS improvement.

Stable 72 FPS, thermal behavior, visual parity in missions, mirrors, and stealth
lightgem values require a Quest 3 run. Native build checks and offscreen rendering
checks establish build/shader correctness, not a measured headset speedup. No
headset measurement or APK installation is claimed by these host checks.
