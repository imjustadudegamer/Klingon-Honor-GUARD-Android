#version 460
layout(push_constant) uniform BloomPushConstants { float W0,W1,W2,W3,W4,W5,W6,W7; };
layout(binding = 0) uniform sampler2D texSampler;
layout(location = 0) in vec2 texCoord;
layout(location = 0) out vec4 outColor;
void main()
{
#if defined(BLUR_HORIZONTAL)
    outColor =
        textureOffset(texSampler, texCoord, ivec2( 0,0))*W0 + textureOffset(texSampler, texCoord, ivec2( 1,0))*W1 +
        textureOffset(texSampler, texCoord, ivec2(-1,0))*W2 + textureOffset(texSampler, texCoord, ivec2( 2,0))*W3 +
        textureOffset(texSampler, texCoord, ivec2(-2,0))*W4 + textureOffset(texSampler, texCoord, ivec2( 3,0))*W5 +
        textureOffset(texSampler, texCoord, ivec2(-3,0))*W6;
#else
    outColor =
        textureOffset(texSampler, texCoord, ivec2(0, 0))*W0 + textureOffset(texSampler, texCoord, ivec2(0, 1))*W1 +
        textureOffset(texSampler, texCoord, ivec2(0,-1))*W2 + textureOffset(texSampler, texCoord, ivec2(0, 2))*W3 +
        textureOffset(texSampler, texCoord, ivec2(0,-2))*W4 + textureOffset(texSampler, texCoord, ivec2(0, 3))*W5 +
        textureOffset(texSampler, texCoord, ivec2(0,-3))*W6;
#endif
}
