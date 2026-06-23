#version 450
// KHG VulkanDrv Phase 3 — gouraud (actor/mesh/sprite) vertex shader. View-space position via the
// reverse-Z push-constant matrix; per-vertex Gouraud colour + single texture UV.
layout(push_constant) uniform Push { mat4 mvp; } pc;
layout(location = 0) in vec3 inPos;
layout(location = 1) in vec2 inUV;
layout(location = 2) in vec4 inColor;
layout(location = 0) out vec2 vUV;
layout(location = 1) out vec4 vColor;
void main()
{
    gl_Position = pc.mvp * vec4(inPos, 1.0);
    vUV = inUV;
    vColor = inColor;
}
