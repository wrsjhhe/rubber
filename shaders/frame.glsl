layout(std140,set=0,binding=0) uniform Frame {
    mat4 vp, oldVP, invVP, lightVP[4];
    vec4 eye, sunDirection, sunColor, splits, jitter, settings, pointPosition[16], pointColor[16], info;
} frame;
struct Instance {
    mat4 model, oldModel;
    vec4 colorRough, material;
    uvec4 label;
    vec4 boundsLo, boundsHi;
};
layout(std430,set=0,binding=1) readonly buffer Instances { Instance data[]; } instances;
layout(push_constant) uniform Draw { uint object; int cascade; int stable; } draw;
