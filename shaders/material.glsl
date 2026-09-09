layout(set=1,binding=0,std140) uniform Material { vec4 baseColor; vec4 emissive; vec4 factors; vec4 flags; vec4 uvScaleOffset[5]; vec4 uvRotationSet[5]; } m;
layout(set=1,binding=1) uniform sampler2D baseMap;
layout(set=1,binding=2) uniform sampler2D metallicMap;
layout(set=1,binding=3) uniform sampler2D normalMap;
layout(set=1,binding=4) uniform sampler2D occlusionMap;
layout(set=1,binding=5) uniform sampler2D emissiveMap;
vec2 materialUV(int index,vec2 uv0,vec2 uv1) {
    vec2 v=(m.uvRotationSet[index].y>.5?uv1:uv0)*m.uvScaleOffset[index].xy;
    float a=m.uvRotationSet[index].x;
    return mat2(cos(a),sin(a),-sin(a),cos(a))*v+m.uvScaleOffset[index].zw;
}

