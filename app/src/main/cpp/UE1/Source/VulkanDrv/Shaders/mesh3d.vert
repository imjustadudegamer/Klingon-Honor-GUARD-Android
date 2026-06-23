#version 450
// KHG VulkanDrv Phase 3 — world surface vertex shader. Position is UE1 view-space (Pts->Point);
// reverse-Z, Y-down projection supplied as a push constant. Two UV sets: base + lightmap.
layout(push_constant) uniform Push { mat4 mvp; vec4 params; } pc;
layout(location = 0) in vec3 inPos;
layout(location = 1) in vec2 inUV0;
layout(location = 2) in vec2 inUV1;
layout(location = 0) out vec2 vUV0;
layout(location = 1) out vec2 vUV1;
void main()
{
    gl_Position = pc.mvp * vec4(inPos, 1.0);
    vUV0 = inUV0;
    vUV1 = inUV1;
}
