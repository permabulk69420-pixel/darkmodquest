// The Dark Mod GPLv3. Include after the engine GL declarations and XR_Log.
#pragma once

#include "renderer/QuestFrameTiming.h"
#include <cstdio>
#include <cstring>
#include <mutex>

#ifndef GL_GPU_DISJOINT_EXT
#define GL_GPU_DISJOINT_EXT 0x8FBB
#endif

class OpenXRFrameTimer {
public:
	explicit OpenXRFrameTimer(QuestFrameTiming &timing = QuestTiming()) : timing(timing) {}
	void Init() {
		glGenQueriesEXT = (void (*)(GLsizei, GLuint *))eglGetProcAddress("glGenQueriesEXT");
		glDeleteQueriesEXT = (void (*)(GLsizei, const GLuint *))eglGetProcAddress("glDeleteQueriesEXT");
		glBeginQueryEXT = (void (*)(GLenum, GLuint))eglGetProcAddress("glBeginQueryEXT");
		glEndQueryEXT = (void (*)(GLenum))eglGetProcAddress("glEndQueryEXT");
		glGetQueryObjectuivEXT = (void (*)(GLuint, GLenum, GLuint *))eglGetProcAddress("glGetQueryObjectuivEXT");
		glGetQueryObjectui64vEXT = (void (*)(GLuint, GLenum, uint64_t *))eglGetProcAddress("glGetQueryObjectui64vEXT");
		const char *extensions = (const char *)qglGetString(GL_EXTENSIONS);
		gpuOk = extensions && std::strstr(extensions, "GL_EXT_disjoint_timer_query") &&
			glGenQueriesEXT && glDeleteQueriesEXT && glBeginQueryEXT && glEndQueryEXT && glGetQueryObjectuivEXT && glGetQueryObjectui64vEXT;
		Reset();
		XR_Log("frame timing: complete-cycle main-thread accounting; GL elapsed queries %s", gpuOk ? "available" : "unavailable");
	}
	void Shutdown() {
		Reset(false);
		gpuOk = false;
	}
	void Reset(bool recreateQueries = true) {
		if (current >= 0) glEndQueryEXT(GL_TIME_ELAPSED_EXT);
		current = -1;
		// Deleting pending queries is asynchronous; never wait for a result.
		if (gpuOk) {
			glDeleteQueriesEXT(NumQueries, queries);
			for (auto &query : queries) query = 0;
			if (recreateQueries) glGenQueriesEXT(NumQueries, queries);
		}
		for (auto &slot : slots) slot = {};
		nextQuery = 0;
		timing.Enable(false);
		lastReport = 0; lastReportId = 0; disjoints = 0;
		std::lock_guard<std::mutex> lock(textMutex);
		text[0] = details[0] = 0;
	}
	void Enable(bool enabled) {
		if (enabled == timing.Enabled()) return;
		Reset();
		timing.Enable(enabled);
		lastReport = timing.Now();
	}
	void BeginGpu() {
		if (!gpuOk || !timing.Enabled()) return;
		PollGpu();
		for (int offset = 0; offset < NumQueries; ++offset) {
			const int slot = (nextQuery + offset) % NumQueries;
			if (slots[slot].pending) continue;
			slots[slot].id = timing.CurrentId();
			slots[slot].valid = true;
			glBeginQueryEXT(GL_TIME_ELAPSED_EXT, queries[slot]);
			current = slot; nextQuery = (slot + 1) % NumQueries;
			return;
		}
	}
	void EndGpu() {
		if (current < 0) return;
		glEndQueryEXT(GL_TIME_ELAPSED_EXT);
		slots[current].pending = true;
		current = -1;
	}
	bool GetText(char *buffer, int size) const { return CopyText(buffer, size, false); }
	bool GetDetails(char *buffer, int size) const { return CopyText(buffer, size, true); }

