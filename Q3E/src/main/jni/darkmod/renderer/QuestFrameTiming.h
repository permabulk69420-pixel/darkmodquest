// The Dark Mod GPLv3: complete OpenXR frame accounting, on the GL/main thread.
#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <ctime>

class QuestFrameTiming {
public:
	enum Phase { Engine, Render, XrWait, XrBegin, Acquire, Release, XrEnd,
		FrontendWait, Upload, FenceWait, LightgemReadback, PhaseCount };
	using Tick = int64_t;
	using Clock = Tick (*)();
	static Tick WallClock() {
		return std::chrono::duration_cast<std::chrono::nanoseconds>(
			std::chrono::steady_clock::now().time_since_epoch()).count();
	}
	static Tick ThreadClock() {
#if defined(CLOCK_THREAD_CPUTIME_ID)
		timespec value;
		if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &value) == 0)
			return Tick(value.tv_sec) * 1000000000 + value.tv_nsec;
#endif
		return -1;
	}
	struct Sample {
		uint64_t id = 0;
		double wall = 0, cpu = -1, gpu = -1;
		double phases[PhaseCount] = {};
		bool rendered = false;
	};
	struct Report {
		uint64_t first = 0, last = 0;
		int count = 0, rendered = 0, cpuCount = 0, gpuCount = 0, pairedCpuCount = 0;
		double wall = 0, cpu = 0, gpu = 0, p95 = 0, max = 0;
		double pairedWall = 0, pairedCpu = 0;
		double phases[PhaseCount] = {};
	};
	explicit QuestFrameTiming(Clock wall = WallClock, Clock cpu = ThreadClock) : wallClock(wall), cpuClock(cpu) {}

	void Enable(bool value) {
		if (value == enabled) return;
		enabled = value;
		++generation;
		for (auto &sample : samples) sample = {};
		current = {};
		current.id = 1;
		phase = Engine;
		warm = false;
		boundary = mark = value ? wallClock() : 0;
		cpuBoundary = value ? cpuClock() : -1;
	}
	bool Enabled() const { return enabled; }
	uint64_t CurrentId() const { return current.id; }
	uint64_t LastId() const { return current.id - 1; }
	Tick Now() const { return wallClock(); }

	// Nested scopes are exclusive: a buffer fence inside rendering is counted
	// as a fence wait, never counted a second time as render submission.
	class Scope {
	public:
		Scope(QuestFrameTiming &owner, Phase next) : owner(owner), active(owner.enabled), generation(owner.generation) {
			if (active) { previous = owner.phase; owner.ChangePhase(next); }
		}
		~Scope() { if (active && owner.enabled && generation == owner.generation) owner.ChangePhase(previous); }
		Scope(const Scope &) = delete;
		Scope &operator=(const Scope &) = delete;
	private:
		QuestFrameTiming &owner;
		bool active;
		unsigned generation;
		Phase previous = Engine;
	};

	// Boundary AFTER swapchain release and xrEndFrame. The next cycle also
	// includes engine work, frontend joins, uploads and fences outside OpenXR.
	void Complete(bool rendered) {
		if (!enabled) return;
		const Tick now = wallClock(), cpuNow = cpuClock();
		Accumulate(now);
		current.wall = double(now - boundary) / 1e6;
		current.cpu = cpuNow >= cpuBoundary && cpuBoundary >= 0 ? double(cpuNow - cpuBoundary) / 1e6 : -1;
		current.rendered = rendered;
		if (warm) samples[current.id % Capacity] = current;
		warm = true; // first cycle lacks the engine work before profiling began
		const uint64_t next = current.id + 1;
		current = {};
		current.id = next;
		boundary = mark = now;
		cpuBoundary = cpuNow;
	}
	void AttachGpu(uint64_t id, double milliseconds) {
		if (!enabled || !std::isfinite(milliseconds) || milliseconds < 0) return;
		if (id == current.id) current.gpu = milliseconds;
		else if (id && samples[id % Capacity].id == id) samples[id % Capacity].gpu = milliseconds;
	}
	void InvalidateGpu() {
		current.gpu = -1;
		for (auto &sample : samples) sample.gpu = -1;
	}
	Report Since(uint64_t after) const {
		Report report;
		double intervals[Capacity];
		const uint64_t begin = std::max(after + 1, current.id > Capacity ? current.id - Capacity : uint64_t(1));
		for (uint64_t id = begin; id < current.id; ++id) {
			const Sample &s = samples[id % Capacity];
			if (s.id != id) continue;
			if (!report.count) report.first = id;
			report.last = id;
			intervals[report.count++] = s.wall;
			report.rendered += s.rendered;
			report.wall += s.wall;
			report.max = std::max(report.max, s.wall);
			if (s.cpu >= 0) { report.cpu += s.cpu; ++report.cpuCount; }
			for (int p = 0; p < PhaseCount; ++p) report.phases[p] += s.phases[p];
			if (s.gpu >= 0) {
				report.gpu += s.gpu; report.pairedWall += s.wall; ++report.gpuCount;
				if (s.cpu >= 0) { report.pairedCpu += s.cpu; ++report.pairedCpuCount; }
			}
		}
		if (report.count) {
			std::sort(intervals, intervals + report.count);
			report.p95 = intervals[(report.count * 95 + 99) / 100 - 1];
			for (double &value : report.phases) value /= report.count;
			report.wall /= report.count;
		}
		if (report.cpuCount) report.cpu /= report.cpuCount;
		if (report.gpuCount) { report.gpu /= report.gpuCount; report.pairedWall /= report.gpuCount; }
		if (report.pairedCpuCount) report.pairedCpu /= report.pairedCpuCount;
		return report;
	}

private:
	static constexpr int Capacity = 512;
	void Accumulate(Tick now) { current.phases[phase] += double(now - mark) / 1e6; mark = now; }
	void ChangePhase(Phase next) { Accumulate(wallClock()); phase = next; }
	Clock wallClock, cpuClock;
	bool enabled = false, warm = false;
	unsigned generation = 0;
	Phase phase = Engine;
	Tick boundary = 0, mark = 0, cpuBoundary = -1;
	Sample current;
	Sample samples[Capacity];
};

// Inline function-local static is shared across engine translation units. Hooks
// below are GL/main-thread only; frontend worker times remain separate snapshots.
inline QuestFrameTiming &QuestTiming() { static QuestFrameTiming timing; return timing; }
