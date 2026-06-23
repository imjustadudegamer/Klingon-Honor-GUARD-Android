#version 460
// [KHG] Phase 4 present fragment shader (from UT99 shaders/Present.frag, single variant baked:
// GAMMA_MODE_D3D9 + COLOR_CORRECT_MODE0, no HDR). At default config (GammaCorrection=1, Contrast=1,
// Saturation=1, Brightness=0) this is a passthrough + ordered dither — the 1:1 scene fidelity lives in
// Scene.frag. Brightness/gamma can be driven later via the push constants (the GL gamma-ramp equivalent).
layout(push_constant) uniform PresentPushConstants
{
    float Contrast;
    float Saturation;
    float Brightness;
    float HdrScale;
    vec4 GammaCorrection;
};

layout(binding = 0) uniform sampler2D texSampler;
layout(binding = 1) uniform sampler2D texDither;
layout(location = 0) in vec2 texCoord;
layout(location = 0) out vec4 outColor;

vec3 dither(vec3 c)
{
    vec2 texSize = vec2(textureSize(texDither, 0));
    float threshold = texture(texDither, gl_FragCoord.xy / texSize).r;
    return floor(c.rgb * 255.0 + threshold) / 255.0;
}

vec3 gammaCorrect(vec3 c)
{
    return pow(c, GammaCorrection.xyz);
}

vec3 colorCorrect(vec3 c)
{
    float v = c.r + c.g + c.b;
    vec3 valgray = vec3(v, v, v) * (1 - Saturation) / 3 + c * Saturation;
    vec3 val = valgray * Contrast - (Contrast - 1.0) * 0.5;
    val += Brightness * 0.5;
    return max(val, vec3(0.0, 0.0, 0.0));
}

void main()
{
    vec3 color = gammaCorrect(colorCorrect(texture(texSampler, texCoord).rgb));
    outColor = vec4(dither(color), 1.0f);
}
