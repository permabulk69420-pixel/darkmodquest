#version 320 es
#ifdef QUEST_MULTIVIEW
#extension GL_OVR_multiview2 : require
layout(num_views=2) in;
#endif
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
#ifdef QUEST_MULTIVIEW
uniform mat4 u_modelViewMatrix[2], u_projectionMatrix[2];
uniform vec3 u_viewOrigin[2];
flat out int questEye;
#define QUEST_MODELVIEW u_modelViewMatrix[gl_ViewID_OVR]
#define QUEST_PROJECTION u_projectionMatrix[gl_ViewID_OVR]
#define QUEST_ORIGIN u_viewOrigin[gl_ViewID_OVR]
#else
uniform mat4 u_modelViewMatrix, u_projectionMatrix;
uniform vec3 u_viewOrigin;
#define QUEST_MODELVIEW u_modelViewMatrix
#define QUEST_PROJECTION u_projectionMatrix
#define QUEST_ORIGIN u_viewOrigin
#endif
uniform int u_flags;
uniform int u_vertexColorMode;
uniform vec4 u_bumpMatrix[2], u_diffuseMatrix[2], u_specularMatrix[2], u_parallaxMatrix[2];
out vec2 texNormal, texDiffuse, texSpecular, texParallax, texCoord;
out vec3 positionModel, positionWorld, viewLocal;
out mat3 tangentToModel;
out vec4 vertexColor;
void main() {
#ifdef QUEST_MULTIVIEW
    questEye = int(gl_ViewID_OVR);
#endif
    gl_Position = objectPosToClip(attr_Position, QUEST_MODELVIEW, QUEST_PROJECTION);
    texNormal = transformSurfaceTexCoords(attr_TexCoord.xy, true, u_bumpMatrix);
    texDiffuse = transformSurfaceTexCoords(attr_TexCoord.xy, true, u_diffuseMatrix);
    texSpecular = transformSurfaceTexCoords(attr_TexCoord.xy, true, u_specularMatrix);
    texParallax = transformSurfaceTexCoords(attr_TexCoord.xy, true, u_parallaxMatrix);
    texCoord = attr_TexCoord.xy;
    positionModel = attr_Position.xyz;
    positionWorld = (u_modelMatrix * attr_Position).xyz;
    tangentToModel = mat3(clamp(attr_Tangent, -1.0, 1.0), clamp(attr_Bitangent, -1.0, 1.0), clamp(attr_Normal, -1.0, 1.0));
    viewLocal = (worldPosToObject(QUEST_ORIGIN, u_modelMatrix) - positionModel) * tangentToModel;
    vertexColor = u_vertexColorMode == 0 ? vec4(1.0) : u_vertexColorMode == 1 ? attr_Color : vec4(1.0) - attr_Color;
}
