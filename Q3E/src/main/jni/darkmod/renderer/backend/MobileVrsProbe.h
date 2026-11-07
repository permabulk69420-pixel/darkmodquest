/* The Dark Mod GPL Source Code. See LICENSE.txt for license terms. */
#pragma once
struct MobileVrsProbeResult { int width=0,height=0; };
// One startup readback, not a frame-time measurement. Zero means unverified.
MobileVrsProbeResult R_QuestProbeVrs(void (*setRate)(unsigned));
