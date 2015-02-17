#version 110
#pragma vp_entryPoint "oe_clamp_fragment"
#pragma vp_location   "fragment_coloring"

varying float oe_clamp_alpha;

void oe_clamp_fragment(inout vec4 color)
{
    // adjust the alpha component to "hide" geometry beyond the visible horizon.
    color.a *= oe_clamp_alpha;
}