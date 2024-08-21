/******************************************************************************
 * Copyright (c) 2019-2024, NVIDIA CORPORATION. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *  * Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *  * Neither the name of NVIDIA CORPORATION nor the names of its
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *****************************************************************************/

import common;
import material;
import mdl_target_code_types;
import mdl_renderer_runtime;

typedef int MaterialFlags;
static const MaterialFlags MATERIAL_FLAG_NONE = 0;
static const MaterialFlags MATERIAL_FLAG_OPAQUE = 1 << 0; // allows to skip opacity evaluation
static const MaterialFlags MATERIAL_FLAG_SINGLE_SIDED = 1 << 1; // geometry is only visible from the front side

// ------------------------------------------------------------------------------------------------
// defined in the global root signature
// ------------------------------------------------------------------------------------------------

// Ray tracing acceleration structure, accessed as a SRV
RaytracingAccelerationStructure SceneBVH : register(t0, space0);

// Environment map and sample data for importance sampling
Texture2D<float4> environment_texture : register(t0, space1);
StructuredBuffer<Environment_sample_data> environment_sample_buffer : register(t1, space1);

// ------------------------------------------------------------------------------------------------
// defined in the local root signature
// ------------------------------------------------------------------------------------------------

// mesh data
StructuredBuffer<uint> indices: register(t2, space0);

// geometry data
// as long as there are only a few values here, place them directly instead of a constant buffer
cbuffer _Geometry_constants_0 : register(b2, space0) { uint geometry_vertex_buffer_byte_offset; }
cbuffer _Geometry_constants_1 : register(b3, space0) { uint geometry_vertex_stride; }
cbuffer _Geometry_constants_2 : register(b4, space0) { uint geometry_index_offset; }
cbuffer _Geometry_constants_3 : register(b5, space0) { uint geometry_scene_data_info_offset; }

cbuffer Material_constants : register(b0, space3)
{
    // shared for all material compiled from the same MDL material
    // - none -

    // individual properties of the different material instances
    int material_id;
    uint material_flags;
}

// ------------------------------------------------------------------------------------------------
// helper
// ------------------------------------------------------------------------------------------------

// selects one light source randomly
float3 sample_lights(Shading_state_material state, out float3 to_light, out float light_pdf, inout uint seed)
{
    float p_select_light = 1.0f;
    if (point_light_enabled != 0)
    {
        // keep it simple and use either point light or environment light, each with the same
        // probability. If the environment factor is zero, we always use the point light
        // Note: see also miss shader
        p_select_light = environment_intensity_factor > 0.0f ? 0.5f : 1.0f;

        // in general, you would select the light depending on the importance of it
        // e.g. by incorporating their luminance

        // randomly select one of the lights
        if (rnd(seed) <= p_select_light)
        {
            light_pdf = DIRAC; // infinity

            // compute light direction and distance
			to_light = point_light_position - state.position.val;

            const float inv_distance2 = 1.0f / dot(to_light, to_light);
            to_light *= sqrt(inv_distance2);

            return point_light_intensity * inv_distance2 * 0.25f * M_ONE_OVER_PI / p_select_light;
        }

        // probability to select the environment instead
        p_select_light = (1.0f - p_select_light);
    }

    // light from the environment
    float3 radiance = environment_sample(   // (see common.hlsl)
        environment_texture,                // assuming lat long map
        environment_sample_buffer,          // importance sampling data of the environment map
        seed,
        to_light,
        light_pdf);

    // return radiance over pdf
    light_pdf *= p_select_light;
    return radiance / light_pdf;
}


// fetch vertex data with known layout
float3 fetch_vertex_data_float3(const uint index, const uint byte_offset)
{
    const uint address =
        geometry_vertex_buffer_byte_offset + // base address for this part of the mesh
        geometry_vertex_stride * index +     // offset to the selected vertex
        byte_offset;                         // offset within the vertex

    return asfloat(vertices.Load3(address));
}

// fetch vertex data with known layout
float4 fetch_vertex_data_float4(const uint index, const uint byte_offset)
{
    const uint address =
        geometry_vertex_buffer_byte_offset + // base address for this part of the mesh
        geometry_vertex_stride * index +     // offset to the selected vertex
        byte_offset;                         // offset within the vertex

    return asfloat(vertices.Load4(address));
}

