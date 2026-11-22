#version 460
#if RAY_QUERY
#extension GL_EXT_ray_query : require
#endif
#include "frame.glsl"
layout(location=0) in vec2 uv;
layout(location=0) out vec4 outputColor;
layout(set=1,binding=0) uniform sampler2D albedoTex;
layout(set=1,binding=1) uniform sampler2D normalTex;
layout(set=1,binding=3) uniform sampler2D metricTex;
layout(set=1,binding=4) uniform usampler2D idTex;
layout(set=1,binding=5) uniform sampler2D zTex;
layout(set=1,binding=6) uniform sampler2D shadowTex;
#if RAY_QUERY
layout(set=1,binding=7) uniform accelerationStructureEXT shadowScene;
#endif
const float PI = 3.14159265;
vec3 sky(vec3 d) {
    vec3 c = mix(vec3(.65,.72,.79),vec3(.095,.22,.39),pow(max(0,d.y),.55));
    float sun = max(0,dot(d,-frame.sunDirection.xyz));
    return c + vec3(1,.83,.60)*pow(sun,130)*1.6;
}
vec3 brdf(vec3 n,vec3 v,vec3 l,vec3 base,float metal,float rough) {
    vec3 h = normalize(v+l);
    float nl=max(dot(n,l),0), nv=max(dot(n,v),.001), nh=max(dot(n,h),0);
    float a2=pow(rough,4), den=nh*nh*(a2-1)+1;
    float D=a2/(PI*den*den);
    float vis=.5/max(nl*sqrt(nv*nv*(1-a2)+a2)+nv*sqrt(nl*nl*(1-a2)+a2),1e-5);
    vec3 f0=mix(vec3(.04),base,metal);
    vec3 f=f0+(1-f0)*pow(1-max(dot(h,v),0),5);
    return ((1-f)*(1-metal)*base/PI+D*vis*f)*nl;
}
#if RAY_QUERY
float rayContactVisibility(vec3 origin, vec3 direction, float distance, uint receiverId) {
    if(distance<=.004) return 1.;
    rayQueryEXT ray;
    // 保留最近遮挡距离，使短射线在距离上限处连续淡出。
    rayQueryInitializeEXT(ray,shadowScene,gl_RayFlagsNoOpaqueEXT,0xff,origin,.004,direction,distance);
    while(rayQueryProceedEXT(ray)) {
        // 光滑法线仍受光时，射线可能从本三角分面的背面穿出。
        // 跳过同一实例的背面候选，保留凹部的正面遮挡及其他物体的双面遮挡。
        if(!rayQueryGetIntersectionFrontFaceEXT(ray,false)) {
            int index=rayQueryGetIntersectionInstanceCustomIndexEXT(ray,false);
            if(instances.data[index].label.x==receiverId) continue;
        }
        rayQueryConfirmIntersectionEXT(ray);
    }
    if(rayQueryGetIntersectionTypeEXT(ray,true)==gl_RayQueryCommittedIntersectionNoneEXT) return 1.;
    float t=rayQueryGetIntersectionTEXT(ray,true)/distance;
    return 1.-.85*(1.-smoothstep(.65,1.,t));
}
#endif
const vec2 shadowDisk[16] = vec2[](
    vec2(-.942,-.399), vec2(.946,-.769), vec2(-.094,-.929), vec2(.345,.294),
    vec2(-.916,.458), vec2(-.815,-.879), vec2(-.382,.277), vec2(.974,.756),
    vec2(.443,-.975), vec2(.537,-.474), vec2(-.265,-.418), vec2(.792,.191),
    vec2(-.242,.998), vec2(-.814,.914), vec2(.200,.786), vec2(.144,-.141)
);
float shadowDepth(vec2 texel, ivec2 origin, ivec2 size) {
    return texelFetch(shadowTex, origin+clamp(ivec2(floor(texel+.5)),ivec2(0),size-1),0).r;
}
float visibility(vec3 pos, int c, vec3 n) {
    vec4 p=frame.lightVP[c]*vec4(pos,1);
    vec3 q=p.xyz/p.w;
    vec2 local=q.xy*.5+.5;
    if(any(lessThan(local,vec2(0))) || any(greaterThan(local,vec2(1))) || q.z<0 || q.z>1) return 1;
    ivec2 tileSize=textureSize(shadowTex,0)/2;
    ivec2 tileOrigin=ivec2(c%2,c/2)*tileSize;
    vec2 texelPos=local*vec2(tileSize)-.5;
    ivec2 base=ivec2(floor(texelPos));
    vec2 f=fract(texelPos);
    float bias=.00022+.0007*(1-max(0,dot(n,-frame.sunDirection.xyz)));
    if(frame.shadowOptions.x>0) {
        // 正交光源投影将归一化深度差换算成米，再按角半径估算半影。
        mat4 light=frame.lightVP[c];
        float depthScale=length(vec3(light[0].z,light[1].z,light[2].z));
        float texelsPerMetre=.5*float(tileSize.x)*length(vec3(light[0].x,light[1].x,light[2].x));
        float texelsPerDepth=tan(frame.shadowOptions.x)*texelsPerMetre/depthScale;
        float searchRadius=clamp(q.z*texelsPerDepth,2.,12.);
        float blockerDepth=0;
        int blockers=0;
        for(int i=0;i<16;i++) {
            float d=shadowDepth(texelPos+shadowDisk[i]*searchRadius,tileOrigin,tileSize);
            if(d<q.z-bias) {
                blockerDepth+=d;
                blockers++;
            }
        }
        if(blockers>0) {
            float radius=clamp((q.z-blockerDepth/float(blockers))*texelsPerDepth,0.,12.);
            if(radius>1.5) {
                float lit=0;
                for(int i=0;i<16;i++) {
                    float d=shadowDepth(texelPos+shadowDisk[i]*radius,tileOrigin,tileSize);
                    lit+=q.z-bias<=d ? 1. : 0.;
                }
                return lit/16.;
            }
        }
    }
    float sum=0;
    // 合并九次双线性深度比较的权值，阴影轮廓可在相邻 texel 间连续变化。
    for(int y=-1;y<=2;y++) for(int x=-1;x<=2;x++) {
        ivec2 coord=tileOrigin+clamp(base+ivec2(x,y),ivec2(0),tileSize-1);
        float wx=x==-1 ? 1-f.x : (x==2 ? f.x : 1);
        float wy=y==-1 ? 1-f.y : (y==2 ? f.y : 1);
        sum += q.z-bias<=texelFetch(shadowTex,coord,0).r ? wx*wy : 0;
    }
    return sum/9;
}
#if !RAY_QUERY
float contactVisibility(vec3 world, vec3 n, vec3 toSun) {
    float rayLength=frame.shadowOptions.y;
    ivec2 size=textureSize(metricTex,0);
    ivec2 receiver=ivec2(gl_FragCoord.xy);
    vec3 origin=world+n*.008;
    const int steps=16;
    for(int i=0;i<steps;i++) {
        float t=(float(i)+.5)/float(steps);
        vec4 clip=frame.vp*vec4(origin+toSun*(rayLength*t),1);
        if(clip.w<=0 || clip.z<0 || clip.z>clip.w) break;
        // 深度来自带抖动的 G-buffer，步进投影使用同一份抖动。
        vec2 sampleUV=(clip.xy/clip.w+frame.jitter.xy)*.5+.5;
        if(any(lessThan(sampleUV,vec2(0))) || any(greaterThanEqual(sampleUV,vec2(1)))) break;
        ivec2 pixel=ivec2(sampleUV*vec2(size));
        if(all(equal(pixel,receiver))) continue;
        float sceneZ=texelFetch(metricTex,pixel,0).r;
        float behind=clip.w-sceneZ;
        if(sceneZ>0 && behind>.004 && behind<.035)
            return 1.-.85*(1.-smoothstep(.65,1.,t));
    }
    return 1.;
}
#endif
void main() {
    float z=texture(zTex,uv).r;
    vec4 p=frame.invVP*vec4(uv*2-1-frame.jitter.xy,z,1);
    vec3 world=p.xyz/p.w;
    uint id=texture(idTex,uv).r;
    if(id==0) {
        outputColor=vec4(sky(normalize(world-frame.eye.xyz)),1);
        return;
    }
    vec4 am=texture(albedoTex,uv), nr=texture(normalTex,uv);
    vec3 n=normalize(nr.xyz), v=normalize(frame.eye.xyz-world);
    float depth=texture(metricTex,uv).r;
    int cascade=int(depth>frame.splits.x)+int(depth>frame.splits.y)+int(depth>frame.splits.z);
    float shadow=1;
    if(frame.settings.z>0 && dot(n,-frame.sunDirection.xyz)>0) {
        shadow=visibility(world,cascade,n);
        if(shadow>0 && frame.shadowOptions.y>0) {
#if RAY_QUERY
            // 用米制深度定位射线原点，避免远处硬件深度量化将起点推入表面。
            vec4 nearPoint=frame.invVP*vec4(uv*2-1-frame.jitter.xy,0,1);
            vec3 rayOrigin=frame.eye.xyz+(nearPoint.xyz-frame.eye.xyz*nearPoint.w)*depth;
            shadow*=rayContactVisibility(rayOrigin,-frame.sunDirection.xyz,frame.shadowOptions.y,id);
#else
            shadow*=contactVisibility(world,n,-frame.sunDirection.xyz);
#endif
        }
    }
    vec3 c=brdf(n,v,-frame.sunDirection.xyz,am.rgb,am.a,nr.a)*frame.sunColor.rgb*shadow;
    for(int i=0;i<int(frame.info.x);i++) {
        vec3 d=frame.pointPosition[i].xyz-world;
        c+=brdf(n,v,normalize(d),am.rgb,am.a,nr.a)*frame.pointColor[i].rgb/max(dot(d,d),.01);
    }
    c+=am.rgb*(1-am.a)*mix(vec3(.085,.065,.055),vec3(.34,.40,.48),n.y*.5+.5);
    c+=mix(vec3(.04),am.rgb,am.a)*mix(sky(reflect(-v,n)),vec3(.31,.35,.4),nr.a)*.48;
    // 输出只有 Beauty。调试通道由 presentation 单独出图，光照结果不因它改变，
    // 因此同一帧同时要 Beauty 与调试图时两者互不覆盖，历史里也只有 Beauty。
    outputColor=vec4(c,1);
}
