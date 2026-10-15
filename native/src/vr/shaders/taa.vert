#version 450
// Full-screen triangle without a vertex buffer (both TAA passes; viewport = the whole eye, positive height,
// so gl_FragCoord addresses the same texel of the eye colour, the eye depth and the TAA history).
void main(){
    const vec2 p=vec2(float((gl_VertexIndex<<1)&2),float(gl_VertexIndex&2));
    gl_Position=vec4(p*2.0-1.0,0.0,1.0);
}
