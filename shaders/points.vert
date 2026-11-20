#version 450
// 点云顶点着色器：每个点是一个正对相机的圆盘。位置已在快照准备时变换到世界空间，
// 半径按透视投影换算成像素，因此同一份数据在不同分辨率与距离下保持物理尺寸。
#include "frame.glsl"
struct Point {
    vec4 positionRadius;
    vec4 colorIntensity;
    uvec4 label;
};
layout(std430,set=1,binding=0) readonly buffer Points { Point data[]; } points;
layout(location=0) out vec2 localCoord;
layout(location=1) flat out vec4 colorIntensity;
layout(location=2) flat out uint pointLabel;
layout(location=3) out vec3 worldCenter;
const vec2 corners[6]=vec2[6](vec2(-1,-1),vec2(1,-1),vec2(1,1),vec2(-1,-1),vec2(1,1),vec2(-1,1));
void main() {
    Point p=points.data[gl_InstanceIndex];
    vec3 center=p.positionRadius.xyz;
    float radius=max(p.positionRadius.w,1e-5);
    vec4 clip=frame.vp*vec4(center,1);
    // 焦距取投影矩阵的 y 缩放。Vulkan 的裁剪空间 y 向下，投影在同一分量上带负号，
    // 因此半径换算必须取绝对值，否则像素半径会变成负数并退化成一个像素。
    float resY=1.0/frame.info.z;
    float pixels=radius*abs(frame.vp[1][1])*0.5*resY/max(clip.w,1e-5);
    pixels=max(pixels,0.5);
    vec2 corner=corners[gl_VertexIndex%6];
    localCoord=corner;
    colorIntensity=p.colorIntensity;
    pointLabel=p.label.x;
    worldCenter=center;
    gl_Position=clip;
    vec2 resolution=vec2(1.0/frame.info.y,resY);
    gl_Position.xy+=corner*vec2(2.0*pixels)/resolution*gl_Position.w;
    // 与网格一致：抖动只作用于颜色通道，几何真值 pass 使用固定像素中心。
    if(draw.stable==0) gl_Position.xy+=frame.jitter.xy*gl_Position.w;
}
