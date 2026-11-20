#version 450
#include "frame.glsl"
layout(location=0) in vec2 uv;
layout(location=0) out vec4 outputColor;
layout(set=1,binding=0) uniform sampler2D albedoTex;
layout(set=1,binding=1) uniform sampler2D normalTex;
layout(set=1,binding=3) uniform sampler2D metricTex;
layout(set=1,binding=4) uniform usampler2D idTex;
layout(set=1,binding=5) uniform sampler2D zTex;
layout(set=1,binding=6) uniform sampler2D shadowTex;
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
void main() {
    float z=texture(zTex,uv).r;
    vec4 p=frame.invVP*vec4(uv*2-1-frame.jitter.xy,z,1);
    vec3 world=p.xyz/p.w;
    uint id=texture(idTex,uv).r;
    if(id==0) {
        outputColor=vec4(frame.settings.w==0 ? sky(normalize(world-frame.eye.xyz)):vec3(0),1);
        return;
    }
    vec4 am=texture(albedoTex,uv), nr=texture(normalTex,uv);
    vec3 n=normalize(nr.xyz), v=normalize(frame.eye.xyz-world);
    float depth=texture(metricTex,uv).r;
    int cascade=int(depth>frame.splits.x)+int(depth>frame.splits.y)+int(depth>frame.splits.z);
    float shadow=frame.settings.z>0 ? visibility(world,cascade,n):1;
    vec3 c=brdf(n,v,-frame.sunDirection.xyz,am.rgb,am.a,nr.a)*frame.sunColor.rgb*shadow;
    for(int i=0;i<int(frame.info.x);i++) {
        vec3 d=frame.pointPosition[i].xyz-world;
        c+=brdf(n,v,normalize(d),am.rgb,am.a,nr.a)*frame.pointColor[i].rgb/max(dot(d,d),.01);
    }
    c+=am.rgb*(1-am.a)*mix(vec3(.085,.065,.055),vec3(.34,.40,.48),n.y*.5+.5);
    c+=mix(vec3(.04),am.rgb,am.a)*mix(sky(reflect(-v,n)),vec3(.31,.35,.4),nr.a)*.48;
    if(frame.settings.w==1) c=n*.5+.5;
    if(frame.settings.w==2) c=vec3(log(1+depth)/log(1+frame.splits.w));
    if(frame.settings.w==3) c=fract(float(id)*vec3(.618,.317,.753));
    if(frame.settings.w==4) c*=vec3[4](vec3(1,.45,.4),vec3(.5,1,.5),vec3(.4,.65,1),vec3(1,.8,.35))[cascade];
    outputColor=vec4(c,1);
}
