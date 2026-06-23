#version 460
layout(push_constant) uniform BloomPushConstants { float W0,W1,W2,W3,W4,W5,W6,W7; };
layout(binding = 0) uniform sampler2D texSampler;
layout(location = 0) in vec2 texCoord;
layout(location = 0) out vec4 outColor;
void main() { outColor = texture(texSampler, texCoord); }
