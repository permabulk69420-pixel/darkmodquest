#version 320 es
// Quest depth-only shadows: ordinary cube-face clipping, no clip-distance extension.
precision highp float;
precision highp int;
in vec4 attr_Position;
in vec4 attr_TexCoord;
uniform mat4 u_modelMatrix;
uniform mat4 u_textureMatrix;
uniform mat4 u_shadowProjection;
uniform vec3 u_lightOrigin;
uniform int u_face;
uniform int u_parallel;
uniform float u_alphaTest;
out vec2 texCoord;

void main() {
    vec4 world = u_modelMatrix * attr_Position;
    texCoord = u_alphaTest >= 0.0 ? (u_textureMatrix * attr_TexCoord).xy : vec2(0.0);
    if (u_parallel != 0) {
        gl_Position = u_shadowProjection * world;
        return;
    }
    vec3 v = world.xyz - u_lightOrigin;
    vec3 p;
    if      (u_face == 0) p = vec3(-v.z, -v.y, -v.x);
    else if (u_face == 1) p = vec3( v.z, -v.y,  v.x);
    else if (u_face == 2) p = vec3( v.x,  v.z, -v.y);
    else if (u_face == 3) p = vec3( v.x, -v.z,  v.y);
    else if (u_face == 4) p = vec3( v.x, -v.y, -v.z);
    else                 p = vec3(-v.x, -v.y,  v.z);
    // Same near=1, infinite far depth convention as TDM shadow maps.
    gl_Position = vec4(p.xy, -p.z - 2.0, -p.z);
}
