#version 460
// [KHG] Phase 4 present/postprocess fullscreen-triangle vertex shader (from UT99 shaders/PPStep.vert).
layout(location = 0) out vec2 texCoord;

vec2 positions[6] = vec2[](
    vec2(-1.0, -1.0),
    vec2( 1.0, -1.0),
    vec2(-1.0,  1.0),
    vec2(-1.0,  1.0),
    vec2( 1.0, -1.0),
    vec2( 1.0,  1.0)
);

void main()
{
    vec4 pos = vec4(positions[gl_VertexIndex], 0.0, 1.0);
    gl_Position = pos;
    texCoord = pos.xy * 0.5 + 0.5;
}
