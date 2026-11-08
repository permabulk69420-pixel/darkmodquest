// Deterministic clock, GL-query and Meta-runtime fixtures around production code.
#include "renderer/QuestFrameTiming.h"
#include <cassert>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

using GLuint = unsigned; using GLenum = unsigned; using GLsizei = int; using GLint = int;
enum { GL_EXTENSIONS, GL_TIME_ELAPSED_EXT, GL_QUERY_RESULT_AVAILABLE, GL_QUERY_RESULT };
static int64_t wall = 0, cpu = 0;
static int clockReads = 0;
int64_t FakeWall() { ++clockReads; return wall; }
int64_t FakeCpu() { return cpu; }
void Advance(double milliseconds) { wall += int64_t(milliseconds * 1e6); }
void Near(double a, double b) { assert(std::abs(a - b) < 0.00001); }

struct Query { bool ended = false; int ready = 0; uint64_t ns = 0; };
static std::map<GLuint, Query> queries;
static GLuint nextQuery = 1, activeQuery = 0;
static int gpuFrame = 0, delay = 3, disjoint = 0, begins = 0, resultsRead = 0;
static bool extension = true;
void Gen(GLsizei n, GLuint *out) { while (n--) { *out = nextQuery++; queries[*out++] = {}; } }
void Delete(GLsizei n, const GLuint *ids) { while (n--) queries.erase(*ids++); }
void Begin(GLenum, GLuint id) { assert(!activeQuery); activeQuery = id; ++begins; }
void End(GLenum) {
	assert(activeQuery); auto &q = queries.at(activeQuery);
	q.ended = true; q.ready = gpuFrame + delay; q.ns = uint64_t(30 + gpuFrame) * 1000000;
	activeQuery = 0;
}
void Available(GLuint id, GLenum, GLuint *value) { const auto &q = queries.at(id); *value = q.ended && q.ready <= gpuFrame; }
void Result(GLuint id, GLenum, uint64_t *value) {
	const auto &q = queries.at(id); assert(q.ended && q.ready <= gpuFrame); ++resultsRead; *value = q.ns;
}
using Proc = void (*)();
Proc eglGetProcAddress(const char *name) {
	if (!std::strcmp(name, "glGenQueriesEXT")) return reinterpret_cast<Proc>(Gen);
	if (!std::strcmp(name, "glDeleteQueriesEXT")) return reinterpret_cast<Proc>(Delete);
	if (!std::strcmp(name, "glBeginQueryEXT")) return reinterpret_cast<Proc>(Begin);
	if (!std::strcmp(name, "glEndQueryEXT")) return reinterpret_cast<Proc>(End);
	if (!std::strcmp(name, "glGetQueryObjectuivEXT")) return reinterpret_cast<Proc>(Available);
	if (!std::strcmp(name, "glGetQueryObjectui64vEXT")) return reinterpret_cast<Proc>(Result);
	return nullptr;
}
const unsigned char *qglGetString(GLenum) { return reinterpret_cast<const unsigned char *>(extension ? "GL_EXT_disjoint_timer_query" : "GL_OTHER"); }
void qglGetIntegerv(GLenum, GLint *out) { *out = disjoint; disjoint = 0; }
void XR_Log(const char *, ...) {}
#include "renderer/vr/OpenXRFrameTimer.h"
#include "renderer/vr/OpenXRPerfMetrics.h"

