/* The Dark Mod GPL Source Code. See LICENSE.txt for license terms. */
#pragma once

#include <algorithm>
#include <vector>
#include <utility>
#include <cmath>
#include <limits>

// GL-independent, deterministic shelf packing. Each light owns six global and
// six local cube faces in a 3 x 4 rectangle. Both eyes use exactly these pages.
// Reduce resolution under pressure; never drop a light or its shadows.
namespace MobileRenderPlan {
// A captured camera view can be reused by the second eye only when its pose is
// independent of the headset and its image copy immediately follows the draw.
inline bool ReuseCapturedView( bool world, bool subview, bool mirror, bool portalSky,
	bool followsHead, bool playerView, bool adjacentImageCopy, bool flatFrame ) {
	return world && subview && !mirror && !portalSky && !followsHead && !playerView && adjacentImageCopy && !flatFrame;
}
// Half-open framebuffer pixel coordinates, independent of the combined-view
// rectangles produced by the frontend before the late OpenXR eye poses.
struct Rect {
	int x1 = 0, y1 = 0, x2 = 0, y2 = 0;
	bool Empty() const { return x2 <= x1 || y2 <= y1; }
	long long Area() const { return Empty() ? 0 : static_cast<long long>(x2-x1) * (y2-y1); }
	void Add( const Rect &r ) {
		if ( r.Empty() ) return;
		if ( Empty() ) { *this = r; return; }
		x1 = std::min(x1, r.x1); y1 = std::min(y1, r.y1);
		x2 = std::max(x2, r.x2); y2 = std::max(y2, r.y2);
	}
};

// Conservative fixed foveation. Any draw/tile touching either eye's fovea stays
// at full rate. Distance uses the closest point, never the rectangle's centre,
// so a large wall/ground surface cannot coarsen the centre of the image.
// 0=1x1, 1=1x2, 2=2x2, 3=4x2 fragment shading; depth stays per pixel.
inline int FoveationDensity(const Rect &bounds,const Rect &viewport,
	const float centres[2][2],int level) {
	if(level<=0||level>3||bounds.Empty()||viewport.Empty())return 0;
	double distance=std::numeric_limits<double>::infinity();
	for(int eye=0;eye<2;++eye) {
		const double cx=centres[eye][0],cy=centres[eye][1];
		if(!std::isfinite(cx)||!std::isfinite(cy))return 0;
		const double x1=2.0*(bounds.x1-viewport.x1)/(viewport.x2-viewport.x1)-1.0;
		const double x2=2.0*(bounds.x2-viewport.x1)/(viewport.x2-viewport.x1)-1.0;
		const double y1=2.0*(bounds.y1-viewport.y1)/(viewport.y2-viewport.y1)-1.0;
		const double y2=2.0*(bounds.y2-viewport.y1)/(viewport.y2-viewport.y1)-1.0;
		const double dx=std::max(0.0,std::max(x1-cx,cx-x2));
		const double dy=std::max(0.0,std::max(y1-cy,cy-y2));
		distance=std::min(distance,dx*dx+dy*dy);
	}
	const double radius=level==1?0.75:level==2?0.60:0.50;
	if(distance<=radius*radius)return 0;
	if(level==1)return 1;
	return level==3&&distance>1.0?3:2;
}

// Project a world AABB with the active eye's matrices. Clip edges against the
// near plane before dividing by W: simply dropping behind-eye corners clips
// visible lights as the head moves through them. Outward pixel rounding and a
// one-pixel guard preserve coverage with MSAA and floating-point roundoff.
inline Rect ProjectScissor( const float lo[3], const float hi[3],
	const float modelView[16], const float projection[16], const Rect &viewport ) {
	if ( viewport.Empty() ) return {};
	for ( int i = 0; i < 3; ++i )
		if ( !std::isfinite(lo[i]) || !std::isfinite(hi[i]) || lo[i] > hi[i] ) return viewport;
	double clip[8][4];
	for ( int i = 0; i < 8; ++i ) {
		double point[4] = { (i&1) ? hi[0] : lo[0], (i&2) ? hi[1] : lo[1], (i&4) ? hi[2] : lo[2], 1.0 };
		double view[4] = {};
		for ( int row = 0; row < 4; ++row )
			for ( int col = 0; col < 4; ++col ) view[row] += modelView[4*col+row] * point[col];
		for ( int row = 0; row < 4; ++row ) {
			clip[i][row] = 0;
			for ( int col = 0; col < 4; ++col ) clip[i][row] += projection[4*col+row] * view[col];
			if ( !std::isfinite(clip[i][row]) ) return viewport;
		}
	}
	double minX = std::numeric_limits<double>::infinity(), minY = minX;
	double maxX = -minX, maxY = -minY;
	bool uncertain = false;
	auto add = [&]( const double p[4] ) {
		if ( p[3] <= 1e-8 ) { uncertain = true; return; }
		const double x = p[0]/p[3], y = p[1]/p[3];
		minX = std::min(minX, x); maxX = std::max(maxX, x);
		minY = std::min(minY, y); maxY = std::max(maxY, y);
	};
	for ( int i = 0; i < 8; ++i ) {
		const double da = clip[i][2] + clip[i][3];
		if ( std::abs(da) < 1e-6 * (std::abs(clip[i][2])+std::abs(clip[i][3])+1.0) ) return viewport;
		if ( da >= 0 ) add(clip[i]);
		for ( int axis = 0; axis < 3; ++axis ) {
			const int j = i ^ (1<<axis);
			if ( j < i ) continue;
			const double db = clip[j][2] + clip[j][3];
			if ( (da < 0) == (db < 0) ) continue;
			const double t = da / (da-db);
			double p[4];
			for ( int k = 0; k < 4; ++k ) p[k] = clip[i][k] + t * (clip[j][k]-clip[i][k]);
			add(p);
		}
	}
	if ( uncertain ) return viewport;
	if ( minX > maxX ) return {};
	const int width = viewport.x2-viewport.x1, height = viewport.y2-viewport.y1;
	if ( minX > 1.0+2.0/width || maxX < -1.0-2.0/width || minY > 1.0+2.0/height || maxY < -1.0-2.0/height ) return {};
	auto clampNdc = [](double x) { return std::max(-1.0, std::min(1.0, x)); };
	Rect r;
	r.x1 = std::max(viewport.x1, viewport.x1 + int(std::floor((clampNdc(minX)*0.5+0.5)*width))-1);
	r.y1 = std::max(viewport.y1, viewport.y1 + int(std::floor((clampNdc(minY)*0.5+0.5)*height))-1);
	r.x2 = std::min(viewport.x2, viewport.x1 + int(std::ceil((clampNdc(maxX)*0.5+0.5)*width))+1);
	r.y2 = std::min(viewport.y2, viewport.y1 + int(std::ceil((clampNdc(maxY)*0.5+0.5)*height))+1);
	return r;
}

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