	// Call only after the render scope, image releases and xrEndFrame complete.
	void Complete(bool rendered) {
		if (!timing.Enabled()) return;
		if (gpuOk) PollGpu();
		timing.Complete(rendered);
		const auto now = timing.Now();
		if (now - lastReport < 2000000000) return;
		const auto r = timing.Since(lastReportId);
		if (!r.count || r.wall <= 0) return;
		char gpu[24] = "n/a", cpu[24] = "n/a", pairedCpu[24] = "n/a", pairedWall[24] = "n/a";
		if (r.gpuCount) std::snprintf(gpu, sizeof(gpu), "%.1f", r.gpu);
		if (r.gpuCount) std::snprintf(pairedWall, sizeof(pairedWall), "%.1f", r.pairedWall);
		if (r.cpuCount) std::snprintf(cpu, sizeof(cpu), "%.1f", r.cpu);
		if (r.pairedCpuCount) std::snprintf(pairedCpu, sizeof(pairedCpu), "%.1f", r.pairedCpu);
		const auto *p = r.phases;
		{
			std::lock_guard<std::mutex> lock(textMutex);
			std::snprintf(text, sizeof(text), "%.0f app fps | frame %.1f ms | main CPU %s ms | GL span %s ms",
				1000.0 * r.rendered / (r.count * r.wall), r.wall, cpu, gpu);
			std::snprintf(details, sizeof(details),
				"Timing cycles %llu-%llu (%d) | frame p95 %.1f ms\n"
				"Wall ms: engine %.1f | render %.1f | XR begin %.1f\n"
				"XR ms: wait %.1f | acquire %.1f | release %.1f | end %.1f\n"
				"Sync ms: frontend %.1f | upload %.1f | fence %.1f | LG read %.1f\n"
				"GL matched %d/%d: span %s | main CPU %s | wall %s\n"
				"Main CPU excludes workers; GL overlaps CPU/includes idle. Disjoint %u\n",
				(unsigned long long)r.first, (unsigned long long)r.last, r.count, r.p95,
				p[QuestFrameTiming::Engine], p[QuestFrameTiming::Render], p[QuestFrameTiming::XrBegin],
				p[QuestFrameTiming::XrWait], p[QuestFrameTiming::Acquire], p[QuestFrameTiming::Release], p[QuestFrameTiming::XrEnd],
				p[QuestFrameTiming::FrontendWait], p[QuestFrameTiming::Upload], p[QuestFrameTiming::FenceWait], p[QuestFrameTiming::LightgemReadback],
				r.gpuCount, r.count, gpu, pairedCpu, pairedWall, disjoints);
		}
		XR_Log("timing: cycles %llu-%llu | %.1f ms main cycle (p95 %.1f max %.1f) | main CPU %s ms | GL span %s ms (%d/%d paired) | engine %.1f render %.1f XR begin %.1f wait %.1f acquire %.1f release %.1f end %.1f frontend wait %.1f upload %.1f fence %.1f lightgem read %.1f",
			(unsigned long long)r.first, (unsigned long long)r.last, r.wall, r.p95, r.max, cpu, gpu, r.gpuCount, r.count,
			p[QuestFrameTiming::Engine], p[QuestFrameTiming::Render], p[QuestFrameTiming::XrBegin], p[QuestFrameTiming::XrWait], p[QuestFrameTiming::Acquire], p[QuestFrameTiming::Release], p[QuestFrameTiming::XrEnd], p[QuestFrameTiming::FrontendWait], p[QuestFrameTiming::Upload], p[QuestFrameTiming::FenceWait], p[QuestFrameTiming::LightgemReadback]);
		lastReport = now; lastReportId = r.last;
	}
private:
	bool CopyText(char *buffer, int size, bool detailed) const {
		if (size <= 0) return false;
		std::lock_guard<std::mutex> lock(textMutex);
		const char *source = detailed ? details : text;
		if (!source[0]) return false;
		std::snprintf(buffer, size, "%s", source);
		return true;
	}
	void PollGpu() {
		GLint disjoint = 0;
		qglGetIntegerv(GL_GPU_DISJOINT_EXT, &disjoint);
		if (disjoint) {
			++disjoints;
			timing.InvalidateGpu();
			// The flag covers the whole clock-discontinuity interval, not just
			// the first query retrieved. Drain ALL pending queries as invalid.
			for (auto &slot : slots) slot.valid = false;
		}
		for (int i = 0; i < NumQueries; ++i) {
			if (!slots[i].pending) continue;
			GLuint available = 0;
			glGetQueryObjectuivEXT(queries[i], GL_QUERY_RESULT_AVAILABLE, &available);
			if (!available) continue;
			uint64_t ns = 0;
			glGetQueryObjectui64vEXT(queries[i], GL_QUERY_RESULT, &ns);
			if (slots[i].valid) timing.AttachGpu(slots[i].id, double(ns) / 1e6);
			slots[i].pending = false;
		}
	}
	static constexpr int NumQueries = 8;
	void (*glGenQueriesEXT)(GLsizei, GLuint *) = nullptr;
	void (*glDeleteQueriesEXT)(GLsizei, const GLuint *) = nullptr;
	void (*glBeginQueryEXT)(GLenum, GLuint) = nullptr;
	void (*glEndQueryEXT)(GLenum) = nullptr;
	void (*glGetQueryObjectuivEXT)(GLuint, GLenum, GLuint *) = nullptr;
	void (*glGetQueryObjectui64vEXT)(GLuint, GLenum, uint64_t *) = nullptr;
	QuestFrameTiming &timing;
	bool gpuOk = false;
	GLuint queries[NumQueries] = {};
	struct Slot { uint64_t id = 0; bool pending = false, valid = false; } slots[NumQueries];
	int current = -1, nextQuery = 0;
	unsigned disjoints = 0;
	QuestFrameTiming::Tick lastReport = 0;
	uint64_t lastReportId = 0;
	mutable std::mutex textMutex;
	char text[160] = {}, details[1100] = {};
};
