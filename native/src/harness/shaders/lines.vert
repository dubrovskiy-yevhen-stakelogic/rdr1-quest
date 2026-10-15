#version 450
layout(push_constant) uniform Draw { mat4 vp; vec4 a; vec4 b; vec4 color; } draw;
layout(location=0) out vec4 color;
void main() {
    gl_Position = draw.vp * (gl_VertexIndex == 0 ? draw.a : draw.b);
    color = draw.color;
}
