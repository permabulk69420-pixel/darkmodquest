#pragma tdm_define "QUEST_MULTIVIEW"
#ifdef GL_EXT_fragment_shading_rate
#extension GL_EXT_fragment_shading_rate : enable
#endif
precision highp float;
precision highp int;
// Lighting never writes depth or stencil. Reject hidden fragments before
// material/parallax/shadow work, including programs with mirror-plane discard.
layout(early_fragment_tests) in;
#pragma tdm_include "tdm_utils.glsl"
#pragma tdm_include "tdm_lightproject.glsl"
#pragma tdm_include "tdm_interaction.glsl"
#pragma tdm_include "stages/mobile/brdf.glsl"
#pragma tdm_include "tdm_compression.glsl"
#pragma tdm_include "tdm_parallax.glsl"
#pragma tdm_include "tdm_mobile_shadows.glsl"

uniform sampler2D u_normalTexture, u_diffuseTexture, u_specularTexture, u_parallaxTexture;
uniform sampler2D u_projection0, u_projection1, u_falloff0, u_falloff1;
uniform samplerCube u_cube0, u_cube1, u_ambientDiffuse0, u_ambientDiffuse1, u_ambientSpecular0, u_ambientSpecular1;
uniform highp sampler2D u_shadowAtlas;
uniform mat4 u_modelMatrix;
#ifdef QUEST_MULTIVIEW
flat in int questEye;
uniform highp sampler2DArray u_ssaoTexture;
uniform vec3 u_viewOrigin[2];
#define QUEST_ORIGIN u_viewOrigin[questEye]
#else
uniform sampler2D u_ssaoTexture;
uniform vec3 u_viewOrigin;
#define QUEST_ORIGIN u_viewOrigin
#endif
uniform vec4 u_clipPlane;
uniform vec2 u_renderResolution, u_ambientParams;
uniform int u_flags, u_lightCount, u_lightFlags[QUEST_LIGHT_CAPACITY], u_lightTextureSet[QUEST_LIGHT_CAPACITY], u_shadowFilter;
uniform vec3 u_lightOrigin[QUEST_LIGHT_CAPACITY];
uniform vec4 u_diffuseColor[QUEST_LIGHT_CAPACITY], u_specularColor[QUEST_LIGHT_CAPACITY];
uniform mat4 u_lightProjection[QUEST_LIGHT_CAPACITY], u_shadowProjection[QUEST_LIGHT_CAPACITY];
uniform vec4 u_lightTextureMatrix[2*QUEST_LIGHT_CAPACITY], u_shadowPage[QUEST_LIGHT_CAPACITY], u_shadowParams[QUEST_LIGHT_CAPACITY];
uniform vec4 u_bumpMatrix[2], u_diffuseMatrix[2], u_specularMatrix[2];
uniform vec2 u_parallaxHeightScale;
uniform ivec3 u_parallaxIterations;
uniform float u_parallaxGrazingAngle, u_parallaxShadowSoftness;
in vec2 texNormal, texDiffuse, texSpecular, texParallax, texCoord;
in vec3 positionModel, positionWorld, viewLocal;
in mat3 tangentToModel;
in vec4 vertexColor;
out vec4 fragColor;

vec4 illuminate(int i, sampler2D projectionTexture, samplerCube projectionCube, sampler2D falloffTexture,
                samplerCube ambientDiffuse, samplerCube ambientSpecular,
                vec4 diffuseTexel, vec4 specularTexel, vec3 normal, vec3 offset, vec3 scaledOffset) {
    int flags = u_lightFlags[i];
    vec4 lightTex = computeLightTex(u_lightProjection[i], vec4(positionModel, 1.0));
    vec4 rows[2] = vec4[2](u_lightTextureMatrix[2*i], u_lightTextureMatrix[2*i+1]);
    vec3 lightColor = checkFlag(flags, SFL_LIGHT_CUBIC)
        ? projFalloffOfCubicLight(projectionCube, lightTex).rgb
        : projFalloffOfNormalLight(projectionTexture, falloffTexture, rows, lightTex).rgb;
    // Projection/falloff is evaluated before this branch, preserving texture
    // derivatives. A zero contribution needs no BRDF, parallax shadow or PCF.
    if (!checkFlag(flags, 0x100000) && !checkFlag(u_flags, SFL_SURFACE_HAS_PARALLAX_TEXTURE)
        && all(equal(lightColor, vec3(0.0))))
        return vec4(0.0, 0.0, 0.0, 1.0);
    if (checkFlag(flags, 0x100000)) {
        AmbientGeometry geom = computeAmbientGeometry(QUEST_ORIGIN - positionWorld, normal, tangentToModel, mat3(u_modelMatrix));
        vec4 color = computeAmbientInteraction(geom, u_diffuseColor[i].rgb, diffuseTexel, u_specularColor[i].rgb, specularTexel,
            vertexColor.rgb, checkFlag(flags, SFL_LIGHT_AMBIENT_HAS_DIFFUSE_CUBEMAP), checkFlag(flags, SFL_LIGHT_AMBIENT_HAS_SPECULAR_CUBEMAP),
            ambientDiffuse, ambientSpecular, u_ambientParams.x, u_ambientParams.y);
#ifdef QUEST_MULTIVIEW
        float ao = checkFlag(flags, SFL_LIGHT_AMBIENT_HAS_SSAO) ? texture(u_ssaoTexture, vec3(gl_FragCoord.xy / u_renderResolution, float(questEye))).r : 1.0;
#else
        float ao = checkFlag(flags, SFL_LIGHT_AMBIENT_HAS_SSAO) ? texture(u_ssaoTexture, gl_FragCoord.xy / u_renderResolution).r : 1.0;
#endif
        return vec4(lightColor * ao * color.rgb, color.a);
    }
    vec3 toLight = (worldPosToObject(u_lightOrigin[i], u_modelMatrix) - positionModel) * tangentToModel - scaledOffset;
    InteractionGeometry geom = computeInteractionGeometry(toLight, viewLocal - scaledOffset, normal);
    vec3 color = questAdvancedInteraction(geom, u_diffuseColor[i].rgb, diffuseTexel,
        u_specularColor[i].rgb, specularTexel, vertexColor.rgb, checkFlag(u_flags, SFL_INTERACTION_BUMPMAP_LIGHT_TOGGLING_FIX));
    float parallaxShadow = 1.0;
    if (checkFlag(u_flags, SFL_SURFACE_HAS_PARALLAX_TEXTURE) && u_parallaxIterations.z > 0)
        parallaxShadow = computeParallaxShadow(u_parallaxTexture, u_parallaxHeightScale, texParallax, offset, toLight,
                                              u_parallaxIterations.z, u_parallaxShadowSoftness);
    vec3 worldOffset = checkFlag(u_flags, SFL_INTERACTION_PARALLAX_OFFSET_EXTERNAL_SHADOWS)
        ? mat3(u_modelMatrix) * tangentToModel * scaledOffset : vec3(0.0);
    // Bias from geometric normals avoids normal-map-dependent shadow acne.
    vec3 worldNormal = normalize(mat3(u_modelMatrix) * tangentToModel[2]);
    float visibility = mobileShadow(u_shadowAtlas, u_shadowPage[i], u_shadowParams[i], u_shadowProjection[i],
        positionWorld + worldOffset, u_lightOrigin[i], worldNormal, u_shadowFilter != 0);
    return vec4(color * lightColor * parallaxShadow * visibility, 1.0);
}

