#version 450
// [KHG] Non-bindless COMPATIBILITY Scene vertex shader. Identical to Scene.vert EXCEPT it omits the bindless
// texture-index passthrough (aTextureBinds/textureBinds) and the GL_EXT_nonuniform_qualifier enable, so it
// carries no descriptor-indexing capability. Compiled at --target-env=vulkan1.0 (SPIR-V 1.0) so it runs on
// GPUs without descriptor indexing, down to Vulkan 1.0. The vertex buffer still contains the TextureBinds
// attribute at location 7; leaving it unconsumed here is legal (unused vertex attributes are ignored).

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
// location 7 (aTextureBinds) is present in the vertex buffer but unused by the compatibility path.

layout(location = 0) flat out uint flags;
layout(location = 1) out vec2 texCoord;
layout(location = 2) out vec2 texCoord2;
layout(location = 3) out vec2 texCoord3;
layout(location = 4) out vec2 texCoord4;
layout(location = 5) out vec4 color;
layout(location = 6) flat out uint hitIndex;
layout(location = 8) out float clipNear; // near-plane clip distance -> fragment discard (Mali-safe)

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
}
