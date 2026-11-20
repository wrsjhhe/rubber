#version 450
#include "frame.glsl"
layout(location=0) in vec3 position;
layout(location=1) in vec3 normal;
layout(location=2) in vec2 uv;
layout(location=0) out vec3 worldNormal;
layout(location=1) out vec2 texcoord;
layout(location=2) out vec4 oldClip;
layout(location=3) out vec3 worldPosition;
void main() {
    Instance m = instances.data[draw.object];
    vec4 p = m.model * vec4(position,1);
    worldPosition = p.xyz;
    worldNormal = transpose(inverse(mat3(m.model))) * normal;
    texcoord = uv * m.material.yz;
    oldClip = frame.oldVP * histories.data[draw.object].oldModel * vec4(position,1);
    gl_Position = frame.vp * p;
    if (draw.stable == 0) gl_Position.xy += frame.jitter.xy * gl_Position.w;
}
