#version 450
#include "frame.glsl"
layout(location=0) in vec2 uv;
// 颜色变换：曝光、色调映射与 sRGB 编码。只读取时序解析结果，不修改历史，
// 调试通道的显示裁剪也集中在这里。
layout(location=0) out vec4 display;
layout(set=1,binding=0) uniform sampler2D resolved;
void main() {
    vec3 c=texelFetch(resolved,ivec2(gl_FragCoord.xy),0).rgb;
    vec3 x=c*frame.settings.x;
    vec3 mapped=frame.settings.w==0 || frame.settings.w==4 ? clamp((x*(2.51*x+.03))/(x*(2.43*x+.59)+.14),0,1):clamp(c,0,1);
    display=vec4(mix(12.92*mapped,1.055*pow(mapped,vec3(1./2.4))-.055,step(.0031308,mapped)),1);
}
