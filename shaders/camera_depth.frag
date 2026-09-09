#version 450
#extension GL_GOOGLE_include_directive : require
#include "material.glsl"
layout(location=3) in vec4 C;
layout(location=4) in vec2 UV0;
layout(location=5) in vec2 UV1;
void main() {
    if(m.flags.x>.5 && m.baseColor.a*C.a*texture(baseMap,materialUV(0,UV0,UV1)).a<m.flags.y)discard;
    // Use the same fixed-function depth as the later EQUAL PBR passes.
}
