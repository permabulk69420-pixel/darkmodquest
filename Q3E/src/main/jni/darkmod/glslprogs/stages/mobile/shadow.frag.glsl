#version 320 es
precision highp float;
precision highp int;
uniform sampler2D u_texture;
uniform float u_alphaTest;
in vec2 texCoord;
void main() {
    if (u_alphaTest >= 0.0 && texture(u_texture, texCoord).a <= u_alphaTest) discard;
}
