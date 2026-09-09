// Shared std140/std430 contract with AssetLighting.inl.
layout(set=0,binding=0,std140) uniform LightingView {
    mat4 vp; vec4 eye; mat4 inverseVP; mat4 view;
    vec4 camera; uvec4 extent; vec4 settings; vec4 options;
} frame;
struct Light { vec4 positionRadius; vec4 colorIntensity; vec4 directionType; vec4 shadow; mat4 cascade[4]; vec4 splits; };
layout(set=0,binding=1,std430) readonly buffer Lights { Light lights[]; };
#ifdef PROTO_TILE_WRITE
layout(set=0,binding=2,std430) buffer Tiles { uint tileWords[]; };
#else
layout(set=0,binding=2,std430) readonly buffer Tiles { uint tileWords[]; };
#endif
struct InstanceData { mat4 world; vec4 normal0; vec4 normal1; vec4 normal2; vec4 color; };
layout(set=0,binding=3,std430) readonly buffer Instances { InstanceData instances[]; };
layout(set=0,binding=4,std430) readonly buffer DrawIndices { uint drawIndices[]; };
layout(set=0,binding=5) uniform sampler2DArray sunShadowLayers;
layout(set=0,binding=6) uniform samplerCubeArray sunShadowCubes;
layout(set=0,binding=7) uniform sampler2DArray pointShadowLayers;
layout(set=0,binding=8) uniform samplerCubeArray pointShadowCubes;
layout(set=0,binding=9) uniform sampler2D mainDepth;
#ifdef PROTO_TILE_WRITE
layout(set=0,binding=10,std430) buffer Counters { uint overflowTiles; uint populatedTiles; uint padding0; uint padding1; };
#else
layout(set=0,binding=10,std430) readonly buffer Counters { uint overflowTiles; uint populatedTiles; uint padding0; uint padding1; };
#endif
layout(push_constant) uniform Pass { mat4 lightVP; vec4 lightPositionRadius; uvec4 mode; } pass;
