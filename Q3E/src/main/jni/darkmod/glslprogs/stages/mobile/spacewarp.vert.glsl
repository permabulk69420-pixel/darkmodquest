#version 320 es
#extension GL_OVR_multiview2 : require
layout(num_views=2) in;
precision highp float;
precision highp int;
layout(location=0) in vec3 attr_Position;
layout(location=1) in vec3 previousPosition;
layout(location=8) in vec2 attr_TexCoord;
uniform mat4 u_currentMvp[2], u_previousMvp[2], u_textureMatrix;
out highp vec4 currentClip, previousClip;
out highp vec2 textureCoord;
void main() {
	currentClip = u_currentMvp[gl_ViewID_OVR] * vec4(attr_Position,1.0);
	previousClip = u_previousMvp[gl_ViewID_OVR] * vec4(previousPosition,1.0);
	textureCoord = (u_textureMatrix * vec4(attr_TexCoord,0.0,1.0)).xy;
	gl_Position = currentClip;
}
