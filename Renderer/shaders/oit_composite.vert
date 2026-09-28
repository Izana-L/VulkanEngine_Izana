#version 450

// Composite of the weighted blended OIT (subpass 2 of the render pass):
// one triangle that covers the whole viewport, generated from
// gl_VertexIndex with no vertex buffer (the pipeline declares no vertex
// input). Drawn with vkCmdDraw(3, 1, 0, 0).
//
//   index 0 -> (-1, -1), index 1 -> (3, -1), index 2 -> (-1, 3)
//
// The parts outside the viewport are clipped. The depth written here is
// irrelevant: the composite draws with the depth test disabled, and the
// read-only depth attachment of the subpass is never written.
void main()
{
    const vec2 corner = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);

    gl_Position = vec4(corner * 2.0 - 1.0, 0.0, 1.0);
}
