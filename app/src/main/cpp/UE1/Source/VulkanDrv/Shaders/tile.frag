#version 450
// KHG VulkanDrv Phase 2 — 2D tile fragment shader. Compiled twice: plain and with ALPHATEST
// (for PF_Masked tiles, palette index 0 -> alpha 0 in the upload).
layout(set = 0, binding = 0) uniform sampler2D tex0;
layout(location = 0) in vec2 vUV;
layout(location = 1) in vec4 vColor;
layout(location = 0) out vec4 outColor;
void main()
{
    vec4 t = texture(tex0, vUV);
#ifdef ALPHATEST
    if (t.a < 0.5) discard;
#endif
    outColor = t * vColor;
}