void PhaseCycle(QuestFrameTiming &t) {
	Advance(10);
	{ QuestFrameTiming::Scope s(t, QuestFrameTiming::FrontendWait); Advance(11); }
	{ QuestFrameTiming::Scope s(t, QuestFrameTiming::Upload); Advance(7); }
	{ QuestFrameTiming::Scope s(t, QuestFrameTiming::FenceWait); Advance(9); }
	{
		QuestFrameTiming::Scope s(t, QuestFrameTiming::Render); Advance(20);
		{ QuestFrameTiming::Scope w(t, QuestFrameTiming::XrWait); Advance(5); }
		{ QuestFrameTiming::Scope w(t, QuestFrameTiming::XrBegin); Advance(1); }
		{ QuestFrameTiming::Scope w(t, QuestFrameTiming::Acquire); Advance(4); }
		{ QuestFrameTiming::Scope w(t, QuestFrameTiming::LightgemReadback); Advance(3); }
		{ QuestFrameTiming::Scope w(t, QuestFrameTiming::Release); Advance(2); }
		{ QuestFrameTiming::Scope w(t, QuestFrameTiming::XrEnd); Advance(9); }
	}
	cpu += 24000000;
	t.Complete(true);
}
void TestTimeline() {
	QuestFrameTiming t(FakeWall, FakeCpu);
	const auto before = clockReads;
	{ QuestFrameTiming::Scope s(t, QuestFrameTiming::Render); }
	assert(clockReads == before); // disabled scopes don't read the clock
	t.Enable(true); PhaseCycle(t); assert(t.Since(0).count == 0); // warmup
	PhaseCycle(t); t.AttachGpu(2, 36);
	const auto r = t.Since(0);
	assert(r.first == 2 && r.last == 2 && r.count == 1 && r.rendered == 1 && r.gpuCount == 1);
	Near(r.wall, 81); Near(r.cpu, 24); Near(r.gpu, 36); Near(r.pairedWall, 81); Near(r.pairedCpu, 24);
	const double expected[] = {10,20,5,1,4,2,9,11,7,9,3};
	double total = 0;
	for (int p = 0; p < QuestFrameTiming::PhaseCount; ++p) { Near(r.phases[p], expected[p]); total += r.phases[p]; }
	Near(total, r.wall); // no missing milliseconds, no double-counted waits
	PhaseCycle(t); t.AttachGpu(2, 37); auto late = t.Since(0);
	assert(late.count == 2 && late.gpuCount == 1); Near(late.gpu, 37); // result stays with cycle 2
	t.InvalidateGpu(); assert(t.Since(0).gpuCount == 0);
	t.Enable(false); Advance(10000); t.Enable(true); PhaseCycle(t); assert(t.Since(0).count == 0);
	for (int i = 1; i <= 100; ++i) { Advance(i); t.Complete(i % 2); }
	auto percentile = t.Since(0); Near(percentile.p95, 95); Near(percentile.wall, 50.5);
	assert(percentile.rendered == 50);
	for (int i = 0; i < 600; ++i) { Advance(1); t.Complete(true); }
	t.AttachGpu(2, 999); assert(t.Since(0).count == 512 && t.Since(0).gpuCount == 0); // stale ID cannot corrupt a reused slot
	t.Enable(false);
	{ QuestFrameTiming::Scope s(t, QuestFrameTiming::Render); t.Enable(true); Advance(1); }
	t.Complete(true); Advance(2); t.Complete(true); Near(t.Since(0).phases[QuestFrameTiming::Engine], 2);
	t.Enable(false);
}
void TestGpuQueries() {
	QuestFrameTiming t(FakeWall, FakeCpu); OpenXRFrameTimer timer(t);
	timer.Init(); timer.Enable(true); gpuFrame = 0; begins = resultsRead = 0;
	for (int i = 0; i < 6; ++i) {
		gpuFrame = i; timer.BeginGpu(); Advance(81); timer.EndGpu(); cpu += 24000000; timer.Complete(true);
	}
	auto r = t.Since(0); assert(r.count == 5 && r.gpuCount == 2); Near(r.gpu, 31.5);
	// Clock disjoint invalidates every pending and unreported result, not one.
	disjoint = 1; gpuFrame = 6; timer.BeginGpu(); timer.EndGpu(); Advance(81); timer.Complete(true);
	assert(t.Since(0).gpuCount == 0);
	for (int i = 7; i < 10; ++i) {
		gpuFrame = i; timer.BeginGpu(); timer.EndGpu(); Advance(81); timer.Complete(true);
	}
	r = t.Since(0); assert(r.gpuCount == 1); Near(r.gpu, 36);
	// Pool exhaustion skips a sample without synchronously reading the GPU.
	timer.Reset(); timer.Enable(true); delay = 100; begins = resultsRead = 0;
	for (int i = 0; i < 30; ++i) {
		gpuFrame = i; timer.BeginGpu(); timer.EndGpu(); Advance(81); cpu += 24000000; timer.Complete(true);
	}
	assert(begins == 8 && resultsRead == 0 && t.Since(0).gpuCount == 0);
	char text[160], details[1100]; assert(timer.GetText(text, sizeof(text)) && timer.GetDetails(details, sizeof(details)));
	assert(std::strstr(text, "frame 81.0 ms") && std::strstr(text, "main CPU 24.0") && std::strstr(text, "GL span n/a"));
	assert(std::strstr(details, "GL matched 0/") && std::strstr(details, "wall n/a") && std::strstr(details, "includes idle"));
	timer.Enable(false); const int oldBegins = begins; timer.BeginGpu(); timer.EndGpu(); assert(begins == oldBegins);
	timer.Shutdown(); assert(queries.empty());
	// Exported function pointers alone do not prove the extension is supported.
	extension = false; timer.Init(); timer.Enable(true); timer.BeginGpu(); timer.EndGpu(); assert(queries.empty()); timer.Shutdown();
	extension = true; delay = 3;
}

