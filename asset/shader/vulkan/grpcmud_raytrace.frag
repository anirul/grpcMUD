#version 450

layout(location = 0) in vec2 out_uv;
layout(location = 0) out vec4 frag_color;

layout(set = 0, binding = 0) uniform sampler2D ground_texture;
layout(set = 0, binding = 1) uniform sampler2D wall_texture;

struct Vertex
{
    vec3 position;
    float pad0;
    vec3 normal;
    float pad1;
    vec2 uv;
    vec2 pad2;
};

struct Triangle
{
    Vertex v0;
    Vertex v1;
    Vertex v2;
};

layout(std430, set = 0, binding = 2) buffer FloorTrianglesBuffer
{
    Triangle FloorTriangles[];
};

layout(std430, set = 0, binding = 3) buffer WallTrianglesBuffer
{
    Triangle WallTriangles[];
};

layout(std430, set = 0, binding = 4) buffer PlayerTrianglesBuffer
{
    Triangle PlayerTriangles[];
};

layout(std430, set = 0, binding = 5) buffer NpcTrianglesBuffer
{
    Triangle NpcTriangles[];
};

layout(set = 0, binding = 6) uniform UniformBlock
{
    mat4 projection;
    mat4 view;
    mat4 projection_inv;
    mat4 view_inv;
    mat4 model;
    mat4 model_inv;
    mat4 env_map_model;
    vec4 camera_position;
    vec4 light_dir;
    vec4 light_color;
    vec4 time_s;
} ubo;

bool RayTriangleIntersect(
    vec3 ray_origin,
    vec3 ray_dir,
    Triangle tri,
    out float t_out,
    out vec2 bary_out)
{
    const float kEps = 1e-6;
    vec3 edge1 = tri.v1.position - tri.v0.position;
    vec3 edge2 = tri.v2.position - tri.v0.position;
    vec3 pvec = cross(ray_dir, edge2);
    float det = dot(edge1, pvec);
    if (abs(det) < kEps)
    {
        return false;
    }

    float inv_det = 1.0 / det;
    vec3 tvec = ray_origin - tri.v0.position;
    float u = dot(tvec, pvec) * inv_det;
    if (u < 0.0 || u > 1.0)
    {
        return false;
    }

    vec3 qvec = cross(tvec, edge1);
    float v = dot(ray_dir, qvec) * inv_det;
    if (v < 0.0 || (u + v) > 1.0)
    {
        return false;
    }

    float t = dot(edge2, qvec) * inv_det;
    if (t <= kEps)
    {
        return false;
    }

    t_out = t;
    bary_out = vec2(u, v);
    return true;
}

vec2 InterpolateUv(Triangle tri, vec2 bary)
{
    float w = 1.0 - bary.x - bary.y;
    return (tri.v0.uv * w) + (tri.v1.uv * bary.x) + (tri.v2.uv * bary.y);
}

vec3 MaterialColor(int material_id, vec2 uv)
{
    if (material_id == 0)
    {
        vec2 uv_a = fract(uv * 2.0);
        vec2 uv_b = fract((uv.yx + vec2(0.37, 0.11)) * 3.0);
        float tex_a = texture(ground_texture, uv_a).r;
        float tex_b = texture(ground_texture, uv_b).g;
        float blend = clamp((tex_a * 0.55) + (tex_b * 0.45), 0.0, 1.0);
        return mix(vec3(0.08, 0.24, 0.07), vec3(0.24, 0.50, 0.18), blend);
    }
    if (material_id == 1)
    {
        vec2 wall_uv = fract(uv * 1.75);
        vec3 rocky = texture(wall_texture, wall_uv).rgb;
        return rocky * vec3(0.95, 0.92, 0.88);
    }
    if (material_id == 2)
    {
        return vec3(0.22, 0.58, 0.92);
    }
    return vec3(0.85, 0.37, 0.32);
}

