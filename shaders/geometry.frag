#version 450
#include "frame.glsl"
layout(location=0) in vec3 worldNormal;
layout(location=1) in vec2 uv;
layout(location=2) in vec4 oldClip;
layout(location=3) in vec3 worldPosition;
layout(set=1,binding=0) uniform sampler2D colorMap;
layout(set=1,binding=1) uniform sampler2D normalMap;
layout(set=1,binding=2) uniform sampler2D mrMap;
layout(location=0) out vec4 albedoMetal;
layout(location=1) out vec4 normalRough;
layout(location=2) out vec4 motion;
layout(location=3) out float depthMetres;
layout(location=4) out uint instanceID;
void main() {
    Instance m = instances.data[draw.object];
    vec4 mr = texture(mrMap,uv);
    albedoMetal = vec4(texture(colorMap,uv).rgb*m.colorRough.rgb,m.material.x*mr.b);
    vec3 n = normalize(worldNormal);
    vec3 dx = dFdx(worldPosition), dy = dFdy(worldPosition);
    vec2 s = dFdx(uv), t = dFdy(uv);
    float det = s.x*t.y-s.y*t.x;
    if(m.material.w > 0.5 && abs(det)>1e-10) {
        vec3 tangent = (dx*t.y-dy*s.y)/det;
        tangent = normalize(tangent-n*dot(n,tangent));
        vec3 b = cross(n,tangent)*sign(dot(cross(n,tangent),(dy*s.x-dx*t.x)/det));
        n = normalize(mat3(tangent,b,n)*(texture(normalMap,uv).xyz*2-1));
    }
    normalRough = vec4(n,clamp(m.colorRough.w*mr.g,0.045,1));
    depthMetres = 1/gl_FragCoord.w;
    vec2 currentUV = gl_FragCoord.xy * frame.info.yz;
    if (draw.stable == 0) currentUV -= frame.jitter.xy * 0.5;
    motion = vec4(oldClip.xy/max(oldClip.w,1e-5)*0.5+0.5-currentUV,oldClip.w,m.label.y);
    instanceID = m.label.x;
}
