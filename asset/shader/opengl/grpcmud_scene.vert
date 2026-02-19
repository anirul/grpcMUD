#version 330 core

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec2 in_texcoord;

out vec3 vert_normal;
out vec3 vert_world_position;

uniform mat4 projection;
uniform mat4 view;
uniform mat4 model;

void main()
{
    vec4 world_position = model * vec4(in_position, 1.0);
    vert_world_position = world_position.xyz;

    mat3 normal_matrix = mat3(transpose(inverse(model)));
    vert_normal = normalize(normal_matrix * in_normal);

    gl_Position = projection * view * world_position;
}
