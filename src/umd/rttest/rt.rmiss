#version 460
#extension GL_EXT_ray_tracing : require
layout(location = 0) rayPayloadInEXT uvec2 payload;
void main() { payload = uvec2(0xFFFFu, 0u); }
