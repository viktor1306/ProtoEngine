#version 450
layout(location=0) in vec3 position;
layout(location=1) in vec3 normal;
layout(location=2) in vec4 tangent;
layout(location=3) in vec4 vertexColor;
layout(location=4) in vec2 uv0;
layout(location=5) in vec2 uv1;
layout(location=6) in mat4 world;
layout(location=10) in vec4 normal0;
layout(location=11) in vec4 normal1;
layout(location=12) in vec4 normal2;
layout(location=13) in vec4 color;
layout(set=0,binding=0) uniform View {mat4 vp;vec4 eye;} camera;
layout(location=0) out vec3 worldPosition;
layout(location=1) out vec3 N;
layout(location=2) out vec4 T;
layout(location=3) out vec4 C;
layout(location=4) out vec2 UV0;
layout(location=5) out vec2 UV1;
void main(){vec4 p=world*vec4(position,1);worldPosition=p.xyz;N=normalize(mat3(normal0.xyz,normal1.xyz,normal2.xyz)*normal);vec3 t=mat3(world)*tangent.xyz;T=vec4(normalize(t-N*dot(N,t)),tangent.w*normal0.w);C=vertexColor*color;UV0=uv0;UV1=uv1;gl_Position=camera.vp*p;}
