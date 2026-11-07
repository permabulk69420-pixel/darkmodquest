// A single depth atlas contains independent global and local caster layers.
// page = normalized (origin.x, origin.y, faceSize, texelSize).
// params = (near, includeLocalCasters, parallel, valid).
// Faces are laid out 3x2; the local layer is two face rows below global.
vec3 mobileCubeUV(vec3 v, out int face) {
    vec3 a = abs(v);
    float d = max(a.x, max(a.y, a.z));
    vec2 uv;
    if      (d == v.x)  { face = 0; uv = vec2(-v.z, -v.y); }
    else if (d == -v.x) { face = 1; uv = vec2( v.z, -v.y); }
    else if (d == v.y)  { face = 2; uv = vec2( v.x,  v.z); }
    else if (d == -v.y) { face = 3; uv = vec2( v.x, -v.z); }
    else if (d == v.z)  { face = 4; uv = vec2( v.x, -v.y); }
    else               { face = 5; uv = vec2(-v.x, -v.y); }
    return vec3(uv / max(d, 1e-6) * 0.5 + 0.5, d);
}

float mobileShadow(highp sampler2D atlas, vec4 page, vec4 params, mat4 projection,
                   vec3 worldPosition, vec3 lightOrigin, vec3 worldNormal, bool filterShadow) {
    if (params.w == 0.0) return 1.0;
    int face = 0;
    vec3 p;
    float reference;
    if (params.z != 0.0) {
        p = (projection * vec4(worldPosition, 1.0)).xyz * 0.5 + 0.5;
        if (any(lessThan(p, vec3(0.0))) || any(greaterThan(p, vec3(1.0)))) return 1.0;
        reference = p.z - 0.0002;
    } else {
        vec3 fromLight = worldPosition - lightOrigin;
        p = mobileCubeUV(fromLight, face);
        if (p.z <= params.x) return 1.0;
        float facing = max(abs(dot(normalize(-fromLight), worldNormal)), 0.1);
        float worldBias = dot(worldNormal, worldNormal) > 0.25 ? 2.0 * p.z * page.w / (page.z * facing) : 0.0;
        reference = 1.0 - params.x / max(params.x, p.z - worldBias);
    }
    vec2 origin = page.xy + vec2(float(face % 3), float(face / 3)) * page.z;
    vec2 uv = origin + p.xy * page.z;
    vec2 lo = origin + vec2(0.5 * page.w), hi = origin + vec2(page.z - 0.5 * page.w);
    float visibility = 0.0;
    // Bounded PCF: at most four taps per caster layer, independent of eye and
    // distance. Clamp to this face, never read a neighbour's atlas page.
    int samples = filterShadow ? 4 : 1;
    for (int i = 0; i < samples; ++i) {
        vec2 offset = filterShadow ? (vec2(float(i & 1), float(i >> 1)) - 0.5) * page.w : vec2(0.0);
        vec2 tc = clamp(uv + offset, lo, hi);
        float z = textureLod(atlas, tc, 0.0).r;
        if (params.y != 0.0) z = min(z, textureLod(atlas, tc + vec2(0.0, 2.0 * page.z), 0.0).r);
        visibility += step(reference, z);
    }
    return visibility / float(samples);
}
