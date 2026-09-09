#version 450
layout(location = 0) out vec3 color;
// Engine convention: X right, Y up, Vulkan depth [0, 1]. Negative viewport
// height maps Y up to the top of the displayed image. Front faces are CCW.
const vec2 triangle[3] = vec2[](vec2(-0.7, -0.65), vec2(0.7, -0.65), vec2(0.0, 0.7));
const vec3 colors[3] = vec3[](vec3(1, 0, 0), vec3(0, 0, 1), vec3(0, 1, 0));
void main() {
    int i = gl_VertexIndex;
    if (i < 3) {
        gl_Position = vec4(triangle[i], 0.25, 1);
        color = colors[i];
    } else if (i < 6) {
        // Drawn afterwards: must fail depth against the foreground triangle.
        gl_Position = vec4(triangle[i - 3], 0.75, 1);
        color = vec3(1, 1, 0);
    } else {
        // Clockwise triangle in the upper-right corner: must be culled.
        const vec2 back[3] = vec2[](vec2(0.60, 0.60), vec2(0.76, 0.90), vec2(0.92, 0.60));
        gl_Position = vec4(back[i - 6], 0.1, 1);
        color = vec3(1, 0, 1);
    }
}
