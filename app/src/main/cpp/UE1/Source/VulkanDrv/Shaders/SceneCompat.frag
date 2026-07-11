#version 450
// [KHG] Non-bindless COMPATIBILITY Scene fragment shader. Same 1:1 lighting math as Scene.frag, but samples
// four FIXED sampler2D bindings instead of a runtime-indexed bindless array `textures[]`. No
// GL_EXT_nonuniform_qualifier, no runtimeDescriptorArray => runs on GPUs without descriptor indexing.
// Compiled at --target-env=vulkan1.0 (SPIR-V 1.0). Compiled twice: plain and -DALPHATEST (PF_Masked).
//
// Binding <-> bindless slot mapping (see DescriptorSetManager::GetCompatTextureSet):
//   binding 0 texBase      = bindless textureBinds.x (base)
//   binding 1 texLightmap  = bindless textureBinds.w (lightmap)
//   binding 2 texDetailFog = bindless textureBinds.z (detail OR fog map; mutually exclusive)
//   binding 3 texMacro     = bindless textureBinds.y (macro)

layout(binding = 0) uniform sampler2D texBase;
layout(binding = 1) uniform sampler2D texLightmap;
layout(binding = 2) uniform sampler2D texDetailFog;
layout(binding = 3) uniform sampler2D texMacro;

layout(location = 0) flat in uint flags;
layout(location = 1) centroid in vec2 texCoord;
layout(location = 2) in vec2 texCoord2;
layout(location = 3) in vec2 texCoord3;
layout(location = 4) in vec2 texCoord4;
layout(location = 5) in vec4 color;
layout(location = 6) flat in uint hitIndex;
layout(location = 8) in float clipNear;

layout(location = 0) out vec4 outColor;
layout(location = 1) out uint outHitIndex;

vec4 darkClamp(vec4 c)
{
    // Match Scene.frag: nudge near-black texels (coronas etc.) to true black.
    float cutoff = 3.1/255.0;
    return vec4(clamp((c.rgb - cutoff) / (1.0 - cutoff), 0.0, 1.0), c.a);
}

void main()
{
    if (clipNear < 0.0) discard; // near-plane clip (replaces gl_ClipDistance)

    float actorXBlending = (flags & 32) != 0 ? 1.5 : 1.0;
    float oneXBlending = (flags & 64) != 0 ? 1.0 : 2.0;

    outColor = darkClamp(texture(texBase, texCoord)) * color;
    outColor.rgb *= actorXBlending;

    if ((flags & 2) != 0) // Macro texture
    {
        outColor *= darkClamp(texture(texMacro, texCoord3));
    }

    if ((flags & 1) != 0) // Lightmap
    {
        outColor.rgb *= clamp(texture(texLightmap, texCoord2).rgb, 0.0, 1.0) * oneXBlending;
    }

    if ((flags & 4) != 0) // Detail texture
    {
        float fadedistance = 380.0f;
        float a = clamp(2.0f - (1.0f / gl_FragCoord.w) / fadedistance, 0.0f, 1.0f);
        vec4 detailColor = (texture(texDetailFog, texCoord4) - 0.5) * 0.8 + 1.0;
        outColor.rgb = mix(outColor.rgb, outColor.rgb * detailColor.rgb, a);
    }
    else if ((flags & 8) != 0) // Fog map
    {
        vec4 fogcolor = texture(texDetailFog, texCoord4);
        outColor.rgb = fogcolor.rgb + outColor.rgb * (1.0 - fogcolor.a);
    }
    else if ((flags & 16) != 0) // Fog color
    {
        vec4 fogcolor = vec4(texCoord2, texCoord3);
        outColor.rgb = fogcolor.rgb + outColor.rgb * (1.0 - fogcolor.a);
    }

    #if defined(ALPHATEST)
    if (outColor.a < 0.5) discard;
    #endif

    outColor = clamp(outColor, 0.0, 1.0);

    outHitIndex = hitIndex;
}
