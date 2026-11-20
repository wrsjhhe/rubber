#version 450
#include "frame.glsl"
layout(location=0) in vec2 uv;
layout(location=0) out vec4 history;
layout(location=1) out vec4 display;
layout(set=1,binding=0) uniform sampler2D hdr;
layout(set=1,binding=1) uniform sampler2D velocity;
layout(set=1,binding=2) uniform sampler2D metric;
layout(set=1,binding=3) uniform sampler2D oldHistory;
ivec2 inBounds(ivec2 p) {
    return clamp(p,ivec2(0),textureSize(hdr,0)-1);
}

vec3 movingResolve(ivec2 pixel, vec4 motion, float depth) {
    // 在轮廓附近用最近的前景速度带动覆盖边缘，包括中心刚好落在天空的像素。
    float nearest=depth>0 ? depth : 1e30;
    for(int y=-1;y<=1;y++) for(int x=-1;x<=1;x++) {
        ivec2 q=inBounds(pixel+ivec2(x,y));
        float z=texelFetch(metric,q,0).r;
        if(z>0 && z<nearest) {
            nearest=z;
            depth=z;
            motion=texelFetch(velocity,q,0);
        }
    }
    // 输出始终位于固定像素中心。旋转相机时，天空使用无穷远方向重投影。
    vec2 previous=uv+motion.xy;
    float expectedZ=motion.z;
    bool valid=motion.w>.5 && expectedZ>0;
    if(depth==0) {
        vec4 world=frame.invVP*vec4(uv*2-1,1,1);
        vec4 old=frame.oldVP*vec4(world.xyz/world.w-frame.eye.xyz,0);
        previous=old.xy/max(old.w,1e-6)*.5+.5;
        expectedZ=0;
        valid=old.w>0;
    }

    // 当前抖动样本重建到输出栅格，新显露的边缘也有空间滤波。
    vec2 offset=frame.jitter.xy*vec2(textureSize(hdr,0))*.5;
    ivec2 stepDir=ivec2(sign(offset));
    vec2 t=abs(offset);
    vec3 c=mix(mix(texelFetch(hdr,pixel,0).rgb,texelFetch(hdr,inBounds(pixel+ivec2(stepDir.x,0)),0).rgb,t.x),
               mix(texelFetch(hdr,inBounds(pixel+ivec2(0,stepDir.y)),0).rgb,
                   texelFetch(hdr,inBounds(pixel+stepDir),0).rgb,t.x),t.y);
    if(!valid || any(lessThan(previous,vec2(0))) || any(greaterThanEqual(previous,vec2(1)))) return c;

    // 分别检验双线性采样的四个深度，避免前后景交界处出现虚假的中间深度。
    vec2 p=previous*vec2(textureSize(hdr,0))-.5;
    ivec2 base=ivec2(floor(p));
    vec2 f=fract(p);
    float weight=0;
    for(int y=0;y<2;y++) for(int x=0;x<2;x++) {
        vec4 h=texelFetch(oldHistory,inBounds(base+ivec2(x,y)),0);
        bool compatible=depth==0 ? h.a==0 : h.a>0 && abs(h.a-expectedZ)<max(.035,.015*expectedZ);
        if(compatible) {
            float w=(x==0?1-f.x:f.x)*(y==0?1-f.y:f.y);
            weight+=w;
        }
    }
    if(weight<1e-4) return c;
    vec3 lo=c,hi=c;
    for(int y=-1;y<=1;y++) for(int x=-1;x<=1;x++) {
        vec3 neighbor=texelFetch(hdr,inBounds(pixel+ivec2(x,y)),0).rgb;
        lo=min(lo,neighbor);
        hi=max(hi,neighbor);
    }
    // 合法历史的颜色已经是像素覆盖率积分，重建时保留前后景之间的插值。
    vec2 f2=f*f, f3=f2*f;
    vec2 w0=-.5*f+f2-.5*f3, w1=1.-2.5*f2+1.5*f3;
    vec2 w2=.5*f+2.*f2-1.5*f3, w3=-.5*f2+.5*f3;
    vec2 weights[4]=vec2[4](w0,w1,w2,w3);
    vec3 reconstructed=vec3(0);
    for(int y=0;y<4;y++) for(int x=0;x<4;x++)
        reconstructed+=texelFetch(oldHistory,inBounds(base+ivec2(x-1,y-1)),0).rgb*weights[x].x*weights[y].y;
    return mix(texelFetch(hdr,pixel,0).rgb,clamp(reconstructed,lo,hi),.86);
}
void main() {
    ivec2 pixel=ivec2(gl_FragCoord.xy);
    vec3 c=texelFetch(hdr,pixel,0).rgb;
    vec4 motion=texelFetch(velocity,pixel,0);
    float depth=texelFetch(metric,pixel,0).r;
    if(frame.settings.y>0) {
        if(frame.info.w>=0)
            c=mix(texelFetch(oldHistory,pixel,0).rgb,c,frame.info.w);
        else
            c=movingResolve(pixel,motion,depth);
    }
    history=vec4(c,depth);
    vec3 x=c*frame.settings.x;
    vec3 mapped=frame.settings.w==0 || frame.settings.w==4 ? clamp((x*(2.51*x+.03))/(x*(2.43*x+.59)+.14),0,1):clamp(c,0,1);
    display=vec4(mix(12.92*mapped,1.055*pow(mapped,vec3(1./2.4))-.055,step(.0031308,mapped)),1);
}