void main()
{
    vec2 uv = out_uv * 2.0 - 1.0;
    vec4 clip_pos = vec4(uv, -1.0, 1.0);
    vec4 view_pos = ubo.projection_inv * clip_pos;
    view_pos = vec4(view_pos.xy, -1.0, 0.0);
    vec3 ray_dir_world = normalize((ubo.view_inv * view_pos).xyz);

    vec3 ray_origin_model = (ubo.model_inv * vec4(ubo.camera_position.xyz, 1.0)).xyz;
    vec3 ray_dir_model = normalize(mat3(ubo.model_inv) * ray_dir_world);

    float best_t = 1e30;
    int best_material = -1;
    vec3 best_normal_model = vec3(0.0);
    vec2 best_uv = vec2(0.0);

    for (int i = 0; i < FloorTriangles.length(); ++i)
    {
        float t;
        vec2 bary;
        if (RayTriangleIntersect(ray_origin_model, ray_dir_model, FloorTriangles[i], t, bary) &&
            t < best_t)
        {
            best_t = t;
            best_material = 0;
            float w = 1.0 - bary.x - bary.y;
            best_normal_model = normalize(FloorTriangles[i].v0.normal * w +
                                          FloorTriangles[i].v1.normal * bary.x +
                                          FloorTriangles[i].v2.normal * bary.y);
            best_uv = InterpolateUv(FloorTriangles[i], bary);
        }
    }
    for (int i = 0; i < WallTriangles.length(); ++i)
    {
        float t;
        vec2 bary;
        if (RayTriangleIntersect(ray_origin_model, ray_dir_model, WallTriangles[i], t, bary) &&
            t < best_t)
        {
            best_t = t;
            best_material = 1;
            float w = 1.0 - bary.x - bary.y;
            best_normal_model = normalize(WallTriangles[i].v0.normal * w +
                                          WallTriangles[i].v1.normal * bary.x +
                                          WallTriangles[i].v2.normal * bary.y);
            best_uv = InterpolateUv(WallTriangles[i], bary);
        }
    }
    for (int i = 0; i < PlayerTriangles.length(); ++i)
    {
        float t;
        vec2 bary;
        if (RayTriangleIntersect(ray_origin_model, ray_dir_model, PlayerTriangles[i], t, bary) &&
            t < best_t)
        {
            best_t = t;
            best_material = 2;
            float w = 1.0 - bary.x - bary.y;
            best_normal_model = normalize(PlayerTriangles[i].v0.normal * w +
                                          PlayerTriangles[i].v1.normal * bary.x +
                                          PlayerTriangles[i].v2.normal * bary.y);
            best_uv = InterpolateUv(PlayerTriangles[i], bary);
        }
    }
    for (int i = 0; i < NpcTriangles.length(); ++i)
    {
        float t;
        vec2 bary;
        if (RayTriangleIntersect(ray_origin_model, ray_dir_model, NpcTriangles[i], t, bary) &&
            t < best_t)
        {
            best_t = t;
            best_material = 3;
            float w = 1.0 - bary.x - bary.y;
            best_normal_model = normalize(NpcTriangles[i].v0.normal * w +
                                          NpcTriangles[i].v1.normal * bary.x +
                                          NpcTriangles[i].v2.normal * bary.y);
            best_uv = InterpolateUv(NpcTriangles[i], bary);
        }
    }

    if (best_material < 0)
    {
        float sky = clamp(0.55 + (0.45 * ray_dir_world.y), 0.0, 1.0);
        vec3 sky_color = mix(vec3(0.14, 0.17, 0.20), vec3(0.42, 0.50, 0.62), sky);
        frag_color = vec4(sky_color, 1.0);
        return;
    }

    vec3 normal_world = normalize(transpose(mat3(ubo.model_inv)) * best_normal_model);
    if (dot(normal_world, -ray_dir_world) < 0.0)
    {
        normal_world = -normal_world;
    }

    vec3 light_dir_world = ubo.light_dir.xyz;
    if (dot(light_dir_world, light_dir_world) < 1e-4)
    {
        light_dir_world = vec3(0.6, 1.0, 0.45);
    }
    light_dir_world = normalize(light_dir_world);

    float diffuse = max(dot(normal_world, light_dir_world), 0.0);
    float shade = 0.25 + (0.75 * diffuse);
    vec3 base_color = MaterialColor(best_material, best_uv);
    frag_color = vec4(base_color * shade, 1.0);
}
