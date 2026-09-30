#version 450

layout(constant_id = 1) const bool s_gamma_bound = true;

layout(binding = 0) uniform sampler2D s_image;
layout(binding = 1) uniform sampler1D s_gamma;

layout(location = 0) in  vec2 i_coord;
layout(location = 0) out vec4 o_color;

layout(push_constant)
uniform present_info_t {
  ivec2 src_offset;
  uvec2 src_extent;
  uint present_flags;
};

void main() {
  vec2 coord = vec2(src_offset) + vec2(src_extent) * i_coord;
  o_color = textureLod(s_image, coord, 0.0f);
  
  if (s_gamma_bound) {
    o_color = vec4(
      texture(s_gamma, o_color.r).r,
      texture(s_gamma, o_color.g).g,
      texture(s_gamma, o_color.b).b,
      o_color.a);
  }

  if ((present_flags & 1u) != 0u) {
    vec3 linear = clamp(o_color.rgb, vec3(0.0f), vec3(1.0f));
    vec3 low = linear * 12.92f;
    vec3 high = 1.055f * pow(linear, vec3(1.0f / 2.4f)) - 0.055f;
    o_color.rgb = mix(high, low, lessThanEqual(linear, vec3(0.0031308f)));
  }
  if ((present_flags & 2u) != 0u)
    o_color.a = 1.0f;
}
