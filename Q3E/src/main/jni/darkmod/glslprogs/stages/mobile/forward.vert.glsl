#version 320 es
precision highp float;
precision highp int;
// Surface lighting uses GL_EQUAL against the separate depth program.
invariant gl_Position;
#pragma tdm_include "tdm_utils.glsl"
#pragma tdm_include "tdm_interaction.glsl"
in vec4 attr_Position;
in vec4 attr_TexCoord;
in vec3 attr_Tangent;
in vec3 attr_Bitangent;
in vec3 attr_Normal;
in vec4 attr_Color;
uniform mat4 u_modelMatrix;
uniform mat4 u_modelViewMatrix;
uniform mat4 u_projectionMatrix;
uniform vec3 u_viewOrigin;
uniform int u_flags;
uniform int u_vertexColorMode;
uniform vec4 u_bumpMatrix[2], u_diffuseMatrix[2], u_specularMatrix[2], u_parallaxMatrix[2];
out vec2 texNormal, texDiffuse, texSpecular, texParallax, texCoord;
out vec3 positionModel, positionWorld, viewLocal;
out mat3 tangentToModel;
out vec4 vertexColor;
void main() {
    gl_Position = objectPosToClip(attr_Position, u_modelViewMatrix, u_projectionMatrix);
    texNormal = transformSurfaceTexCoords(attr_TexCoord.xy, true, u_bumpMatrix);
    texDiffuse = transformSurfaceTexCoords(attr_TexCoord.xy, true, u_diffuseMatrix);
    texSpecular = transformSurfaceTexCoords(attr_TexCoord.xy, true, u_specularMatrix);
    texParallax = transformSurfaceTexCoords(attr_TexCoord.xy, true, u_parallaxMatrix);
    texCoord = attr_TexCoord.xy;
    positionModel = attr_Position.xyz;
    positionWorld = (u_modelMatrix * attr_Position).xyz;
    tangentToModel = mat3(clamp(attr_Tangent, -1.0, 1.0), clamp(attr_Bitangent, -1.0, 1.0), clamp(attr_Normal, -1.0, 1.0));
    viewLocal = (worldPosToObject(u_viewOrigin, u_modelMatrix) - positionModel) * tangentToModel;
    vertexColor = u_vertexColorMode == 0 ? vec4(1.0) : u_vertexColorMode == 1 ? attr_Color : vec4(1.0) - attr_Color;
}
