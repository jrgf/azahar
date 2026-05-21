#version 460

layout (location = 0) in vec4 in_color;
layout (location = 1) in vec2 in_texcoord0;
layout (location = 0) out vec4 out_color;

layout (binding = 0) uniform sampler2D tex0;

void main()
{
    out_color = in_color * texture(tex0, in_texcoord0);
}
