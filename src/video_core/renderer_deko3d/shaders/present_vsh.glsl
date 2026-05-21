#version 460

layout (location = 0) in vec4 in_position;
layout (location = 1) in vec4 in_color;
layout (location = 2) in vec2 in_texcoord0;

layout (location = 0) out vec4 out_color;
layout (location = 1) out vec2 out_texcoord0;

void main()
{
    gl_Position = vec4(in_position.x, in_position.y, -in_position.z, in_position.w);
    out_color = in_color;
    out_texcoord0 = in_texcoord0;
}
