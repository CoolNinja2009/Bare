#version 450
// pass.glsl: the vertex half of every canvas pass (bare_pass). One triangle
// covers the whole target; the app's fragment shader runs once per pixel and
// gets v_uv: (0, 0) at the target's top-left texel corner, (1, 1) at the
// bottom-right, the same convention texture() samples with.

#ifdef BARE_VERTEX

layout(location = 0) out vec2 v_uv;

void main() {
    vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));   // (0,0) (2,0) (0,2)
    v_uv = p;
    gl_Position = vec4(p.x * 2.0 - 1.0, 1.0 - p.y * 2.0, 0.0, 1.0);
}

#endif
