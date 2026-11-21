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
    // 焦距取纯投影矩阵的两轴尺度（像素），不用 vp 的元素：vp 含视图旋转，俯仰/翻滚时会改变
    // 该元素，圆盘半径随之漂移，90 度翻滚还会退化到最小像素。x/y 分开换算，fx!=fy 时为椭圆。
    float depth=max(clip.w,1e-5);
    vec2 pixels=max(vec2(radius*max(frame.lens.x,1e-6),radius*max(frame.lens.y,1e-6))/depth,vec2(0.5));
    vec2 corner=corners[gl_VertexIndex%6];
    localCoord=corner;
    colorIntensity=p.colorIntensity;
    pointLabel=p.label.x;
    worldCenter=center;
    gl_Position=clip;
    // 像素偏移换回裁剪空间：ndc = 2*pixels/resolution，clip = ndc*w。
    gl_Position.xy+=corner*2.0*pixels*vec2(frame.info.y,frame.info.z)*gl_Position.w;
    // 与网格一致：抖动只作用于颜色通道，几何真值 pass 使用固定像素中心。
    if(draw.stable==0) gl_Position.xy+=frame.jitter.xy*gl_Position.w;
}