bool is_back_face()
{
    // get vertex indices for the hit triangle
    const uint index_offset = 3 * PrimitiveIndex() + geometry_index_offset;
    const uint3 vertex_indices = uint3(indices[index_offset + 0],
                                       indices[index_offset + 1],
                                       indices[index_offset + 2]);

    // get position of the hit point
    const float3 pos0 = fetch_vertex_data_float3(vertex_indices.x, VERT_BYTEOFFSET_POSITION);
    const float3 pos1 = fetch_vertex_data_float3(vertex_indices.y, VERT_BYTEOFFSET_POSITION);
    const float3 pos2 = fetch_vertex_data_float3(vertex_indices.z, VERT_BYTEOFFSET_POSITION);

    // compute geometry normal and check for back face hit
    const float3 geom_normal = normalize(cross(pos1 - pos0, pos2 - pos0));
    return dot(geom_normal, ObjectRayDirection()) > 0.0f;
}

void setup_mdl_shading_state(out Shading_state_material mdl_state, Attributes attrib)
{
    // get vertex indices for the hit triangle
    const uint index_offset = 3 * PrimitiveIndex() + geometry_index_offset;
    const uint3 vertex_indices = uint3(indices[index_offset + 0],
                                       indices[index_offset + 1],
                                       indices[index_offset + 2]);

    // coordinates inside the triangle
    const float3 barycentric = float3(1.0f - attrib.bary.x - attrib.bary.y, attrib.bary.x, attrib.bary.y);

    // mesh transformations
    const float4x4 object_to_world = to4x4(ObjectToWorld());
    const float4x4 world_to_object = to4x4(WorldToObject());

    // get position of the hit point
    const float3 pos0 = fetch_vertex_data_float3(vertex_indices.x, VERT_BYTEOFFSET_POSITION);
    const float3 pos1 = fetch_vertex_data_float3(vertex_indices.y, VERT_BYTEOFFSET_POSITION);
    const float3 pos2 = fetch_vertex_data_float3(vertex_indices.z, VERT_BYTEOFFSET_POSITION);
    float3 hit_position = pos0 * barycentric.x + pos1 * barycentric.y + pos2 * barycentric.z;
    hit_position = mul(object_to_world, float4(hit_position, 1)).xyz;

    // get normals (geometry normal and interpolated vertex normal)
    const float3 geom_normal = normalize(cross(pos1 - pos0, pos2 - pos0));
    const float3 normal = normalize(fetch_vertex_data_float3(vertex_indices.x, VERT_BYTEOFFSET_NORMAL) * barycentric.x
                                    + fetch_vertex_data_float3(vertex_indices.y, VERT_BYTEOFFSET_NORMAL) * barycentric.y
                                    + fetch_vertex_data_float3(vertex_indices.z, VERT_BYTEOFFSET_NORMAL) * barycentric.z);

    // transform normals using inverse transpose
    // -  world_to_object = object_to_world^-1
    // -  mul(v, world_to_object) = mul(object_to_world^-T, v)
    float3 world_geom_normal = normalize(mul(float4(geom_normal, 0), world_to_object).xyz);
    float3 world_normal = normalize(mul(float4(normal, 0), world_to_object).xyz);

    // reconstruct tangent frame from vertex data
    float3 world_tangent, world_binormal;
    float4 tangent0 = fetch_vertex_data_float4(vertex_indices.x, VERT_BYTEOFFSET_TANGENT) * barycentric.x
                      + fetch_vertex_data_float4(vertex_indices.y, VERT_BYTEOFFSET_TANGENT) * barycentric.y
                      + fetch_vertex_data_float4(vertex_indices.z, VERT_BYTEOFFSET_TANGENT) * barycentric.z;
    tangent0.xyz = normalize(tangent0.xyz);
    world_tangent = normalize(mul(object_to_world, float4(tangent0.xyz, 0)).xyz);
    world_tangent = normalize(world_tangent - dot(world_tangent, world_normal) * world_normal);
    world_binormal = cross(world_normal, world_tangent) * tangent0.w;

    // flip normals to the side of the incident ray
    const bool backfacing_primitive = dot(world_geom_normal, WorldRayDirection()) > 0.0;
    if (backfacing_primitive)
        world_geom_normal *= -1.0f;

    if (dot(world_normal, WorldRayDirection()) > 0.0)
        world_normal *= -1.0f;

    // handle low tessellated meshes with smooth normals
    float3 k2 = reflect(WorldRayDirection(), world_normal);
    if (dot(world_geom_normal, k2) < 0.0f)
        world_normal = world_geom_normal;

    // fill the actual state fields used by MD
    mdl_state.normal = world_normal;
    mdl_state.geom_normal = world_geom_normal;
	// currently not supported
	mdl_state.position.val = hit_position;
	mdl_state.position.dx = float3(0, 0, 0);
	mdl_state.position.dy = float3(0, 0, 0);
    mdl_state.animation_time = (enable_animiation >= 0) ? total_time : 0.0f;
    mdl_state.tangent_u[0] = world_tangent;
    mdl_state.tangent_v[0] = world_binormal;
    mdl_state.ro_data_segment_offset = 0;
    mdl_state.world_to_object = world_to_object;
    mdl_state.object_to_world = object_to_world;
    mdl_state.object_id = 0;
    mdl_state.meters_per_scene_unit = meters_per_scene_unit;
    mdl_state.arg_block_offset = 0;

    // fill the renderer state information
    mdl_state.renderer_state.scene_data_info_offset = geometry_scene_data_info_offset;
    mdl_state.renderer_state.scene_data_geometry_byte_offset = geometry_vertex_buffer_byte_offset;
    mdl_state.renderer_state.hit_vertex_indices = vertex_indices;
    mdl_state.renderer_state.barycentric = barycentric;
    mdl_state.renderer_state.hit_backface = backfacing_primitive;

    // get texture coordinates using a manually added scene data element with the scene data id
    // defined as `SCENE_DATA_ID_TEXCOORD_0`
    // (see end of target code generation on application side)
    float2 texcoord0 = scene_data_lookup_float2(mdl_state, 1 /* SCENE_DATA_ID_TEXCOORD_0 */, float2(0.0f, 0.0f), false);

    // apply uv transformations
    texcoord0 = texcoord0 * uv_scale + uv_offset;
    if (uv_repeat != 0)
        texcoord0 = texcoord0 - floor(texcoord0);
    if (uv_saturate != 0)
        texcoord0 = saturate(texcoord0);

	// would make sense in a rasterizer. for a ray tracers this is not straight forward
	mdl_state.text_coords[0].val = float3(texcoord0, 0);
	mdl_state.text_coords[0].dx = float3(0, 0, 0);
	mdl_state.text_coords[0].dy = float3(0, 0, 0);
}


