#version 450
layout(set=0,binding=0) uniform View {mat4 vp;vec4 eye;} camera;
layout(set=1,binding=0) uniform Material {vec4 baseColor;vec4 emissive;vec4 factors;vec4 flags;vec4 uvScaleOffset[5];vec4 uvRotationSet[5];} m;
layout(set=1,binding=1) uniform sampler2D baseMap;
layout(set=1,binding=2) uniform sampler2D metallicMap;
layout(set=1,binding=3) uniform sampler2D normalMap;
layout(set=1,binding=4) uniform sampler2D occlusionMap;
layout(set=1,binding=5) uniform sampler2D emissiveMap;
layout(location=0) in vec3 worldPosition;
layout(location=1) in vec3 N;
layout(location=2) in vec4 T;
layout(location=3) in vec4 C;
layout(location=4) in vec2 UV0;
layout(location=5) in vec2 UV1;
layout(location=0) out vec4 outColor;
const float PI=3.14159265359;
vec2 uv(int i){vec2 v=(m.uvRotationSet[i].y>.5?UV1:UV0)*m.uvScaleOffset[i].xy;float a=m.uvRotationSet[i].x;return mat2(cos(a),sin(a),-sin(a),cos(a))*v+m.uvScaleOffset[i].zw;}
vec3 srgb(vec3 c){return mix(12.92*c,1.055*pow(max(c,vec3(0)),vec3(1.0/2.4))-.055,step(vec3(.0031308),c));}
void main(){vec4 base=m.baseColor*C*texture(baseMap,uv(0));if(m.flags.x>.5&&base.a<m.flags.y)discard;if(m.flags.z>.5){outColor=vec4(srgb(base.rgb),1);return;}
    vec3 n=normalize(N),t=normalize(T.xyz-n*dot(n,T.xyz)),b=cross(n,t)*T.w;vec3 map=texture(normalMap,uv(2)).xyz*2-1;map.xy*=m.factors.z;n=normalize(mat3(t,b,n)*map);if(!gl_FrontFacing)n=-n;
    vec4 mr=texture(metallicMap,uv(1));float metallic=clamp(m.factors.x*mr.b,0,1),rough=clamp(m.factors.y*mr.g,.045,1);vec3 v=normalize(camera.eye.xyz-worldPosition);
    // Fixed material-preview illumination. Scene lights and shadows belong to M3.
    vec3 l=normalize(vec3(.4,.7,1)),h=normalize(v+l);float nv=max(dot(n,v),.0001),nl=max(dot(n,l),0),nh=max(dot(n,h),0),vh=max(dot(v,h),0);
    float a=rough*rough,a2=a*a,den=nh*nh*(a2-1)+1,D=a2/(PI*den*den);float gv=2*nv/(nv+sqrt(a2+(1-a2)*nv*nv)),gl=2*nl/max(nl+sqrt(a2+(1-a2)*nl*nl),.0001);
    vec3 f0=mix(vec3(.04),base.rgb,metallic),F=f0+(1-f0)*pow(1-vh,5);vec3 spec=D*gv*gl*F/max(4*nv*nl,.0001),diff=(1-F)*(1-metallic)*base.rgb/PI;
    float ao=mix(1,texture(occlusionMap,uv(3)).r,m.factors.w);vec3 radiance=(diff+spec)*nl*3.0+base.rgb*.035*ao+m.emissive.rgb*texture(emissiveMap,uv(4)).rgb;
    vec3 mapped=clamp((radiance*(2.51*radiance+.03))/(radiance*(2.43*radiance+.59)+.14),0,1);outColor=vec4(srgb(mapped),1);
}
