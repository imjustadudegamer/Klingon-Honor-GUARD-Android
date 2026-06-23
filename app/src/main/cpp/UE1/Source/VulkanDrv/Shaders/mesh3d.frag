#version 450
// KHG VulkanDrv Phase 3 — world surface fragment shader. Base texture * lightmap * 2.0 (UE1 overbright,
// per the GLES SF_Lightmap path). Surfaces with no lightmap bind a neutral 0.5-grey map (-> full bright).
// Then the GLES "Glide-FX" world post-process: brightness scale + shadow-region gamma + shadow lift.
// Compiled twice: plain and with ALPHATEST for PF_Masked surfaces.
// push params: x = combined brightness, y = world gamma, z = world shadow lift, w = applyPost (1 for
// OPAQUE lightmapped surfaces; 0 for translucent/modulated glass/forcefield/water so they blend exactly
// as before — post-process must not alter their src colour under ONE,ONE_MINUS_SRC_COLOR/DST blends).
layout(push_constant) uniform Push { mat4 mvp; vec4 params; } pc;
layout(set = 0, binding = 0) uniform sampler2D texBase;
layout(set = 0, binding = 1) uniform sampler2D texLight;
layout(location = 0) in vec2 vUV0;
layout(location = 1) in vec2 vUV1;
layout(location = 0) out vec4 outColor;
void main()
{
    vec4 b = texture(texBase, vUV0);
#ifdef ALPHATEST
    if (b.a < 0.5) discard;
#endif
    vec3 lm = texture(texLight, vUV1).rgb;
    vec3 c = b.rgb * lm * 2.0;

    if (pc.params.w > 0.5)
    {
        // Brightness slider (GLES applies BrightScale to lightmapped world geometry).
        c *= pc.params.x;

        // Shadow-region gamma + shadow lift (GLES SF_Lightmap path): brighten dark areas without
        // washing out lit ones. ShadowMask ~1 in shadows, ~0 in light.
        vec3 cc = clamp(c, 0.0, 1.0);
        float luma = max(max(cc.r, cc.g), cc.b);
        float shadowMask = 1.0 - smoothstep(0.20, 0.85, luma);
        float g = max(pc.params.y, 0.01);
        vec3 gam = pow(cc, vec3(1.0 / g));
        c = mix(c, gam, shadowMask);
        c += pc.params.z * shadowMask * (1.0 - cc);
    }

    outColor = vec4(c, 1.0);
}
