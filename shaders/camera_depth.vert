#version 450
#extension GL_GOOGLE_include_directive : require
#include "lighting.glsl"
layout(location=0) in vec3 position;
layout(location=3) in vec4 vertexColor;
layout(location=4) in vec2 uv0;
layout(location=5) in vec2 uv1;
layout(location=3) out vec4 C;
layout(location=4) out vec2 UV0;
layout(location=5) out vec2 UV1;
invariant gl_Position;
void main() {
    InstanceData instance=instances[drawIndices[gl_InstanceIndex]];
    vec4 p=instance.world*vec4(position,1);
    C=vertexColor*instance.color; UV0=uv0; UV1=uv1;
    gl_Position=(pass.mode.x==1||pass.mode.x==2?pass.lightVP:frame.vp)*p;
}
