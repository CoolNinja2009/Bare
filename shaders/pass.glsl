#version 450
// pass.glsl: the vertex half of every canvas pass (bare_pass). One triangle
// covers the whole target; the app's fragment shader runs once per pixel and
// gets v_uv: (0, 0) at the target's top-left texel corner, (1, 1) at the
// bottom-right, the same convention texture() samples with.
//
// BARE_NO_UV builds the variant for fragment shaders that never read v_uv
// (only gl_FragCoord): D3D12 links stages by register, so a fragment shader
// without v_uv can't follow a vertex shader that writes it.

#ifdef BARE_VERTEX

#ifndef BARE_NO_UV
layout(location = 0) out vec2 v_uv;
#endif

void main() {
    vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));   // (0,0) (2,0) (0,2)
#ifndef BARE_NO_UV
    v_uv = p;
#endif
    gl_Position = vec4(p.x * 2.0 - 1.0, 1.0 - p.y * 2.0, 0.0, 1.0);
}

#endif
