/* The Dark Mod GPL Source Code. See LICENSE.txt for license terms. */
#pragma once
#include "renderer/backend/MobileRenderPlan.h"

// Hardware variable-rate shading changes fragment frequency, never the eye
// image dimensions or the depth/stencil sample grid. Only lighting uses it.
class MobileFoveation {
public:
	void BeginFrame();
	void BeginLighting(const MobileRenderPlan::Rect &viewport, const float *leftProjection,
		const float *rightProjection, int samples, bool mainView);
	bool UsesDrawRates() const { return drawing && !mapped; }
	void Draw(const MobileRenderPlan::Rect &bounds);
	void EndLighting();
	void Shutdown();
	void GetDiagnostics(char *buffer, int size) const;

private:
	using RateFunction = void (*)(unsigned);
	using CombinerFunction = void (*)(unsigned, unsigned);
	using AttachmentFunction = void (*)(unsigned, unsigned, unsigned, int, int, int, int);
	using QueryFunction = void (*)(int, int, int *, unsigned *);
	RateFunction rate = nullptr;
	CombinerFunction combine = nullptr;
	AttachmentFunction attachment = nullptr;
	QueryFunction query = nullptr;
	bool initialized = false, drawing = false, mapped = false, qcom = false;
	bool mapFailed = false;
	unsigned mapTexture = 0, currentRate = 0;
	int tileSize = 0, mapWidth = 0, mapHeight = 0, cachedLevel = 0, cachedSamples = 0;
	int level = 0, reducedDraws = 0, fullDraws = 0, mappedPasses = 0;
	int gpuWidth = 0, gpuHeight = 0;
	MobileRenderPlan::Rect viewport, cachedViewport;
	float centres[2][2] = {}, cachedCentres[2][2] = {};
	void Init();
	bool BindMap(int samples);
	void SetRate(unsigned value);
};
