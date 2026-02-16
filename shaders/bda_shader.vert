#version 460
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_scalar_block_layout : require
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require

// Definition of the vertex structure in the buffer
struct Vertex {
    vec3 position;
    float pad1; // Alignment padding
    vec3 normal;
    float pad2;
    vec2 uv;
    vec2 pad3;
};

// Validating the block layout (scalar is standard for BDA usually)
layout(buffer_reference, scalar) buffer VertexBuffer {
    Vertex vertices[];
};

// Push Constants now carry the POINTER (address) to the data
layout(push_constant) uniform PushConstants {
    mat4 model;
    mat4 view;
    mat4 proj;
    uint64_t vertexBufferPtr; // Device Address of the vertex buffer
} pc;

void main() {
    // Cast the address to a reference type
    VertexBuffer vBuffer = VertexBuffer(pc.vertexBufferPtr);
    
    // Fetch vertex data directly using gl_VertexIndex
    Vertex v = vBuffer.vertices[gl_VertexIndex];

    gl_Position = pc.proj * pc.view * pc.model * vec4(v.position, 1.0);
    
    // Output for fragment shader...
    // outColor = v.normal; etc...
}
