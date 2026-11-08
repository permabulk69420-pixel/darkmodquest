// The Dark Mod GPLv3. Cached opaque shadow index merging; no extra depth target.
#pragma once

#include "renderer/backend/MobileShadowCache.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <map>
#include <tuple>
#include <vector>

namespace MobileShadowBatch {
struct GeometryState {
	unsigned vertexBuffer = 0;
	std::array<float,16> matrix;
	bool operator<(const GeometryState &other) const {
		if (vertexBuffer != other.vertexBuffer) return vertexBuffer < other.vertexBuffer;
		return std::memcmp(matrix.data(),other.matrix.data(),sizeof(matrix)) < 0;
	}
};
struct Source {
	const void *surface = nullptr; // current-frame identity only, never retained
	const uint32_t *indexes = nullptr; // submitted CPU staging region
	unsigned count = 0, vertices = 0, baseVertex = 0, vertexBuffer = 0, slot = 0;
	const float *modelMatrix = nullptr;
	MobileShadowCache::Signature content;
};
struct Draw {
	GLuint buffer = 0;
	unsigned count = 0, vertices = 0;
	std::vector<const void *> surfaces;
	std::vector<unsigned> slots;
};

class Cache {
public:
	void BeginFrame(bool on, uint64_t revision, uint64_t frame) {
		if (!on || revision != storageRevision) Clear();
		enabled = on; storageRevision = revision; currentFrame = frame;
		uploadedBytes = sourceDraws = batchDraws = hits = 0;
		for (auto it = entries.begin(); it != entries.end();) {
			if (frame > it->second.lastSeen + 120) { Destroy(it->second); it = entries.erase(it); }
			else ++it;
		}
	}
	void Clear() {
		for (auto &item : entries) Destroy(item.second);
		entries.clear(); residentBytes = 0;
	}
	std::vector<Draw> Prepare(uint64_t owner, int face, const std::vector<Source> &sources) {
		std::vector<Draw> result;
		if (!enabled) return result;
		std::map<GeometryState, std::vector<const Source *>> groups;
		for (const auto &source : sources) {
			if (!source.surface || !source.indexes || !source.count || source.count % 3 || !source.modelMatrix) continue;
			GeometryState state; state.vertexBuffer = source.vertexBuffer;
			std::memcpy(state.matrix.data(),source.modelMatrix,sizeof(state.matrix));
			groups[state].push_back(&source);
		}
		for (const auto &group : groups) {
			if (group.second.size() < 4) continue;
			MobileShadowCache::CasterSet set;
			uint64_t count = 0, vertices = 0;
			for (const auto *source : group.second) { set.Add(source->content); count += source->count; vertices += source->vertices; }
			if (count > MaxUpload / sizeof(uint32_t) || vertices > 0x7fffffffu) continue;
			MobileShadowCache::Signature base; base.Add(storageRevision);
			const auto content = set.Finish(base);
			const Key key = std::make_tuple(owner, face, group.first);
			auto &entry = entries[key];
			if (entry.content != content) {
				Destroy(entry); entry.content = content; entry.observations = 0;
			}
			else if (!entry.buffer && entry.lastSeen + 1 < currentFrame) entry.observations = 0;
			if (entry.lastSeen != currentFrame) ++entry.observations;
			entry.lastSeen = currentFrame;
			const unsigned bytes = unsigned(count * sizeof(uint32_t));
			// Two matching observations avoid rebuilding for continually moving
			// rigid objects, flickering membership or changing clipped geometry.
			if (!entry.buffer && entry.observations >= 2 && uploadedBytes + bytes <= MaxUpload) {
				MakeRoom(bytes);
				if (residentBytes + bytes <= MaxResident) {
					std::vector<uint32_t> merged;
					merged.reserve(size_t(count));
					bool valid = true;
					for (const auto *source : group.second) {
						for (unsigned i = 0; i < source->count; ++i) {
							const uint32_t index = source->indexes[i];
							if (index >= source->vertices || uint64_t(index) + source->baseVertex > 0x7fffffffu) { valid = false; break; }
							merged.push_back(index + source->baseVertex);
						}
						if (!valid) break;
					}
					if (valid) {
						qglGenBuffers(1, &entry.buffer);
						qglBindBuffer(GL_COPY_WRITE_BUFFER, entry.buffer);
						qglBufferData(GL_COPY_WRITE_BUFFER, bytes, merged.data(), GL_STATIC_DRAW);
						qglBindBuffer(GL_COPY_WRITE_BUFFER, 0);
						entry.bytes = bytes; residentBytes += bytes; uploadedBytes += bytes;
					}
				}
			} else if (entry.buffer) ++hits;
			if (!entry.buffer) continue;
			Draw draw; draw.buffer = entry.buffer; draw.count = unsigned(count); draw.vertices = unsigned(vertices);
			for (const auto *source : group.second) { draw.surfaces.push_back(source->surface); draw.slots.push_back(source->slot); }
			result.push_back(std::move(draw));
		}
		return result;
	}
	void RecordDraw(const Draw &draw) { sourceDraws += unsigned(draw.surfaces.size()); ++batchDraws; }
	unsigned uploadedBytes = 0, sourceDraws = 0, batchDraws = 0, hits = 0, residentBytes = 0;
private:
	static constexpr unsigned MaxResident = 32 * 1024 * 1024, MaxUpload = 2 * 1024 * 1024;
	using Key = std::tuple<uint64_t, int, GeometryState>;
	struct Entry {
		MobileShadowCache::Signature content;
		GLuint buffer = 0;
		unsigned bytes = 0, observations = 0;
		uint64_t lastSeen = 0;
	};
	void Destroy(Entry &entry) {
		if (entry.buffer) { qglDeleteBuffers(1, &entry.buffer); residentBytes -= entry.bytes; }
		entry.buffer = 0; entry.bytes = 0;
	}
	void MakeRoom(unsigned bytes) {
		for (auto it = entries.begin(); residentBytes + bytes > MaxResident && it != entries.end();) {
			if (it->second.lastSeen + 1 < currentFrame) { Destroy(it->second); it = entries.erase(it); }
			else ++it;
		}
	}
	std::map<Key, Entry> entries;
	uint64_t storageRevision = 0, currentFrame = 0;
	bool enabled = false;
};
}
