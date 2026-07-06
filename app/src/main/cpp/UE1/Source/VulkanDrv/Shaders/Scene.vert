#version 460
// [KHG] VulkanDrv Phase 4 — bindless Scene vertex shader (adapted verbatim from UT99VulkanDrv
// FileResource.cpp shaders/Scene.vert; precompiled to SPIR-V offline since we have no glslang on Android).
#extension GL_EXT_nonuniform_qualifier : enable

layout(push_constant) uniform ScenePushConstants
{
    mat4 objectToProjection;
    vec4 nearClip;
    uint uHitIndex;
    uint padding1, padding2, padding3;
};

out gl_PerVertex
{
    vec4 gl_Position;
};

layout(location = 0) in uint aFlags;
layout(location = 1) in vec3 aPosition;
layout(location = 2) in vec2 aTexCoord;
layout(location = 3) in vec2 aTexCoord2;
layout(location = 4) in vec2 aTexCoord3;
layout(location = 5) in vec2 aTexCoord4;
layout(location = 6) in vec4 aColor;
layout(location = 7) in ivec4 aTextureBinds;

layout(location = 0) flat out uint flags;
layout(location = 1) out vec2 texCoord;
layout(location = 2) out vec2 texCoord2;
layout(location = 3) out vec2 texCoord3;
layout(location = 4) out vec2 texCoord4;
layout(location = 5) out vec4 color;
layout(location = 6) flat out uint hitIndex;
layout(location = 7) flat out ivec4 textureBinds;
layout(location = 8) out float clipNear; // [KHG] near-plane clip distance -> fragment discard (Mali-safe, replaces gl_ClipDistance)

void main()
{
    gl_Position = objectToProjection * vec4(aPosition, 1.0);
    clipNear = dot(nearClip, vec4(aPosition, 1.0));
    flags = aFlags;
    texCoord = aTexCoord;
    texCoord2 = aTexCoord2;
    texCoord3 = aTexCoord3;
    texCoord4 = aTexCoord4;
    color = aColor;
    hitIndex = uHitIndex;
    textureBinds = aTextureBinds;
}
