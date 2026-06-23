#version 460
// [KHG] Phase 4 bloom extract (from UT99). Bloom is DISABLED by config (Bloom=0); compiled only so the
// manager pipeline code builds unchanged. Never runs — KHG has no bloom (not 1:1).
layout(push_constant) uniform BloomPushConstants { float W0,W1,W2,W3,W4,W5,W6,W7; };
layout(binding = 0) uniform sampler2D texSampler;
layout(location = 0) in vec2 texCoord;
layout(location = 0) out vec4 outColor;
void main() { outColor = vec4(max(texture(texSampler, texCoord).rgb - 1.0, 0.0), 0.0); }
