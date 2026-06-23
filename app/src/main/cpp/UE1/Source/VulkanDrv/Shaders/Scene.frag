#version 460
// [KHG] VulkanDrv Phase 4 — bindless Scene fragment shader (adapted verbatim from UT99VulkanDrv
// FileResource.cpp shaders/Scene.frag). This IS the OpenGL-driver 1:1 math:
//   base*color, then *lightmap*2 overbright (oneXBlending=2.0 unless flag 64), macro/detail/fog passes,
//   alpha-test for masked. The lightmap is doubled again at upload (TextureUploader_BGRA8_LM, 7-bit <<1),
//   so full lightmap = base*2 overbright, exactly like OpenGL.cpp (RGB_SCALE=2 + 2*Src upload).
// Compiled twice: plain and -DALPHATEST (PF_Masked surfaces). No per-surface brightness wash / shadow lift
// (those were port inventions; brightness is the global gamma ramp in the present pass, like the GL driver).
#extension GL_EXT_nonuniform_qualifier : enable

layout(binding = 0) uniform sampler2D textures[];

layout(location = 0) flat in uint flags;
layout(location = 1) centroid in vec2 texCoord;
layout(location = 2) in vec2 texCoord2;
layout(location = 3) in vec2 texCoord3;
layout(location = 4) in vec2 texCoord4;
layout(location = 5) in vec4 color;
layout(location = 6) flat in uint hitIndex;
layout(location = 7) flat in ivec4 textureBinds;

layout(location = 0) out vec4 outColor;
layout(location = 1) out uint outHitIndex;

vec4 darkClamp(vec4 c)
{
    // Make all textures a little darker as some of the textures (i.e coronas) never become completely
    // black as they should have
    float cutoff = 3.1/255.0;
    return vec4(clamp((c.rgb - cutoff) / (1.0 - cutoff), 0.0, 1.0), c.a);
}

vec4 textureTex(vec2 uv) { return texture(textures[nonuniformEXT(textureBinds.x)], uv); }
vec4 textureMacro(vec2 uv) { return texture(textures[nonuniformEXT(textureBinds.y)], uv); }
vec4 textureDetail(vec2 uv) { return texture(textures[nonuniformEXT(textureBinds.z)], uv); }
vec4 textureLightmap(vec2 uv) { return texture(textures[nonuniformEXT(textureBinds.w)], uv); }

void main()
{
    float actorXBlending = (flags & 32) != 0 ? 1.5 : 1.0;
    float oneXBlending = (flags & 64) != 0 ? 1.0 : 2.0;

    outColor = darkClamp(textureTex(texCoord)) * color;
    outColor.rgb *= actorXBlending;

    if ((flags & 2) != 0) // Macro texture
    {
        outColor *= darkClamp(textureMacro(texCoord3));
    }

    if ((flags & 1) != 0) // Lightmap
    {
        outColor.rgb *= clamp(textureLightmap(texCoord2).rgb, 0.0, 1.0) * oneXBlending;
    }

    if ((flags & 4) != 0) // Detail texture
    {
        float fadedistance = 380.0f;
        float a = clamp(2.0f - (1.0f / gl_FragCoord.w) / fadedistance, 0.0f, 1.0f);
        vec4 detailColor = (textureDetail(texCoord4) - 0.5) * 0.8 + 1.0;
        outColor.rgb = mix(outColor.rgb, outColor.rgb * detailColor.rgb, a);
    }
    else if ((flags & 8) != 0) // Fog map
    {
        vec4 fogcolor = textureDetail(texCoord4);
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
