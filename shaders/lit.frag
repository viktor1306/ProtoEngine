#version 450
#extension GL_GOOGLE_include_directive : require
#include "lighting.glsl"
#include "material.glsl"
layout(location=0) in vec3 worldPosition;
layout(location=1) in vec3 N;
layout(location=2) in vec4 T;
layout(location=3) in vec4 C;
layout(location=4) in vec2 UV0;
layout(location=5) in vec2 UV1;
layout(location=6) flat in float receiveShadows;
layout(location=0) out vec4 outColor;
const float PI=3.141592653589793;

float cascadeSample(Light light,int cascade,vec3 position,vec3 planeNormal) {
    vec4 projected=light.cascade[cascade]*vec4(position,1);
    vec3 p=projected.xyz/projected.w;vec2 uv=p.xy*.5+.5;
    if(any(lessThan(uv,vec2(0)))||any(greaterThan(uv,vec2(1)))||p.z<0||p.z>1)return 1;
    float layer=light.shadow.x*6+float(cascade);
    mat3 rows=transpose(mat3(light.cascade[cascade]));
    // Receiver plane in shadow NDC. Orthographic rows are orthogonal, so
    // inverse-transpose needs only these dot products. Compare at the actual
    // nearest texel center, including PCF offsets, to avoid grazing-angle acne.
    vec3 plane=vec3(dot(planeNormal,rows[0])/dot(rows[0],rows[0]),
                    dot(planeNormal,rows[1])/dot(rows[1],rows[1]),
                    dot(planeNormal,rows[2])/dot(rows[2],rows[2]));
    vec2 slope=abs(plane.z)>1e-6 ? -2*plane.xy/plane.z : vec2(0);
    vec2 size=vec2(textureSize(sunShadowLayers,0).xy),texel=1/size;
    float visibility=0;int taps=max(1,int(sqrt(max(light.shadow.w,1.0))));
    for(int y=0;y<taps;++y)for(int x=0;x<taps;++x) {
        vec2 tap=uv+(vec2(x,y)-float(taps-1)*.5)*texel;
        tap=clamp((floor(tap*size)+.5)*texel,texel*.5,1-texel*.5);
        float reference=p.z+dot(slope,tap-uv)-frame.settings.y;
        visibility+=reference<=textureLod(sunShadowLayers,vec3(tap,layer),0.0).r?1.0/float(taps*taps):0;
    }
    return visibility;
}
float visibility(Light light,vec3 normal) {
    if(light.shadow.x<0 || receiveShadows<.5)return 1;
    vec3 p=worldPosition+normal*frame.settings.z;
    if(light.directionType.w<.5) {
        float distance=-(frame.view*vec4(worldPosition,1)).z;
        int count=int(light.shadow.y),cascade=0;
        while(cascade<count && distance>light.splits[cascade])++cascade;
        if(cascade>=count)return 1;
        float value=cascadeSample(light,cascade,p,normal);
        if(cascade+1<count) {
            float begin=cascade==0?frame.camera.z:light.splits[cascade-1],end=light.splits[cascade];
            float blend=smoothstep(end-(end-begin)*.12,end,distance);
            if(blend>0)value=mix(value,cascadeSample(light,cascade+1,p,normal),blend);
        }
        return value;
    }
    vec3 direction=p-light.positionRadius.xyz;
    float distance=length(direction),reference=distance/light.positionRadius.w-frame.settings.y;
    int taps=max(1,int(sqrt(max(light.shadow.w,1.0))));
    if(taps==1)return reference<=textureLod(pointShadowCubes,vec4(direction,light.shadow.x),0.0).r?1:0;
    vec3 axis=normalize(direction),helper=abs(axis.y)>.95?vec3(0,0,1):vec3(0,1,0);
    vec3 right=normalize(cross(axis,helper)),up=cross(right,axis);
    float texel=2*distance/float(textureSize(pointShadowCubes,0).x),value=0;
    for(int y=0;y<taps;++y)for(int x=0;x<taps;++x) {
        vec2 offset=vec2(x,y)-float(taps-1)*.5;
        vec3 sampleDirection=direction+(right*offset.x+up*offset.y)*texel;
        value+=reference<=textureLod(pointShadowCubes,vec4(sampleDirection,light.shadow.x),0.0).r?1.0/float(taps*taps):0;
    }
    return value;
}
vec3 illuminate(uint index,vec3 n,vec3 v,vec3 base,float metallic,float rough,vec3 planeNormal) {
    Light light=lights[index];vec3 l;float energy=light.colorIntensity.w;
    if(light.directionType.w<.5)l=-light.directionType.xyz;
    else {
        vec3 delta=light.positionRadius.xyz-worldPosition;float distance=length(delta);
        if(distance>=light.positionRadius.w)return vec3(0);
        l=delta/max(distance,.0001);
        float edge=max(1-pow(distance/light.positionRadius.w,4),0);
        energy*=edge*edge/max(distance*distance,.01);
    }
    float nl=max(dot(n,l),0);if(nl<=0)return vec3(0);
    vec3 h=(v+l)/max(length(v+l),.00001);float nv=max(dot(n,v),.0001),nh=max(dot(n,h),0),vh=max(dot(v,h),0);
    float a=rough*rough,a2=a*a,den=nh*nh*(a2-1)+1,D=a2/(PI*den*den);
    float gv=2*nv/(nv+sqrt(a2+(1-a2)*nv*nv)),gl=2*nl/max(nl+sqrt(a2+(1-a2)*nl*nl),.0001);
    vec3 f0=mix(vec3(.04),base,metallic),F=f0+(1-f0)*pow(1-vh,5);
    vec3 specular=D*gv*gl*F/max(4*nv*nl,.0001),diffuse=(1-F)*(1-metallic)*base/PI;
    return (diffuse+specular)*nl*energy*light.colorIntensity.rgb*visibility(light,planeNormal);
}
void main() {
    // Derivatives must be evaluated before any MASK invocation terminates.
    vec3 planeNormal=normalize(cross(dFdx(worldPosition),dFdy(worldPosition)));
    vec4 base=m.baseColor*C*texture(baseMap,materialUV(0,UV0,UV1));
    vec4 mr=vec4(1);vec3 map=vec3(0,0,1),emission=vec3(0);float ao=1;
    // Material samples also precede termination; every branch here is uniform
    // for a draw, unlike the later per-pixel light and cascade choices.
    if(pass.mode.x==3) {
        ao=mix(1,texture(occlusionMap,materialUV(3,UV0,UV1)).r,m.factors.w);
        emission=m.emissive.rgb*texture(emissiveMap,materialUV(4,UV0,UV1)).rgb;
    } else if(m.flags.z<.5) {
        mr=texture(metallicMap,materialUV(1,UV0,UV1));
        map=texture(normalMap,materialUV(2,UV0,UV1)).xyz*2-1;
    }
    if(m.flags.x>.5&&base.a<m.flags.y)discard;
    if(pass.mode.x==3) {
        vec3 radiance=m.flags.z>.5?base.rgb:base.rgb*frame.settings.x*ao+emission;
        outColor=vec4(min(radiance,vec3(65504)),1);return;
    }
    if(m.flags.z>.5){outColor=vec4(0);return;}
    vec3 n=normalize(N),t=normalize(T.xyz-n*dot(n,T.xyz)),b=cross(n,t)*T.w;
    map.xy*=m.factors.z;n=normalize(mat3(t,b,n)*map);
    if(!gl_FrontFacing)n=-n;
    if(dot(planeNormal,n)<0)planeNormal=-planeNormal;
    float metallic=clamp(m.factors.x*mr.b,0,1),rough=clamp(m.factors.y*mr.g,.045,1);
    vec3 v=normalize(frame.eye.xyz-worldPosition),radiance=vec3(0);
    uvec2 tile=uvec2(gl_FragCoord.xy)/16;uint at=(tile.y*frame.extent.z+tile.x)*frame.extent.w;
    uint count=tileWords[at];
    if(frame.options.y>.5||count>64) {
        for(uint i=0;i<pass.mode.z;++i)radiance+=illuminate(pass.mode.y+i,n,v,base.rgb,metallic,rough,planeNormal);
    } else {
        for(uint i=0;i<count;++i)radiance+=illuminate(tileWords[at+2+i],n,v,base.rgb,metallic,rough,planeNormal);
    }
    outColor=vec4(min(radiance,vec3(65504)),0);
}
