#version 450
layout(set=0,binding=0) uniform sampler2D hdr;
layout(push_constant) uniform Exposure {float exposure;} settings;
layout(location=0) out vec4 outColor;
void main() {
    // Positive light sums may overflow FP16 only far above display saturation.
    vec3 c=clamp(texelFetch(hdr,ivec2(gl_FragCoord.xy),0).rgb,0,65504)*settings.exposure;
    vec3 mapped=clamp((c*(2.51*c+.03))/(c*(2.43*c+.59)+.14),0,1);
    vec3 srgb=mix(mapped*12.92,1.055*pow(mapped,vec3(1.0/2.4))-.055,step(vec3(.0031308),mapped));
    outColor=vec4(srgb,1);
}
