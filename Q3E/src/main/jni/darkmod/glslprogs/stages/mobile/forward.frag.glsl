#version 320 es
precision highp float;
precision highp int;
#pragma tdm_include "tdm_utils.glsl"
#pragma tdm_include "tdm_lightproject.glsl"
#pragma tdm_include "tdm_interaction.glsl"
#pragma tdm_include "tdm_compression.glsl"
#pragma tdm_include "tdm_parallax.glsl"
#pragma tdm_include "tdm_mobile_shadows.glsl"

uniform sampler2D u_normalTexture, u_diffuseTexture, u_specularTexture, u_parallaxTexture;
uniform sampler2D u_projection0, u_projection1, u_falloff0, u_falloff1;
uniform samplerCube u_cube0, u_cube1, u_ambientDiffuse0, u_ambientDiffuse1, u_ambientSpecular0, u_ambientSpecular1;
uniform highp sampler2D u_shadowAtlas;
uniform sampler2D u_ssaoTexture;
uniform mat4 u_modelMatrix;
uniform vec3 u_viewOrigin;
uniform vec4 u_clipPlane;
uniform vec2 u_renderResolution, u_ambientParams;
uniform int u_flags, u_lightCount, u_lightFlags[2], u_shadowFilter;
uniform vec3 u_lightOrigin[2];
uniform vec4 u_diffuseColor[2], u_specularColor[2];
uniform mat4 u_lightProjection[2], u_shadowProjection[2];
uniform vec4 u_lightTextureMatrix[4], u_shadowPage[2], u_shadowParams[2];
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
    if (checkFlag(flags, 0x100000)) {
        AmbientGeometry geom = computeAmbientGeometry(u_viewOrigin - positionWorld, normal, tangentToModel, mat3(u_modelMatrix));
        vec4 color = computeAmbientInteraction(geom, u_diffuseColor[i].rgb, diffuseTexel, u_specularColor[i].rgb, specularTexel,
            vertexColor.rgb, checkFlag(flags, SFL_LIGHT_AMBIENT_HAS_DIFFUSE_CUBEMAP), checkFlag(flags, SFL_LIGHT_AMBIENT_HAS_SPECULAR_CUBEMAP),
            ambientDiffuse, ambientSpecular, u_ambientParams.x, u_ambientParams.y);
        float ao = checkFlag(flags, SFL_LIGHT_AMBIENT_HAS_SSAO) ? texture(u_ssaoTexture, gl_FragCoord.xy / u_renderResolution).r : 1.0;
        return vec4(lightColor * ao * color.rgb, color.a);
    }
    vec3 toLight = (worldPosToObject(u_lightOrigin[i], u_modelMatrix) - positionModel) * tangentToModel - scaledOffset;
    InteractionGeometry geom = computeInteractionGeometry(toLight, viewLocal - scaledOffset, normal);
    vec3 color = computeAdvancedInteraction(geom, u_diffuseColor[i].rgb, diffuseTexel,
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
    // Material fetches (and parallax tracing) happen once for both lights.
    vec4 normalTexel = textureGrad(u_normalTexture, nTC, dFdx(texNormal), dFdy(texNormal));
    vec4 diffuseTexel = textureGrad(u_diffuseTexture, dTC, dFdx(texDiffuse), dFdy(texDiffuse));
    vec4 specularTexel = textureGrad(u_specularTexture, sTC, dFdx(texSpecular), dFdy(texSpecular));
    vec3 normal = unpackSurfaceNormal(normalTexel, true, checkFlag(u_flags, SFL_SURFACE_NORMAL_TEXTURE_RGTC));
    fragColor = illuminate(0, u_projection0, u_cube0, u_falloff0, u_ambientDiffuse0, u_ambientSpecular0,
                           diffuseTexel, specularTexel, normal, offset, scaledOffset);
    // Constant sampler indices are required by GLES. A second bounded light
    // halves material fetches/draw submission on surfaces with overlapping lights.
    if (u_lightCount == 2)
        fragColor += illuminate(1, u_projection1, u_cube1, u_falloff1, u_ambientDiffuse1, u_ambientSpecular1,
                                diffuseTexel, specularTexel, normal, offset, scaledOffset);
}
