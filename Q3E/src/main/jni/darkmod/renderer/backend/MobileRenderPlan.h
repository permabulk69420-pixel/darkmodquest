/* The Dark Mod GPL Source Code. See LICENSE.txt for license terms. */
#pragma once

#include <algorithm>
#include <vector>
#include <utility>

// GL-independent, deterministic shelf packing. Each light owns six global and
// six local cube faces in a 3 x 4 rectangle. Both eyes use exactly these pages.
// Reduce resolution under pressure; never drop a light or its shadows.
namespace MobileRenderPlan {
// Conservative mask for a light-relative world AABB. A face is needed iff the
// box can reach that signed major axis without lying outside its side planes.
inline unsigned CubeFaceMask( const float lo[3], const float hi[3] ) {
	float nearest[3];
	for ( int i = 0; i < 3; ++i ) nearest[i] = lo[i] > 0 ? lo[i] : hi[i] < 0 ? -hi[i] : 0;
	unsigned mask = 0;
	for ( int axis = 0; axis < 3; ++axis ) {
		float required = std::max(1.0f, std::max(nearest[(axis+1)%3], nearest[(axis+2)%3]));
		if ( hi[axis] >= required ) mask |= 1u << (2*axis);
		if ( -lo[axis] >= required ) mask |= 1u << (2*axis+1);
	}
	return mask;
}

struct Page {
	int x = 0, y = 0, size = 0;
};

inline bool Pack( int atlasSize, const std::vector<int> &requested, std::vector<Page> &pages,
	const std::vector<std::pair<int, int>> &shapes = {} ) {
	pages.assign( requested.size(), {} );
	if ( requested.empty() ) return true;
	if ( atlasSize < 4 ) return false;
	if ( !shapes.empty() && shapes.size() != requested.size() ) return false;
	std::vector<int> sizes = requested;
	for ( size_t i = 0; i < sizes.size(); ++i ) {
		int &size = sizes[i];
		const int columns = shapes.empty() ? 3 : shapes[i].first;
		const int rows = shapes.empty() ? 4 : shapes[i].second;
		if ( columns < 1 || rows < 1 ) return false;
		const int maxFace = atlasSize / std::max(columns, rows);
		if ( maxFace < 1 ) return false;
		int power = 1;
		while ( power <= size / 2 && power <= maxFace / 2 ) power *= 2;
		size = power;
	}
	for (;;) {
		std::vector<int> order( sizes.size() );
		for ( size_t i = 0; i < order.size(); ++i ) order[i] = int(i);
		std::stable_sort( order.begin(), order.end(), [&]( int a, int b ) { return sizes[a] > sizes[b]; } );
		int x = 0, y = 0, rowHeight = 0;
		bool fits = true;
		for ( int i : order ) {
			const int w = (shapes.empty() ? 3 : shapes[i].first) * sizes[i];
			const int h = (shapes.empty() ? 4 : shapes[i].second) * sizes[i];
			if ( x + w > atlasSize ) { y += rowHeight; x = rowHeight = 0; }
			if ( y + h > atlasSize ) { fits = false; break; }
			pages[i] = { x, y, sizes[i] };
			x += w;
			rowHeight = std::max( rowHeight, h );
		}
		if ( fits ) return true;
		const int largest = *std::max_element( sizes.begin(), sizes.end() );
		if ( largest == 1 ) { pages.clear(); return false; }
		for ( int &size : sizes ) if ( size == largest ) size /= 2;
	}
}
}
