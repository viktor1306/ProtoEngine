#version 450
layout(location=0) in vec3 position;
layout(location=1) in vec3 faceTint;
layout(location=2) in mat4 world;
layout(location=6) in vec4 tint;
layout(push_constant) uniform Camera { mat4 viewProjection; } camera;
layout(location=0) out vec3 color;
void main() {
    gl_Position = camera.viewProjection * world * vec4(position, 1);
    color = faceTint * tint.rgb;
}
