#version 450
// 点云片元着色器：按覆盖度裁出圆盘，把强度当作辐亮度叠加到已光照的 HDR 上。
// 同一像素的深度、ID、米制深度与速度由点云写入，保证几何真值与颜色属于同一物体。
// POINTS_HDR=0 时用于几何真值 pass：该 pass 没有 HDR 附件，不产生辐射输出。
#include "frame.glsl"
#ifndef POINTS_HDR
#define POINTS_HDR 1
#endif
layout(location=0) in vec2 localCoord;
layout(location=1) flat in vec4 colorIntensity;
layout(location=2) flat in uint pointLabel;
layout(location=3) in vec3 worldCenter;
layout(location=0) out vec4 albedoMetal;
layout(location=1) out vec4 normalRough;
layout(location=2) out vec4 motion;
layout(location=3) out float depthMetres;
layout(location=4) out uint instanceID;
#if POINTS_HDR
layout(location=5) out vec4 radiance;
#endif
void main() {
    float d=length(localCoord);
    // 边缘按像素宽度淡出；半径不足一个像素时仍保留可见的最小覆盖。
    float coverage=clamp((1.0-d)/max(fwidth(d),1e-4),0.0,1.0);
    if(coverage<=0.002) discard;
    vec3 color=colorIntensity.rgb;
    // 点云没有金属度与粗糙度纹理：底色即颜色，法线朝向相机，视为漫反射圆盘。
    albedoMetal=vec4(color,0);
    normalRough=vec4(normalize(frame.eye.xyz-worldCenter),1);
    // 点云没有跨帧历史：速度标记为"无匹配"，时序解析不会复用旧像素，避免拖尾。
    motion=vec4(0);
    depthMetres=1.0/gl_FragCoord.w;
    instanceID=pointLabel;
#if POINTS_HDR
    radiance=vec4(color*colorIntensity.w,coverage);
#endif
}
