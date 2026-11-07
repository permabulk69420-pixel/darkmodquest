/* The Dark Mod GPL Source Code. See LICENSE.txt for license terms. */
#pragma once
#include <chrono>

class idCVar;
extern idCVar vr_rendererDiagnostics;

// Render-thread counters, reset before stereo preparation. Text is published
// under a mutex after both eye passes; the HUD never reads these directly.
struct QuestRenderCounters {
	int fogSurfaceDraws = 0;
	int fogLightPasses = 0;
	int blendFogPasses = 0;
	int volumetricLights = 0;
	int ssaoPasses = 0;
	int softParticleDraws = 0;
	double prepareCpuMs = 0, depthCpuMs = 0, lightingCpuMs = 0;
};
extern QuestRenderCounters questRenderCounters;

class QuestCpuScope {
	using Clock = std::chrono::steady_clock;
	double &total;bool enabled;Clock::time_point start;
public:
	QuestCpuScope(double &value,bool measure):total(value),enabled(measure) { if(enabled) start=Clock::now(); }
	~QuestCpuScope() { if(enabled) total+=std::chrono::duration<double,std::milli>(Clock::now()-start).count(); }
};

inline const char *QuestScissorStatus( bool enabled, long long fullArea, int checks, int mismatches ) {
	if ( !enabled ) return "OFF";
	if ( mismatches ) return "FAIL: GL state mismatch";
	if ( !fullArea || !checks ) return "IDLE: no sampled lighting draw";
	return "VERIFIED: GL test and rectangle";
}
