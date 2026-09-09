#version 450
#extension GL_GOOGLE_include_directive : require
#include "lighting.glsl"
layout(location=0) in vec3 position;
layout(location=1) in vec3 normal;
layout(location=2) in vec4 tangent;
layout(location=3) in vec4 vertexColor;
layout(location=4) in vec2 uv0;
layout(location=5) in vec2 uv1;
layout(location=0) out vec3 worldPosition;
layout(location=1) out vec3 N;
layout(location=2) out vec4 T;
layout(location=3) out vec4 C;
layout(location=4) out vec2 UV0;
layout(location=5) out vec2 UV1;
layout(location=6) flat out float receiveShadows;
invariant gl_Position;
void main() {
    InstanceData i=instances[drawIndices[gl_InstanceIndex]];
    vec4 p=i.world*vec4(position,1);worldPosition=p.xyz;
    N=normalize(mat3(i.normal0.xyz,i.normal1.xyz,i.normal2.xyz)*normal);
    vec3 t=mat3(i.world)*tangent.xyz;T=vec4(normalize(t-N*dot(N,t)),tangent.w*i.normal0.w);
    C=vertexColor*i.color;UV0=uv0;UV1=uv1;receiveShadows=i.normal1.w;
    gl_Position=(pass.mode.x==1||pass.mode.x==2?pass.lightVP:frame.vp)*p;
}