// ------------------------------------------------------------------------------------------------
// MDL hit group shader
// ------------------------------------------------------------------------------------------------

[shader("closesthit")]
void MdlRadianceClosestHitProgram(inout RadianceHitInfo payload, Attributes attrib)
{
    // setup MDL state
    Shading_state_material mdl_state;
    setup_mdl_shading_state(mdl_state, attrib);

	mdl_init(mdl_state);

    // thin-walled materials are allowed to have a different back side
    // buy the can't have volumetric properties
	const bool thin_walled = mdl_thin_walled(mdl_state);

    // for thin-walled materials there is no 'inside'
    const bool inside = has_flag(payload.flags, FLAG_INSIDE);
    const float ior1 = (inside && !thin_walled) ? -1.0f :  1.0f;
    const float ior2 = (inside && !thin_walled) ?  1.0f : -1.0f;

    // Sample Light Sources for next event estimation
    //---------------------------------------------------------------------------------------------

    float3 to_light;
    float light_pdf;
    float3 radiance_over_pdf = sample_lights(mdl_state, to_light, light_pdf, payload.seed);

    // do not next event estimation (but delay the adding of contribution)
    float3 contribution = float3(0.0f, 0.0f, 0.0f);
    const bool next_event_valid = ((dot(to_light, mdl_state.geom_normal) > 0.0f) != inside)
                                  && (light_pdf != 0.0f)
                                  && !has_flag(payload.flags, FLAG_LAST_PATH_SEGMENT);

    if (next_event_valid)
    {
        // call generated mdl function to evaluate the scattering BSDF
        Bsdf_evaluate_data eval_data = (Bsdf_evaluate_data)0;
        eval_data.ior1 = ior1;
        eval_data.ior2 = ior2;
        eval_data.k1 = -WorldRayDirection();
        eval_data.k2 = to_light;

        // Always use surface scattering
		mdl_surface_scattering_evaluate(eval_data, mdl_state);

        // compute lighting for this light
        if(eval_data.pdf > 0.0f)
        {
            const float mis_weight = (light_pdf == DIRAC)
                ? 1.0f
                : light_pdf / (light_pdf + eval_data.pdf);

            // sample weight
            const float3 w = payload.weight * radiance_over_pdf * mis_weight;
			contribution += w * eval_data.bsdf_diffuse;
			contribution += w * eval_data.bsdf_glossy;
        }
    }

    // Sample direction of the next ray
    //---------------------------------------------------------------------------------------------

    // not a camera ray anymore
    remove_flag(payload.flags, FLAG_CAMERA_RAY);

    Bsdf_sample_data sample_data = (Bsdf_sample_data) 0;
    sample_data.ior1 = ior1;                    // IOR current medium
    sample_data.ior2 = ior2;                    // IOR other side
    sample_data.k1 = -WorldRayDirection();      // outgoing direction
    sample_data.xi = rnd4(payload.seed);        // random sample number

    // Always use surface scattering
	mdl_surface_scattering_sample(sample_data, mdl_state);

    // stop on absorb
    if (sample_data.event_type == BSDF_EVENT_ABSORB)
    {
        add_flag(payload.flags, FLAG_DONE);
        // no not return here, we need to do next event estimation first
    }
    else
    {
        // flip inside/outside on transmission
        // setup next path segment
        payload.ray_direction_next = sample_data.k2;
        payload.weight *= sample_data.bsdf_over_pdf;
        if ((sample_data.event_type & BSDF_EVENT_TRANSMISSION) != 0)
        {
            toggle_flag(payload.flags, FLAG_INSIDE);
            // continue on the opposite side
			payload.ray_origin_next = offset_ray(mdl_state.position.val, -mdl_state.geom_normal);
        }
        else
        {
            // continue on the current side
			payload.ray_origin_next = offset_ray(mdl_state.position.val, mdl_state.geom_normal);
        }

        if ((sample_data.event_type & BSDF_EVENT_SPECULAR) != 0)
            payload.last_bsdf_pdf = DIRAC;
        else
            payload.last_bsdf_pdf = sample_data.pdf;
    }

    // Add contribution from next event estimation if not shadowed
    //---------------------------------------------------------------------------------------------

    // cast a shadow ray; assuming light is always outside
    RayDesc ray;
	ray.Origin = offset_ray(mdl_state.position.val, mdl_state.geom_normal * (inside ? -1.0f : 1.0f));
    ray.Direction = to_light;
    ray.TMin = 0.0f;
    ray.TMax = far_plane_distance;

    // prepare the ray and payload but trace at the end to reduce the amount of data that has
    // to be recovered after coming back from the shadow trace
    ShadowHitInfo shadow_payload;
    shadow_payload.isHit = false;
    shadow_payload.seed = payload.seed;

    TraceRay(
        SceneBVH,               // AccelerationStructure
        RAY_FLAG_NONE,          // RayFlags
        0xFF /* allow all */,   // InstanceInclusionMask
        RAY_TYPE_SHADOW,        // RayContributionToHitGroupIndex
        RAY_TYPE_COUNT,         // MultiplierForGeometryContributionToHitGroupIndex
        RAY_TYPE_SHADOW,        // MissShaderIndex
        ray,
        shadow_payload);

    // add to ray contribution from next event estimation
    if (!shadow_payload.isHit)
        payload.contribution += contribution;
}

