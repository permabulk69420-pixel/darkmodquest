/* The Dark Mod GPL Source Code. See LICENSE.txt for license terms. */
#pragma once
#include "renderer/backend/MobileRenderPlan.h"
#include <array>
#include <cstdint>
#include <map>

namespace MobileShadowCache {
// Hash explicit state only, never padding or frontend object contents. Two
// independent accumulators plus a count make caster-set signatures insensitive
// to traversal order while retaining duplicate/membership changes.
struct Signature {
	uint64_t a = 1469598103934665603ull, b = 0x9e3779b97f4a7c15ull;
	void Bytes( const void *data, size_t size ) {
		const auto *bytes = static_cast<const unsigned char *>(data);
		for ( size_t i = 0; i < size; ++i ) {
			a = (a ^ bytes[i]) * 1099511628211ull;
			b ^= uint64_t(bytes[i]) + 0x9e3779b97f4a7c15ull + (b << 6) + (b >> 2);
		}
	}
	template<class T> void Add( const T &value ) { Bytes(&value, sizeof(value)); }
	bool operator==( const Signature &s ) const { return a == s.a && b == s.b; }
	bool operator!=( const Signature &s ) const { return !(*this == s); }
};
struct CasterSet {
	uint64_t a = 0, b = 0, count = 0;
	void Add( const Signature &s ) { a += s.a; b += s.b; ++count; }
	Signature Finish( Signature base ) const { base.Add(a); base.Add(b); base.Add(count); return base; }
};
// Per-light clipped index arrays are immutable for an interaction's lifetime,
// even though the frontend reuploads them into a different temporary GPU range
// each frame. Use their allocation revision, not that temporary buffer address.
inline bool IndexSignature( Signature &content, uint64_t revision, bool isStatic,
	uint32_t offset, uint32_t size, uint32_t frame ) {
	if ( revision ) { content.Add(revision);return true; }
	content.Add(isStatic);content.Add(offset);content.Add(size);content.Add(frame);
	return isStatic;
}
struct FaceState {
	Signature staticContent, finalContent;
	bool haveStatic = false, haveFinal = false;
	bool StaticDirty( const Signature &s ) const { return !haveStatic || s != staticContent; }
	bool FinalDirty( const Signature &s ) const { return !haveFinal || s != finalContent; }
};

struct Request { uint64_t key; int size, columns, rows; };
struct Slot { MobileRenderPlan::Page page; int columns, rows; };

// Keep visible lights in their existing rectangles. A new light fills a hole;
// pressure falls back to the existing quality-preserving packer. Removed slots
// are retired, so returning lights never trust pixels overwritten by a new one.
class AtlasLayout {
public:
	std::map<uint64_t, Slot> slots;
	int atlasSize = 0;
	bool Prepare( int size, const std::vector<Request> &requests, std::vector<MobileRenderPlan::Page> &pages, bool &repacked ) {
		repacked = size != atlasSize;
		if ( repacked ) slots.clear();
		atlasSize = size;
		std::map<uint64_t, Slot> next;
		std::vector<MobileRenderPlan::Rect> occupied;
		pages.assign(requests.size(), {});
		for ( size_t i = 0; i < requests.size(); ++i ) {
			const Request &r = requests[i];
			auto old = slots.find(r.key);
			if ( old == slots.end() || old->second.columns != r.columns || old->second.rows != r.rows || old->second.page.size < r.size ) continue;
			const Slot &slot = old->second;
			pages[i] = slot.page; next.emplace(r.key, slot);
			occupied.push_back({slot.page.x, slot.page.y, slot.page.x+r.columns*slot.page.size, slot.page.y+r.rows*slot.page.size});
		}
		std::vector<size_t> missing;
		for ( size_t i = 0; i < requests.size(); ++i ) if ( !pages[i].size ) missing.push_back(i);
		std::stable_sort(missing.begin(), missing.end(), [&](size_t a,size_t b){return requests[a].size > requests[b].size;});
		for ( size_t i : missing ) {
			const Request &r = requests[i];
			std::vector<int> xs{0}, ys{0};
			for ( const auto &rect : occupied ) { xs.push_back(rect.x2); ys.push_back(rect.y2); }
			std::sort(xs.begin(),xs.end());std::sort(ys.begin(),ys.end());
			bool placed = false;
			for ( int y : ys ) {
				for ( int x : xs ) {
					MobileRenderPlan::Rect rect{x,y,x+r.columns*r.size,y+r.rows*r.size};
					if ( rect.x2 > size || rect.y2 > size ) continue;
					bool overlaps = false;
					for ( const auto &used : occupied )
						if ( rect.x1 < used.x2 && rect.x2 > used.x1 && rect.y1 < used.y2 && rect.y2 > used.y1 ) { overlaps = true; break; }
					if ( overlaps ) continue;
					pages[i] = {x,y,r.size}; occupied.push_back(rect);
					next.emplace(r.key,Slot{pages[i],r.columns,r.rows}); placed = true; break;
				}
				if ( placed ) break;
			}
			if ( !placed ) {
				std::vector<int> sizes;std::vector<std::pair<int,int>> shapes;
				for ( const auto &request : requests ) { sizes.push_back(request.size);shapes.emplace_back(request.columns,request.rows); }
				if ( !MobileRenderPlan::Pack(size,sizes,pages,shapes) ) return false;
				next.clear();repacked = true;
				for ( size_t j = 0; j < requests.size(); ++j ) next.emplace(requests[j].key,Slot{pages[j],requests[j].columns,requests[j].rows});
				break;
			}
		}
		slots.swap(next);
		return true;
	}
	void Clear() { slots.clear();atlasSize = 0; }
};

// A model which starts moving is kept out of the static layer until it settles.
// This is only a cache placement choice: every changed signature is still
// redrawn immediately, including door motion and animated alpha/texture state.
class MotionTracker {
	struct Entry { Signature content; uint64_t changed, used; bool settled; };
	std::map<uint64_t, Entry> entries;
public:
	bool Static( uint64_t key, const Signature &content, uint64_t frame ) {
		auto found = entries.find(key);
		if ( found == entries.end() ) { entries.emplace(key,Entry{content,frame,frame,true}); return true; }
		Entry &entry = found->second;entry.used = frame;
		if ( entry.content != content ) { entry.content = content;entry.changed = frame;entry.settled = false; }
		if ( frame >= entry.changed + 30 ) entry.settled = true;
		return entry.settled;
	}
	void Prune( uint64_t frame ) {
		for ( auto it = entries.begin(); it != entries.end(); )
			if ( frame > it->second.used + 120 ) it = entries.erase(it); else ++it;
	}
	void Clear() { entries.clear(); }
};
}
