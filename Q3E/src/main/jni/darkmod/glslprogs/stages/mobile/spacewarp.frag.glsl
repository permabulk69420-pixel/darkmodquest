#version 320 es
precision highp float;
precision highp int;
in highp vec4 currentClip, previousClip;
in highp vec2 textureCoord;
uniform highp sampler2D u_texture;
uniform highp float u_alphaTest, u_alpha;
layout(location=0) out highp vec4 motion;
void main() {
	if (u_alphaTest >= 0.0 && texture(u_texture,textureCoord).a*u_alpha <= u_alphaTest) discard;
	// XR_FB_space_warp requires current-minus-previous NDC in all RGB channels.
	// Alpha 0 marks rendered geometry; clear alpha 1 leaves background handling
	// to the compositor. Keep clip positions, depth and division at highp.
	vec3 delta = previousClip.w > 0.00001 ?
		currentClip.xyz/currentClip.w - previousClip.xyz/previousClip.w : vec3(0.0);
	motion = vec4(delta,0.0);
}
