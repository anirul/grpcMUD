#version 450 core

in vec2 out_uv;

out vec4 frag_color;

uniform mat4 projection_inv;
uniform mat4 view_inv;
uniform mat4 model_inv;
uniform vec3 camera_position;

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

layout(std430, binding = 0) buffer FloorTrianglesBuffer
{
    Triangle FloorTriangles[];
};

layout(std430, binding = 1) buffer WallTrianglesBuffer
{
    Triangle WallTriangles[];
};

layout(std430, binding = 2) buffer PlayerTrianglesBuffer
{
    Triangle PlayerTriangles[];
};

layout(std430, binding = 3) buffer NpcTrianglesBuffer
{
    Triangle NpcTriangles[];
};

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

vec3 MaterialColor(int material_id)
{
    if (material_id == 0)
    {
        return vec3(0.22, 0.26, 0.20); // floor
    }
    if (material_id == 1)
    {
        return vec3(0.43, 0.39, 0.35); // walls
    }
    if (material_id == 2)
    {
        return vec3(0.22, 0.58, 0.92); // players
    }
    return vec3(0.85, 0.37, 0.32); // npcs
}

void main()
{
    vec2 uv = out_uv * 2.0 - 1.0;
    vec4 clip_pos = vec4(uv, -1.0, 1.0);
    vec4 view_pos = projection_inv * clip_pos;
    view_pos = vec4(view_pos.xy, -1.0, 0.0);
    vec3 ray_dir_world = normalize((view_inv * view_pos).xyz);

    vec3 ray_origin_model = (model_inv * vec4(camera_position, 1.0)).xyz;
    vec3 ray_dir_model = normalize(mat3(model_inv) * ray_dir_world);

    float best_t = 1e30;
    int best_material = -1;
    vec3 best_normal_model = vec3(0.0);

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
        }
    }

    if (best_material < 0)
    {
        float sky = clamp(0.55 + (0.45 * ray_dir_world.y), 0.0, 1.0);
        vec3 sky_color = mix(vec3(0.14, 0.17, 0.20), vec3(0.42, 0.50, 0.62), sky);
        frag_color = vec4(sky_color, 1.0);
        return;
    }

    vec3 normal_world = normalize(transpose(mat3(model_inv)) * best_normal_model);
    if (dot(normal_world, -ray_dir_world) < 0.0)
    {
        normal_world = -normal_world;
    }

    vec3 light_dir = normalize(vec3(0.6, 1.0, 0.45));
    float diffuse = max(dot(normal_world, light_dir), 0.0);
    float shade = 0.25 + (0.75 * diffuse);
    vec3 base_color = MaterialColor(best_material);
    frag_color = vec4(base_color * shade, 1.0);
}
