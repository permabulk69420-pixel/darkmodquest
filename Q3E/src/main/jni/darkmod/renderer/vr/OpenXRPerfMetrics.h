// The Dark Mod GPLv3: optional Meta runtime measurements, not engine estimates.
#pragma once

#include <openxr/openxr.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

class OpenXRPerfMetrics {
public:
	void Init(XrInstance instance, XrSession session, bool extensionEnabled) {
		Shutdown();
		this->session = session;
		if (!extensionEnabled) return;
		Load(instance, "xrEnumeratePerformanceMetricsCounterPathsMETA", enumerate);
		Load(instance, "xrSetPerformanceMetricsStateMETA", setState);
		Load(instance, "xrQueryPerformanceMetricsCounterMETA", query);
		if (!enumerate || !setState || !query) { result = XR_ERROR_FUNCTION_UNSUPPORTED; return; }
		uint32_t count = 0;
		result = enumerate(instance, 0, &count, nullptr);
		if (XR_FAILED(result)) return;
		std::vector<XrPath> paths;
		for (int attempt = 0; attempt < 3; ++attempt) {
			if (count > 4096) { result = XR_ERROR_LIMIT_REACHED; return; }
			paths.resize(count);
			result = enumerate(instance, uint32_t(paths.size()), &count, paths.data());
			if (result != XR_ERROR_SIZE_INSUFFICIENT) break;
		}
		if (XR_FAILED(result) || count > paths.size()) return;
		paths.resize(count);
		const char *names[] = {
			"/perfmetrics_meta/app/cpu_frametime", "/perfmetrics_meta/app/gpu_frametime",
			"/perfmetrics_meta/compositor/cpu_frametime", "/perfmetrics_meta/compositor/gpu_frametime",
			"/perfmetrics_meta/compositor/dropped_frame_count", "/perfmetrics_meta/compositor/spacewarp_mode"
		};
		for (int i = 0; i < CounterCount; ++i) {
			XrPath path = XR_NULL_PATH;
			if (XR_SUCCEEDED(xrStringToPath(instance, names[i], &path)) && std::find(paths.begin(), paths.end(), path) != paths.end())
				counters[i].path = path;
		}
		available = true;
	}
	void Shutdown() {
		if (enabled && setState && session != XR_NULL_HANDLE) {
			XrPerformanceMetricsStateMETA state = { XR_TYPE_PERFORMANCE_METRICS_STATE_META };
			setState(session, &state);
		}
		enabled = available = polled = false;
		session = XR_NULL_HANDLE;
		enumerate = nullptr; setState = nullptr; query = nullptr;
		result = XR_SUCCESS;
		for (auto &counter : counters) counter = {};
	}
	// At most six queries per second; never change rendering/pacing based on
	// these values. Meta defines their intervals, independently of our samples.
	void Poll(bool visible, double nowSeconds) {
		if (!available) return;
		if (!visible) {
			if (enabled && (!polled || nowSeconds - lastPoll >= 1.0)) {
				XrPerformanceMetricsStateMETA state = { XR_TYPE_PERFORMANCE_METRICS_STATE_META };
				result = setState(session, &state);
				polled = true; lastPoll = nowSeconds;
				if (XR_SUCCEEDED(result)) enabled = false;
			}
			if (!enabled) polled = false;
			for (auto &counter : counters) counter.valid = false;
			return;
		}
		if (polled && nowSeconds - lastPoll < 1.0) return;
		polled = true; lastPoll = nowSeconds;
		for (auto &counter : counters) counter.valid = false;
		if (!enabled) {
			XrPerformanceMetricsStateMETA state = { XR_TYPE_PERFORMANCE_METRICS_STATE_META };
			state.enabled = XR_TRUE;
			result = setState(session, &state);
			if (XR_FAILED(result)) return;
			enabled = true;
		}
		result = XR_SUCCESS;
		for (int i = 0; i < CounterCount; ++i) {
			auto &counter = counters[i];
			if (counter.path == XR_NULL_PATH) continue;
			XrPerformanceMetricsCounterMETA value = { XR_TYPE_PERFORMANCE_METRICS_COUNTER_META };
			const XrResult queried = query(session, counter.path, &value);
			if (XR_FAILED(queried)) { result = queried; continue; }
			const auto unit = i < 4 ? XR_PERFORMANCE_METRICS_COUNTER_UNIT_MILLISECONDS_META : XR_PERFORMANCE_METRICS_COUNTER_UNIT_GENERIC_META;
			if (value.counterUnit != unit) continue;
			if (value.counterFlags & XR_PERFORMANCE_METRICS_COUNTER_FLOAT_VALUE_VALID_BIT_META) counter.value = value.floatValue;
			else if (value.counterFlags & XR_PERFORMANCE_METRICS_COUNTER_UINT_VALUE_VALID_BIT_META) counter.value = value.uintValue;
			else continue;
			counter.valid = std::isfinite(counter.value) && counter.value >= 0;
		}
	}
	void GetText(char *buffer, int size) const {
		if (!available || !enabled) {
			if (result == XR_SUCCESS) std::snprintf(buffer, size, "Meta runtime counters: %s\n", available ? "OFF" : "unavailable");
			else std::snprintf(buffer, size, "Meta runtime counters: unavailable (XR %d)\n", int(result));
			return;
		}
		char value[CounterCount][24];
		for (int i = 0; i < CounterCount; ++i) {
			if (counters[i].valid) std::snprintf(value[i], sizeof(value[i]), i < 4 ? "%.1f" : "%.0f", counters[i].value);
			else std::snprintf(value[i], sizeof(value[i]), "n/a");
		}
		std::snprintf(buffer, size,
			"Meta app ms (runtime window): CPU %s | GPU %s\n"
			"Meta compositor ms: CPU %s | GPU %s | drops %s | warp mode %s\n",
			value[0], value[1], value[2], value[3], value[4], value[5]);
	}
private:
	template<class Function> static void Load(XrInstance instance, const char *name, Function &target) {
		PFN_xrVoidFunction function = nullptr;
		if (XR_SUCCEEDED(xrGetInstanceProcAddr(instance, name, &function))) target = reinterpret_cast<Function>(function);
	}
	static constexpr int CounterCount = 6;
	struct Counter { XrPath path = XR_NULL_PATH; bool valid = false; double value = 0; } counters[CounterCount];
	XrSession session = XR_NULL_HANDLE;
	PFN_xrEnumeratePerformanceMetricsCounterPathsMETA enumerate = nullptr;
	PFN_xrSetPerformanceMetricsStateMETA setState = nullptr;
	PFN_xrQueryPerformanceMetricsCounterMETA query = nullptr;
	XrResult result = XR_SUCCESS;
	bool available = false, enabled = false, polled = false;
	double lastPoll = 0;
};
