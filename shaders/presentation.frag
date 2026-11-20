#version 450
#include "frame.glsl"
layout(location=0) in vec2 uv;
// 调试呈现：写自己的图像，不触碰 Beauty/HDR。数值定义沿用原调试视图：
// 1 世界法线映射到 0..1、2 米制深度的对数归一化、3 实例标签的哈希色、
// 4 级联分区（Beauty 按级联着色后使用同样的曝光与色调映射）。背景一律为零。
layout(location=0) out vec4 display;
layout(set=1,binding=0) uniform sampler2D normalTex;
layout(set=1,binding=1) uniform sampler2D metricTex;
layout(set=1,binding=2) uniform usampler2D idTex;
layout(set=1,binding=3) uniform sampler2D hdrTex;
vec3 encode(vec3 m) {
    return mix(12.92*m,1.055*pow(m,vec3(1./2.4))-.055,step(vec3(.0031308),m));
}
void main() {
    ivec2 pixel=ivec2(gl_FragCoord.xy);
    int channel=int(frame.settings.w+0.5);
    uint id=texelFetch(idTex,pixel,0).r;
    vec3 c=vec3(0);
    if(id!=0) {
        if(channel==1) c=normalize(texelFetch(normalTex,pixel,0).xyz)*.5+.5;
        else if(channel==2) {
            float depth=texelFetch(metricTex,pixel,0).r;
            c=vec3(log(1+depth)/log(1+frame.splits.w));
        }
        else if(channel==3) c=fract(float(id)*vec3(.618,.317,.753));
        else if(channel==4) {
            float depth=texelFetch(metricTex,pixel,0).r;
            int cascade=int(depth>frame.splits.x)+int(depth>frame.splits.y)+int(depth>frame.splits.z);
            vec3 tint=vec3[4](vec3(1,.45,.4),vec3(.5,1,.5),vec3(.4,.65,1),vec3(1,.8,.35))[cascade];
            vec3 x=texelFetch(hdrTex,pixel,0).rgb*tint*frame.settings.x;
            c=clamp((x*(2.51*x+.03))/(x*(2.43*x+.59)+.14),0,1);
        }
    }
    display=vec4(encode(clamp(c,0,1)),1);
}