static XrPath paths[] = {1,2,3,4,5,6};
static const char *names[] = {"/perfmetrics_meta/app/cpu_frametime", "/perfmetrics_meta/app/gpu_frametime",
	"/perfmetrics_meta/compositor/cpu_frametime", "/perfmetrics_meta/compositor/gpu_frametime",
	"/perfmetrics_meta/compositor/dropped_frame_count", "/perfmetrics_meta/compositor/spacewarp_mode"};
static bool metricsEnabled = false, failSet = false, failQuery = false;
static int metricQueries = 0, sets = 0, metricPaths = 6;
static XrPerformanceMetricsCounterMETA metricValues[6];
XrResult XRAPI_CALL Enumerate(XrInstance, uint32_t capacity, uint32_t *count, XrPath *out) {
	*count = metricPaths; if (capacity) { if (capacity < *count) return XR_ERROR_SIZE_INSUFFICIENT; std::copy(paths, paths + *count, out); } return XR_SUCCESS;
}
XrResult XRAPI_CALL Set(XrSession, const XrPerformanceMetricsStateMETA *state) {
	++sets; if (failSet) return XR_ERROR_RUNTIME_FAILURE; metricsEnabled = state->enabled; return XR_SUCCESS;
}
XrResult XRAPI_CALL QueryMetric(XrSession, XrPath path, XrPerformanceMetricsCounterMETA *out) {
	assert(metricsEnabled && path && path <= uint64_t(metricPaths)); ++metricQueries;
	if (failQuery) return XR_ERROR_RUNTIME_FAILURE;
	*out = metricValues[path - 1]; return XR_SUCCESS;
}
extern "C" XrResult XRAPI_CALL xrGetInstanceProcAddr(XrInstance, const char *name, PFN_xrVoidFunction *out) {
	if (!std::strcmp(name, "xrEnumeratePerformanceMetricsCounterPathsMETA")) *out = reinterpret_cast<PFN_xrVoidFunction>(Enumerate);
	else if (!std::strcmp(name, "xrSetPerformanceMetricsStateMETA")) *out = reinterpret_cast<PFN_xrVoidFunction>(Set);
	else if (!std::strcmp(name, "xrQueryPerformanceMetricsCounterMETA")) *out = reinterpret_cast<PFN_xrVoidFunction>(QueryMetric);
	else return XR_ERROR_FUNCTION_UNSUPPORTED;
	return XR_SUCCESS;
}
extern "C" XrResult XRAPI_CALL xrStringToPath(XrInstance, const char *name, XrPath *out) {
	for (int i = 0; i < 6; ++i) if (!std::strcmp(name, names[i])) { *out = paths[i]; return XR_SUCCESS; }
	return XR_ERROR_PATH_UNSUPPORTED;
}
void TestMetaMetrics() {
	const XrInstance instance = reinterpret_cast<XrInstance>(1); const XrSession session = reinterpret_cast<XrSession>(2);
	for (int i = 0; i < 6; ++i) {
		auto &v = metricValues[i]; v.type = XR_TYPE_PERFORMANCE_METRICS_COUNTER_META;
		v.counterUnit = i < 4 ? XR_PERFORMANCE_METRICS_COUNTER_UNIT_MILLISECONDS_META : XR_PERFORMANCE_METRICS_COUNTER_UNIT_GENERIC_META;
		v.counterFlags = XR_PERFORMANCE_METRICS_COUNTER_FLOAT_VALUE_VALID_BIT_META; v.floatValue = float(20 + i);
	}
	metricValues[5].counterFlags = XR_PERFORMANCE_METRICS_COUNTER_UINT_VALUE_VALID_BIT_META; metricValues[5].uintValue = 3;
	OpenXRPerfMetrics metrics; char text[512];
	metrics.Init(instance, session, false); metrics.Poll(true, 0); assert(metricQueries == 0);
	metrics.GetText(text, sizeof(text)); assert(std::strstr(text, "unavailable"));
	metrics.Init(instance, session, true); metrics.Poll(false, 0); assert(!metricsEnabled);
	metrics.Poll(true, 0); assert(metricsEnabled && metricQueries == 6); metrics.GetText(text, sizeof(text));
	assert(std::strstr(text, "runtime window") && std::strstr(text, "CPU 20.0 | GPU 21.0") && std::strstr(text, "warp mode 3"));
	metrics.Poll(true, .999); assert(metricQueries == 6); metrics.Poll(true, 1); assert(metricQueries == 12);
	metricValues[0].counterFlags = 0; metricValues[1].floatValue = NAN;
	metricValues[2].counterUnit = XR_PERFORMANCE_METRICS_COUNTER_UNIT_BYTES_META; metricValues[3].floatValue = -1;
	metrics.Poll(true, 2); metrics.GetText(text, sizeof(text)); assert(std::strstr(text, "CPU n/a | GPU n/a"));
	failQuery = true; metrics.Poll(true, 3); metrics.GetText(text, sizeof(text)); assert(std::strstr(text, "drops n/a")); failQuery = false;
	metrics.Poll(false, 4); assert(!metricsEnabled); metrics.Shutdown();
	// Invalidating or hiding measurements cannot leave stale displayed values.
	metrics.GetText(text, sizeof(text)); assert(std::strstr(text, "unavailable"));
	metricPaths = 1; metrics.Init(instance, session, true); int before = metricQueries; metrics.Poll(true, 5);
	assert(metricQueries == before + 1); metrics.Shutdown();
	failSet = true; metrics.Init(instance, session, true); before = metricQueries; metrics.Poll(true, 0); metrics.Poll(true, .5);
	assert(metricQueries == before); metrics.GetText(text, sizeof(text)); assert(std::strstr(text, "unavailable (XR"));
	const int beforeRetry = sets; metrics.Poll(true, .75); assert(sets == beforeRetry);
	metrics.Poll(true, 1); assert(sets == beforeRetry + 1);
	metrics.Shutdown(); failSet = false;
}
int main() {
	TestTimeline(); TestGpuQueries(); TestMetaMetrics();
	std::puts("PASS: complete-cycle exclusive accounting, true main-thread CPU, delayed frame-ID GPU pairing, disjoint/reset/stale/pool handling, p95, disabled overhead and typed/rate-limited Meta counters");
}
