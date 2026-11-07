/* The Dark Mod GPL Source Code. See LICENSE.txt for license terms. */
#pragma tdm_define "QUEST_FULL_PRECISION_LIGHTING"
#ifdef QUEST_FULL_PRECISION_LIGHTING
// Gameplay lightgem measurements retain the original equations and precision.
#define questAdvancedInteraction computeAdvancedInteraction
#else
#define QUEST_UNIT mediump

// Only bounded unit vectors, Fresnel terms and colour weights use FP16.
// Directions are normalized in highp before conversion. Keep the glossy
// half-vector dot/power in highp to avoid stepping in narrow highlights.
// Material texture colours and final accumulation remain highp. Direct-light
// RGB parameters are normalized on the CPU; restore their scale outside here
// so HDR and signed material stages retain their original exponent range.
highp vec3 questAdvancedInteraction(InteractionGeometry props,
    QUEST_UNIT vec3 diffuseParam, highp vec4 diffuseTexel,
    QUEST_UNIT vec3 specularParam, highp vec4 specularTexel,
    highp vec3 vertexColor, bool togglingFix) {
    QUEST_UNIT vec3 lightDirection = props.localL;
    QUEST_UNIT vec3 viewDirection = props.localV;
    QUEST_UNIT vec3 normal = props.localN;
    QUEST_UNIT float NdotL = clamp(dot(normal, lightDirection), 0.0, 1.0);
    QUEST_UNIT float NdotV = clamp(dot(normal, viewDirection), 0.0, 1.0);
    QUEST_UNIT float fresnel = pow(1.0 - NdotV, 4.0);
    QUEST_UNIT float rim = fresnel * clamp(NdotL - 0.3, 0.0, 0.5) * 1.8;
    QUEST_UNIT float fresnelCoeff = fresnel * 0.23 + 0.023;
    QUEST_UNIT float R2f = clamp(lightDirection.z * 4.0, 0.0, 1.0);
    highp float specularPower = mix(10.0, 30.0, specularTexel.z);
    QUEST_UNIT float specularCoeff = pow(props.NdotH, specularPower) * 120.0 * fresnelCoeff;
    QUEST_UNIT float adjusted = NdotL;
    if (togglingFix) {
        QUEST_UNIT float meshDot = max(lightDirection.z, 0.0);
        if (meshDot < min(0.25, NdotL)) adjusted = mix(meshDot, NdotL, meshDot / 0.25);
    }
    QUEST_UNIT float multiplier = adjusted + rim * R2f;
    // Vertex colour is bounded; leave material/HDR values outside the FP16
    // products. Cast weight vectors into the highp colour expression only here.
    QUEST_UNIT vec3 boundedVertexColor = vertexColor;
    QUEST_UNIT vec3 diffuseWeight = diffuseParam * (multiplier * boundedVertexColor);
    QUEST_UNIT vec3 specularWeight = specularParam * ((specularCoeff * R2f * multiplier) * boundedVertexColor);
    return diffuseTexel.rgb * vec3(diffuseWeight)
         + specularTexel.rgb * vec3(specularWeight);
}
#endif
