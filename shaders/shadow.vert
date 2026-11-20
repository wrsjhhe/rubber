#version 450
#include "frame.glsl"
layout(location=0) in vec3 position;
void main() {
    gl_Position = frame.lightVP[draw.cascade] * instances.data[draw.object].model * vec4(position,1);
}