[shader("anyhit")]
void MdlRadianceAnyHitProgram(inout RadianceHitInfo payload, Attributes attrib)
{
    // back face culling
    if (has_flag(material_flags, MATERIAL_FLAG_SINGLE_SIDED)) {
        if (is_back_face()) {
            IgnoreHit();
            return;
        }
    }

    // early out if there is no opacity function
    if (has_flag(material_flags, MATERIAL_FLAG_OPAQUE))
        return;

    // setup MDL state
    Shading_state_material mdl_state;
    setup_mdl_shading_state(mdl_state, attrib);

    // evaluate the cutout opacity
    const float opacity = mdl_standalone_geometry_cutout_opacity(mdl_state);

    // do alpha blending the stochastically way
    if (rnd(payload.seed) < opacity)
        return;

    IgnoreHit();
}

// ------------------------------------------------------------------------------------------------
// MDL shadow group shader
// ------------------------------------------------------------------------------------------------

[shader("anyhit")]
void MdlShadowAnyHitProgram(inout ShadowHitInfo payload, Attributes attrib)
{
    // back face culling
    if (has_flag(material_flags, MATERIAL_FLAG_SINGLE_SIDED)) {
        if (is_back_face()) {
            IgnoreHit();
            return;
        }
    }

    // early out if there is no opacity function
    if (has_flag(material_flags, MATERIAL_FLAG_OPAQUE))
    {
        payload.isHit = true;
        AcceptHitAndEndSearch();
        return;
    }

    // setup MDL state
    Shading_state_material mdl_state;
    setup_mdl_shading_state(mdl_state, attrib);

    // evaluate the cutout opacity
    const float opacity = mdl_standalone_geometry_cutout_opacity(mdl_state);

    // do alpha blending the stochastically way
    if (rnd(payload.seed) < opacity)
    {
        payload.isHit = true;
        AcceptHitAndEndSearch();
        return;
    }

    IgnoreHit();
}