vec4 illuminateSlot(int i, vec4 diffuseTexel, vec4 specularTexel, vec3 normal, vec3 offset, vec3 scaledOffset) {
#if QUEST_LIGHT_CAPACITY > 1
    if (u_lightTextureSet[i] != 0)
        return illuminate(i, u_projection1, u_cube1, u_falloff1, u_ambientDiffuse1, u_ambientSpecular1,
                          diffuseTexel, specularTexel, normal, offset, scaledOffset);
#endif
    return illuminate(i, u_projection0, u_cube0, u_falloff0, u_ambientDiffuse0, u_ambientSpecular0,
                      diffuseTexel, specularTexel, normal, offset, scaledOffset);
}

void main() {
    if (dot(u_clipPlane, vec4(positionWorld, 1.0)) < 0.0) discard;
    vec3 offset = vec3(0.0), scaledOffset = vec3(0.0);
    vec2 nTC = texNormal, dTC = texDiffuse, sTC = texSpecular;
    bool parallax = checkFlag(u_flags, SFL_SURFACE_HAS_PARALLAX_TEXTURE);
    if (parallax) {
        offset = computeParallaxOffset(u_parallaxTexture, u_parallaxHeightScale, texParallax, viewLocal,
                                      u_parallaxGrazingAngle, u_parallaxIterations.xy);
        nTC += transformSurfaceTexOffset(offset.xy, true, u_bumpMatrix);
        dTC += transformSurfaceTexOffset(offset.xy, true, u_diffuseMatrix);
        sTC += transformSurfaceTexOffset(offset.xy, true, u_specularMatrix);
        scaledOffset = scaleTexcoordOffsetToModelSpace(offset, texCoord, positionModel, tangentToModel);
    }
    // Material fetches and parallax tracing happen once for up to eight lights.
    vec4 normalTexel = textureGrad(u_normalTexture, nTC, dFdx(texNormal), dFdy(texNormal));
    vec4 diffuseTexel = textureGrad(u_diffuseTexture, dTC, dFdx(texDiffuse), dFdy(texDiffuse));
    vec4 specularTexel = textureGrad(u_specularTexture, sTC, dFdx(texSpecular), dFdy(texSpecular));
    vec3 normal = unpackSurfaceNormal(normalTexel, true, checkFlag(u_flags, SFL_SURFACE_NORMAL_TEXTURE_RGTC));
    fragColor = illuminateSlot(0, diffuseTexel, specularTexel, normal, offset, scaledOffset);
#if QUEST_LIGHT_CAPACITY > 1
    if (u_lightCount > 1) fragColor += illuminateSlot(1, diffuseTexel, specularTexel, normal, offset, scaledOffset);
#endif
#if QUEST_LIGHT_CAPACITY > 2
    if (u_lightCount > 2) fragColor += illuminateSlot(2, diffuseTexel, specularTexel, normal, offset, scaledOffset);
#endif
#if QUEST_LIGHT_CAPACITY > 3
    if (u_lightCount > 3) fragColor += illuminateSlot(3, diffuseTexel, specularTexel, normal, offset, scaledOffset);
#endif
#if QUEST_LIGHT_CAPACITY > 4
    if (u_lightCount > 4) fragColor += illuminateSlot(4, diffuseTexel, specularTexel, normal, offset, scaledOffset);
#endif
#if QUEST_LIGHT_CAPACITY > 5
    if (u_lightCount > 5) fragColor += illuminateSlot(5, diffuseTexel, specularTexel, normal, offset, scaledOffset);
#endif
#if QUEST_LIGHT_CAPACITY > 6
    if (u_lightCount > 6) fragColor += illuminateSlot(6, diffuseTexel, specularTexel, normal, offset, scaledOffset);
#endif
#if QUEST_LIGHT_CAPACITY > 7
    if (u_lightCount > 7) fragColor += illuminateSlot(7, diffuseTexel, specularTexel, normal, offset, scaledOffset);
#endif
}
