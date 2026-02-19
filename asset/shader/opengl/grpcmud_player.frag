#version 330 core

in vec3 vert_normal;
in vec3 vert_world_position;

layout(location = 0) out vec4 frag_color;
layout(location = 1) out vec4 frag_zbuffer;

void main()
{
    vec3 light_dir = normalize(vec3(0.35, 1.0, -0.25));
    float diffuse = max(dot(normalize(vert_normal), light_dir), 0.0);
    float shade = 0.30 + (0.70 * diffuse);
    vec3 base_color = vec3(0.21, 0.57, 0.93);

    frag_color = vec4(base_color * shade, 1.0);
    float z = gl_FragCoord.z;
    frag_zbuffer = vec4(z, z, z, 1.0);
}
