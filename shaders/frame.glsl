layout(std140,set=0,binding=0) uniform Frame {
    mat4 vp, oldVP, invVP, lightVP[4];
    vec4 eye, sunDirection, sunColor, splits, jitter, settings, pointPosition[16], pointColor[16], info, lens;
    vec4 shadowOptions;
} frame;
struct Instance {
    mat4 model;
    vec4 colorRough, material;
    uvec4 label;
    vec4 boundsLo, boundsHi;
};
layout(std430,set=0,binding=1) readonly buffer Instances { Instance data[]; } instances;
struct History {
    mat4 oldModel;
    uvec4 label;
};
layout(std430,set=0,binding=2) readonly buffer Histories { History data[]; } histories;
layout(push_constant) uniform Draw { uint object; int cascade; int stable; } draw;
