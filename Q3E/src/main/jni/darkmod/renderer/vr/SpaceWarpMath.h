/* The Dark Mod GPL Source Code. See LICENSE.txt for license terms. */
#pragma once
#include <cmath>
#include <openxr/openxr.h>

namespace SpaceWarpMath {
inline XrPosef Identity() {
	XrPosef p = {}; p.orientation.w = 1.0f; return p;
}
inline XrPosef FromGameSpace(const float position[3],const float quaternion[4],float metresPerUnit) {
	XrPosef pose = {};
	pose.orientation={quaternion[1],-quaternion[2],quaternion[0],quaternion[3]};
	pose.position={-position[1]*metresPerUnit,position[2]*metresPerUnit,-position[0]*metresPerUnit};
	return pose;
}
inline XrQuaternionf Multiply(const XrQuaternionf &a, const XrQuaternionf &b) {
	return {a.w*b.x+a.x*b.w+a.y*b.z-a.z*b.y,
		a.w*b.y-a.x*b.z+a.y*b.w+a.z*b.x,
		a.w*b.z+a.x*b.y-a.y*b.x+a.z*b.w,
		a.w*b.w-a.x*b.x-a.y*b.y-a.z*b.z};
}
inline XrVector3f Rotate(const XrQuaternionf &q, const XrVector3f &v) {
	const XrQuaternionf p = {v.x,v.y,v.z,0}, inv = {-q.x,-q.y,-q.z,q.w};
	const auto r = Multiply(Multiply(q,p),inv); return {r.x,r.y,r.z};
}
inline XrPosef Delta(const XrPosef &previous, const XrPosef &current) {
	XrPosef delta = {};
	const auto &q = previous.orientation;
	const XrQuaternionf inv = {-q.x,-q.y,-q.z,q.w};
	delta.orientation = Multiply(inv,current.orientation);
	const XrVector3f distance = {current.position.x-previous.position.x,
		current.position.y-previous.position.y,current.position.z-previous.position.z};
	delta.position = Rotate(inv,distance);
	return delta;
}
inline bool CameraCut(const XrPosef &delta, double seconds) {
	const auto &p = delta.position;
	return seconds <= 0 || seconds > 0.25 || p.x*p.x+p.y*p.y+p.z*p.z > 9.0f ||
		std::fabs(delta.orientation.w) < 0.9238795f; // >45 degrees, teleport or timing discontinuity
}
}
