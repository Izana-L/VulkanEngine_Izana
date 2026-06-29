#version 450

layout(location = 0) out vec4 out_color;

void main()
{
    // Bright red  impossible to miss if anything renders.
    out_color = vec4(1.0, 0.0, 0.0, 1.0);
}
