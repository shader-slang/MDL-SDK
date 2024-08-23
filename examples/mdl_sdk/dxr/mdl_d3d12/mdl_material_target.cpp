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

#include "mdl_material_target.h"

#include <iostream>
#include <fstream>

#include "base_application.h"
#include "descriptor_heap.h"
#include "mdl_material.h"
#include "mdl_sdk.h"
#include "texture.h"

#include <example_shared.h>

namespace mi { namespace examples { namespace mdl_d3d12
{

std::string compute_shader_cache_filename(
    const Base_options& options,
    const std::string& material_hash,
    bool create_parent_folders = false)
{
    std::string compiler = "dxc";
#ifdef MDL_ENABLE_SLANG
    if(options.use_slang)
        compiler = "slang";
#endif

    std::string folder = mi::examples::io::get_executable_folder() +"/shader_cache/" + compiler;
    if (create_parent_folders)
        mi::examples::io::mkdir(folder);
    return folder + "/" + material_hash + ".bin";
}

// ------------------------------------------------------------------------------------------------

Mdl_material_target::Resource_callback::Resource_callback(
    Mdl_sdk* sdk, Mdl_material_target* target, Mdl_material* material)
    : m_sdk(sdk)
    , m_target(target)
    , m_material(material)
{
}

// ------------------------------------------------------------------------------------------------

mi::Uint32 Mdl_material_target::Resource_callback::get_resource_index(
    mi::neuraylib::IValue_resource const *resource)
{
    mi::base::Handle<const mi::neuraylib::ITarget_code> target_code(
        m_target->get_target_code());

    // resource available in the target code?
    // this is the case for resources that are in the material body and for
    // resources contained in the parameters of the first appearance of a material
    mi::Uint32 index = m_sdk->get_transaction().execute<mi::Uint32>(
        [&](mi::neuraylib::ITransaction* t)
    {
        return target_code->get_known_resource_index(t, resource);
    });

    // resource is part of the target code, so we use it
    if (index > 0)
    {
        // we loaded only the body resources so far so we only accept those as is
        switch (resource->get_kind())
        {
        case mi::neuraylib::IValue::VK_TEXTURE:
            if (target_code->get_texture_is_body_resource(index))
                return index;
            break;

        case mi::neuraylib::IValue::VK_LIGHT_PROFILE:
            if (target_code->get_light_profile_is_body_resource(index))
                return index;
            break;

        case mi::neuraylib::IValue::VK_BSDF_MEASUREMENT:
            if (target_code->get_bsdf_measurement_is_body_resource(index))
                return index;
            break;
        default:
            break;
        }
    }

    // invalid (or empty) resource
    const char* name = resource->get_value();
    if (!name)
    {
        return 0;
    }

    // All resources that are loaded for later appearances of a material, i.e. when a
    // material is reused (probably with different parameters), have to handled separately.
    // If the target was not yet generated (usually the case when a shared target code is used),
    // additional resources can be added to the list of resources of the target.
    // Otherwise, resources are added to the material (when separate link units are used).

    Mdl_resource_kind kind;
    Texture_dimension dimension = Texture_dimension::Undefined;
    switch (resource->get_kind())
    {
        case mi::neuraylib::IValue::VK_TEXTURE:
        {
            mi::base::Handle<const mi::neuraylib::IType> type(
                resource->get_type());

            mi::base::Handle<const mi::neuraylib::IType_texture> texture_type(
                type->get_interface<const mi::neuraylib::IType_texture>());

            switch (texture_type->get_shape())
            {
                case mi::neuraylib::IType_texture::TS_2D:
                    kind = Mdl_resource_kind::Texture;
                    dimension = Texture_dimension::Texture_2D;
                    break;

                case mi::neuraylib::IType_texture::TS_3D:
                    kind = Mdl_resource_kind::Texture;
                    dimension = Texture_dimension::Texture_3D;
                    break;

                default:
                    log_error("Invalid texture shape for: " + std::string(name), SRC);
                    return 0;
            }
            break;
        }

        case mi::neuraylib::IValue::VK_LIGHT_PROFILE:
            kind = Mdl_resource_kind::Light_profile;
            break;

        case mi::neuraylib::IValue::VK_BSDF_MEASUREMENT:
            kind = Mdl_resource_kind::Bsdf_measurement;
            break;

        default:
            log_error("Invalid resource kind for: " + std::string(name), SRC);
            return 0;
    }

    // store textures at the material
    size_t mat_resource_index = m_material->register_resource(kind, dimension, name);

    // log these manually defined indices
    log_info(
        "target code: " + m_target->get_compiled_material_hash() +
        " - texture id: " + std::to_string(mat_resource_index) +
        " (material id: " + std::to_string(m_material->get_id()) + ")" +
        " - resource: " + std::string(name) + " (reused material)");

    return mat_resource_index;
}

// ------------------------------------------------------------------------------------------------

mi::Uint32 Mdl_material_target::Resource_callback::get_string_index(
    mi::neuraylib::IValue_string const *s)
{
    // of the string was known to the compiler the mapped id MUST match
    // the one of the target code
    mi::base::Handle<const mi::neuraylib::ITarget_code> target_code(
        m_target->get_target_code());
    mi::Size n = target_code->get_string_constant_count();
    for (mi::Size i = 0; i < n; ++i)
        if (strcmp(target_code->get_string_constant(i), s->get_value()) == 0)
            return static_cast<mi::Uint32>(i);

    // invalid (or empty) string
    const char* name = s->get_value();
    if (!name || name[0] == '\0')
    {
        return 0;
    }

    // additional new string mappings:
    // store string constant at the material
    size_t mat_string_index = m_material->map_string_constant(name);
    assert(mat_string_index >= n); // the new IDs must not collide with the ones of the target code
    return mat_string_index;
}

// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------

Mdl_material_target::Mdl_material_target(
    Base_application* app,
    Mdl_sdk* sdk,
    const std::string& compiled_material_hash)
    : m_app(app)
    , m_sdk(sdk)
    , m_compiled_material_hash(compiled_material_hash)
    , m_target_code(nullptr)
    , m_generation_required(true)
    , m_hlsl_source_code("")
    , m_compilation_required(true)
    , m_dxil_compiled_libraries()
    , m_read_only_data_segment(nullptr)
    , m_target_resources()
    , m_target_string_constants()
{
    // add the empty resources
    for (size_t i = 0, n = static_cast<size_t>(Mdl_resource_kind::_Count); i < n; ++i)
    {
        Mdl_resource_kind kind_i = static_cast<Mdl_resource_kind>(i);
        m_target_resources[kind_i] = std::vector<Mdl_resource_assignment>();
    }

    // will be used in the shaders and when setting up the rt pipeline
    m_radiance_closest_hit_name = "MdlRadianceClosestHitProgram";
    m_radiance_any_hit_name = "MdlRadianceAnyHitProgram";
    m_shadow_any_hit_name = "MdlShadowAnyHitProgram";
}

// ------------------------------------------------------------------------------------------------

Mdl_material_target::~Mdl_material_target()
{
    m_target_code = nullptr;
    m_dxil_compiled_libraries.clear();

    if (m_read_only_data_segment) delete m_read_only_data_segment;

    // free heap block
    m_app->get_resource_descriptor_heap()->free_views(m_first_resource_heap_handle);
}

// ------------------------------------------------------------------------------------------------

mi::neuraylib::ITarget_resource_callback* Mdl_material_target::create_resource_callback(
    Mdl_material* material)
{
    return new Resource_callback(m_sdk, this, material);
}

// ------------------------------------------------------------------------------------------------

const mi::neuraylib::ITarget_code* Mdl_material_target::get_target_code() const
{
    if (!m_target_code)
        return nullptr;

    m_target_code->retain();
    return m_target_code.get();
}

// ------------------------------------------------------------------------------------------------

bool Mdl_material_target::add_material_to_link_unit(
    Mdl_material_target_interface& interface_data,
    Mdl_material* material,
    mi::neuraylib::ILink_unit* link_unit,
    mi::neuraylib::IMdl_execution_context* context)
{
    // get the compiled material and add the material to the link unit
    mi::base::Handle<const mi::neuraylib::ICompiled_material> compiled_material(
        m_sdk->get_transaction().access<const mi::neuraylib::ICompiled_material>(
            material->get_material_compiled_db_name().c_str()));

    // Selecting expressions to generate code for is one of the most important parts
    // You can either choose to generate all supported functions and just use them which is fine.
    // To reduce the effort in the code generation and compilation steps later it makes sense
    // to reduce the code size as good as possible. Therefore, we inspect the compiled
    // material before selecting functions for code generation.

    // third, either the bsdf or the edf need to be non-default (black)

    // helper function to check if a distribution function is invalid
    // if true, the distribution function needs no eval because it has no contribution
    auto is_invalid_df = [&compiled_material](const char* expression_path)
    {
        // fetch the constant expression
        mi::base::Handle<const mi::neuraylib::IExpression> expr(
            compiled_material->lookup_sub_expression(expression_path));
        if (expr->get_kind() != mi::neuraylib::IExpression::EK_CONSTANT)
            return false;

        // get the constant value
        mi::base::Handle<const mi::neuraylib::IExpression_constant> expr_constant(
            expr->get_interface<mi::neuraylib::IExpression_constant>());
        mi::base::Handle<const mi::neuraylib::IValue> value(
            expr_constant->get_value());

        return value->get_kind() == mi::neuraylib::IValue::VK_INVALID_DF;
    };

    // helper function to check if a color function needs to be evaluted
    // returns true if the expression value is constant black, otherwise true
    auto is_constant_black_color = [&compiled_material](const char* expression_path)
    {
        // fetch the constant expression
        mi::base::Handle<const mi::neuraylib::IExpression> expr(
            compiled_material->lookup_sub_expression(expression_path));
        if (expr->get_kind() != mi::neuraylib::IExpression::EK_CONSTANT)
            return false;

        // get the constant value
        mi::base::Handle<const mi::neuraylib::IExpression_constant> expr_constant(
            expr->get_interface<mi::neuraylib::IExpression_constant>());
        mi::base::Handle<const mi::neuraylib::IValue_color> value(
            expr_constant->get_value<mi::neuraylib::IValue_color>());
        if (!value)
            return false;

        // check for black
        for (mi::Size i = 0; i < value->get_size(); ++i)
        {
            mi::base::Handle<mi::neuraylib::IValue_float const> element(value->get_value(i));
            if (element->get_value() != 0.0f)
                return false;
        }
        return true;
    };

    // helper function to check if a bool function needs to be evaluted
    // returns true if the expression value is constant false, otherwise true
    auto is_constant_false = [&compiled_material](const char* expression_path)
    {
        // fetch the constant expression
        mi::base::Handle<const mi::neuraylib::IExpression> expr(
            compiled_material->lookup_sub_expression(expression_path));
        if (expr->get_kind() != mi::neuraylib::IExpression::EK_CONSTANT)
            return false;

        // get the constant value
        mi::base::Handle<const mi::neuraylib::IExpression_constant> expr_constant(
            expr->get_interface<mi::neuraylib::IExpression_constant>());
        mi::base::Handle<const mi::neuraylib::IValue_bool> value(
            expr_constant->get_value<mi::neuraylib::IValue_bool>());
        if (!value)
            return false;

        // check for "false"
        return value->get_value() == false;
    };

    // select expressions to generate HLSL code for
    std::vector<mi::neuraylib::Target_function_description> selected_functions;

    selected_functions.push_back(mi::neuraylib::Target_function_description(
        "init", "mdl_init"));

    // add surface scattering if available
    if (!is_invalid_df("surface.scattering"))
    {
        selected_functions.push_back(mi::neuraylib::Target_function_description(
            "surface.scattering", "mdl_surface_scattering"));
        interface_data.has_surface_scattering = true;
    }

    // add surface emission if available
    if (!is_invalid_df("surface.emission.emission") && !is_constant_black_color("surface.emission.intensity"))
    {
        selected_functions.push_back(mi::neuraylib::Target_function_description(
            "surface.emission.emission", "mdl_surface_emission"));
        selected_functions.push_back(mi::neuraylib::Target_function_description(
            "surface.emission.intensity", "mdl_surface_emission_intensity"));
        interface_data.has_surface_emission = true;
    }

    // add absorption
    if (!is_constant_black_color("volume.absorption_coefficient"))
    {
        selected_functions.push_back(mi::neuraylib::Target_function_description(
            "volume.absorption_coefficient", "mdl_volume_absorption_coefficient"));
        interface_data.has_volume_absorption = true;
    }

    // thin walled and potentially with a different backface
    if (!is_constant_false("thin_walled"))
    {
        selected_functions.push_back(mi::neuraylib::Target_function_description(
            "thin_walled", "mdl_thin_walled"));
        interface_data.can_be_thin_walled = true;

        // back faces could be different for thin walled materials
        // we only need to generate new code
        // 1. if surface and backface are different
        bool need_backface_bsdf =
            compiled_material->get_slot_hash(mi::neuraylib::SLOT_SURFACE_SCATTERING) !=
            compiled_material->get_slot_hash(mi::neuraylib::SLOT_BACKFACE_SCATTERING);
        bool need_backface_edf =
            compiled_material->get_slot_hash(mi::neuraylib::SLOT_SURFACE_EMISSION_EDF_EMISSION) !=
            compiled_material->get_slot_hash(mi::neuraylib::SLOT_BACKFACE_EMISSION_EDF_EMISSION);

        // 2. either the bsdf or the edf need to be non-default (black)
        bool none_default_backface = 
            !is_invalid_df("backface.scattering") || !is_invalid_df("backface.emission.emission");
        need_backface_bsdf &= none_default_backface;
        need_backface_edf &= none_default_backface;

        if (need_backface_bsdf || need_backface_edf)
        {
            // generate code for both backface functions here, even if they are black
            // because it could be requested to have black backsides
            selected_functions.push_back(mi::neuraylib::Target_function_description(
                "backface.scattering", "mdl_backface_scattering"));
            interface_data.has_backface_scattering = true;

            selected_functions.push_back(mi::neuraylib::Target_function_description(
                "backface.emission.emission", "mdl_backface_emission"));
            selected_functions.push_back(mi::neuraylib::Target_function_description(
                "backface.emission.intensity", "mdl_backface_emission_intensity"));
            interface_data.has_backface_emission = true;
        }
    }

    // it's possible that the material does not contain any feature this renderer supports
    if (selected_functions.size() > 1) // note, the 1 function added is 'init'
    {
        interface_data.has_init = true;
        link_unit->add_material(
            compiled_material.get(),
            selected_functions.data(), selected_functions.size(),
            context);

        if (!m_sdk->log_messages("Failed to select functions for code generation.", context, SRC))
            return false;
    }

    // compile cutout_opacity also as standalone version to be used in the anyhit programs,
    // to avoid costly precalculation of expressions only used by other expressions
    mi::neuraylib::Target_function_description standalone_opacity(
        "geometry.cutout_opacity", "mdl_standalone_geometry_cutout_opacity");

    link_unit->add_material(
        compiled_material.get(),
        &standalone_opacity, 1,
        context);

    if (!m_sdk->log_messages("Failed to add cutout_opacity for code generation.", context, SRC))
        return false;

    // get the resulting target code information
    // constant for the entire material, for one material per link unit 0
    interface_data.argument_layout_index = selected_functions[0].argument_block_index;
    return true;
}

// ------------------------------------------------------------------------------------------------

/// Keep a pointer (no ownership) to the material for notifying the material when the
/// target code generation is finished.
void Mdl_material_target::register_material(Mdl_material* material)
{
    Mdl_material_target* current_target = material->get_target_code();

    // mark changed because registered material is called only for new or changed materials
    m_generation_required = true;
    m_compilation_required = true;

    // register with this target code
    std::unique_lock<std::mutex> lock(m_materials_mtx);
    m_materials[material->get_id()] = material;
}

// ------------------------------------------------------------------------------------------------

bool Mdl_material_target::unregister_material(Mdl_material* material)
{
    if (material->get_target_code() != this)
    {
        log_error("Tried to remove a material from the wrong target: " + material->get_name(), SRC);
        return false;
    }

    std::unique_lock<std::mutex> lock(m_materials_mtx);
    auto found = m_materials.find(material->get_id());
    if (found != m_materials.end())
    {
        material->reset_target_interface();
        m_materials.erase(found);
        m_generation_required = true;
        m_compilation_required = true;
    }
    return true;
}

// ------------------------------------------------------------------------------------------------

size_t Mdl_material_target::get_material_resource_count(
    Mdl_resource_kind kind) const
{
    const auto& found = m_material_resource_count.find(kind);
    return found->second;
}

// ------------------------------------------------------------------------------------------------

uint32_t Mdl_material_target::map_string_constant(const std::string& string_value)
{
    // the empty string is also the invalid string
    if (string_value == "")
        return 0;

    // if the constant is already mapped, use it
    for (auto& c : m_target_string_constants)
        if (c.value == string_value)
            return c.runtime_string_id;

    // map the new constant. keep this mapping dense in order to easy the data layout on the GPU
    uint32_t runtime_id =
        m_target_string_constants.empty() ? 1 : m_target_string_constants.back().runtime_string_id + 1;
    Mdl_string_constant entry;
    entry.runtime_string_id = runtime_id;
    entry.value = string_value;

    m_target_string_constants.push_back(entry);
    return runtime_id;
}

// ------------------------------------------------------------------------------------------------

bool Mdl_material_target::visit_materials(std::function<bool(Mdl_material*)> action)
{
    std::lock_guard<std::mutex> lock(m_materials_mtx);
    for (auto it = m_materials.begin(); it != m_materials.end(); it++)
        if (!action(it->second))
            return false;
    return true;
}

// ------------------------------------------------------------------------------------------------

static bool ends_with(std::string_view str, std::string_view suffix)
{
    return str.size() >= suffix.size() && str.compare(str.size() - suffix.size(), suffix.size(), suffix) == 0;
}

Slang_global_state g_slang;

void Slang_global_state::set_compilation_mode(bool modules)
{
    printf("[S] set_compilation_mode: %d\n", modules);
    if (modules == ENABLE_MODULES)
        return;

    ENABLE_MODULES = modules;
    if (modules)
        load_modules();
    else
        erase_slang_modules();
}

bool Slang_global_state::load_global_session()
{
    Slang::Result r_global_session = slang_createGlobalSession(SLANG_API_VERSION, global_session.writeRef());
    if (SLANG_FAILED(r_global_session)) {
        printf("[S] failed to create the global session\n");
        return false;
    }

    printf("[S] successfully loaded the global session\n");
    return true;
}

bool Slang_global_state::load_session()
{
    // Local slang session
    slang_folder = mi::examples::io::get_executable_folder();
    slang_folder /= "content";
    slang_folder /= "slangified";
    
    const char* slang_folder_cstr = slang_folder.string().c_str();

    slang::TargetDesc target_desc = {};
    target_desc.format = SLANG_DXIL;
    target_desc.profile = g_slang.global_session->findProfile("lib_6_6");

    slang::SessionDesc desc {};
    desc.targets = &target_desc;
    desc.targetCount = 1;
    desc.searchPaths = &slang_folder_cstr;
    desc.searchPathCount = 1;

    Slang::Result r_session = g_slang.global_session->createSession(desc, session.writeRef());
    if (SLANG_FAILED(r_session))
    {
        printf("[S] failed to create the local session\n");
        return false;
    }

    printf("[S] successfully loaded the local session\n");
    printf("  search path: %s\n", slang_folder_cstr);
    return true;
}

slang::IModule* Slang_global_state::load_module(const char* name)
{
    Slang::ComPtr<slang::IBlob> diagnostics;

    slang::IModule* module = session->loadModule(name, diagnostics.writeRef());
    if (!module)
    {
        printf("[S] failed to load %s\n", name);
        printf("%s\n", (char *) diagnostics.get()->getBufferPointer());
        return nullptr;
    }

    printf("[S] found %s\n", name);

    return module;
}

bool Slang_global_state::erase_slang_modules()
{
    printf("[S] exploring slangified contents\n");
    for (auto f : std::filesystem::directory_iterator(slang_folder)) {
        std::string path = f.path().string();
        if (ends_with(path, "slang-module")) {
            std::filesystem::remove(path);
            printf("[S] erasing slang module: %s\n", path.c_str());
        }
    }

    printf("[S] done clearing previous modules\n");

    return true;
}

void Slang_global_state::write_module(slang::IModule* module, const char* path)
{
    if (!ENABLE_MODULES)
        return;

    Slang::ComPtr<slang::IBlob> blob;
    module->serialize(blob.writeRef());
    std::filesystem::path dst = slang_folder / path;
    std::ofstream fout(dst, std::ios::binary);
    printf("[S] writing to module: %s\n", dst.string().c_str());
    fout.write((const char *) blob->getBufferPointer(), blob->getBufferSize());
    fout.close();
}

bool Slang_global_state::load_modules()
{
    if (!ENABLE_MODULES)
        return true;

    module_runtime = load_module("runtime");
    module_common = load_module("common");
    module_types = load_module("types");
    module_lighting = load_module("lighting");

    write_module(module_runtime, "runtime.slang-module");
    write_module(module_common, "common.slang-module");
    write_module(module_types, "types.slang-module");
    write_module(module_lighting, "lighting.slang-module");

    return true
        && module_runtime
        && module_common
        && module_types
        && module_lighting;
}

bool Slang_global_state::load()
{
    if (!load_global_session()) return false;
    if (!load_session()) return false;
    if (!erase_slang_modules()) return false;
    if (!load_modules()) return false;
    return (loaded = true);
}

// ------------------------------------------------------------------------------------------------

bool Mdl_material_target::generate()
{
    if (!m_generation_required)
    {
        log_info("Target code does not need generation. Hash: " + m_compiled_material_hash, SRC);
        return true;
    }

    // since this method can be called from multiple threads simultaneously
    // a new context for is created
    mi::base::Handle<mi::neuraylib::IMdl_execution_context> context(m_sdk->create_context());

    // use shader caching if enabled
    Mdl_material_target_interface interface_data;
    
    // use the back-end to generate HLSL code
    mi::base::Handle<mi::neuraylib::ILink_unit> link_unit(nullptr);
	// in order to change the scene scale setting at runtime we need to preserve the conversions
	// in the generated code and expose the factor in the MDL material state of the shader.
	context->set_option("fold_meters_per_scene_unit", false);

	link_unit = m_sdk->get_backend().create_link_unit(m_sdk->get_transaction().get(), context.get());
	if (!m_sdk->log_messages("MDL creating a link unit failed.", context.get(), SRC))
		return false;

    // empty resource list (in case of reload) and rest the counter
    for (size_t i = 0, n = static_cast<size_t>(Mdl_resource_kind::_Count); i < n; ++i)
    {
        Mdl_resource_kind kind_i = static_cast<Mdl_resource_kind>(i);
        m_target_resources[kind_i].clear();
    }

    // add materials to link unit
    std::string process_hash;
    for (auto& pair : m_materials)
    {
        // add materials with the same hash only once
        const std::string& hash = pair.second->get_material_compiled_hash();
        if (process_hash.empty())
        {
            process_hash = hash;

            // add this material to the link unit
            if (!add_material_to_link_unit(interface_data, pair.second, link_unit.get(), context.get()))
            {
                log_error("Adding to link unit failed: " + pair.second->get_name(), SRC);
                return false;
            }
        }
        else
        {
            if (process_hash != hash)
            {
                log_error("Material added to the wrong target: " + pair.second->get_name(), SRC);
                return false;
            }
        }

        // pass target information to the material
        pair.second->set_target_interface(this, interface_data);
    }

    // generate HLSL code
	auto p = m_app->get_profiling().measure("generating HLSL (translate link unit)");
	m_target_code = m_sdk->get_backend().translate_link_unit(link_unit.get(), context.get());
	if (!m_sdk->log_messages("MDL target code generation failed.", context.get(), SRC))
		return false;

    // create a command list for uploading data to the GPU
    Command_queue* command_queue = m_app->get_command_queue(D3D12_COMMAND_LIST_TYPE_DIRECT);
    D3DCommandList* command_list = command_queue->get_command_list();

    // add all body textures, the ones that are required independent of the parameter set
    for (size_t i = 1, n = m_target_code->get_texture_count(); i < n; ++i)
    {
        if (!m_target_code->get_texture_is_body_resource(i))
            continue;

        Mdl_resource_assignment assignment(Mdl_resource_kind::Texture);
        assignment.resource_name = m_target_code->get_texture(i);
        assignment.runtime_resource_id = i;

        switch (m_target_code->get_texture_shape(i))
        {
            case mi::neuraylib::ITarget_code::Texture_shape_2d:
                assignment.dimension = Texture_dimension::Texture_2D;
                break;

            case mi::neuraylib::ITarget_code::Texture_shape_3d:
            case mi::neuraylib::ITarget_code::Texture_shape_bsdf_data:
                assignment.dimension = Texture_dimension::Texture_3D;
                break;

            default:
                log_error("Only 2D and 3D textures are supported by this example.", SRC);
                return false;
        }

        m_target_resources[Mdl_resource_kind::Texture].emplace_back(assignment);
    }

    // add all body light profiles
    for (size_t i = 1, n = m_target_code->get_light_profile_count(); i < n; ++i)
    {
        if (!m_target_code->get_light_profile_is_body_resource(i))
            continue;

        Mdl_resource_assignment assignment(Mdl_resource_kind::Light_profile);
        assignment.resource_name = m_target_code->get_light_profile(i);
        assignment.runtime_resource_id = i;
        m_target_resources[Mdl_resource_kind::Light_profile].emplace_back(assignment);
    }

    // add all body bsdf measurements
    for (size_t i = 1, n = m_target_code->get_bsdf_measurement_count(); i < n; ++i)
    {
        if (!m_target_code->get_bsdf_measurement_is_body_resource(i))
            continue;

        Mdl_resource_assignment assignment(Mdl_resource_kind::Bsdf_measurement);
        assignment.resource_name = m_target_code->get_bsdf_measurement(i);
        assignment.runtime_resource_id = i;
        m_target_resources[Mdl_resource_kind::Bsdf_measurement].emplace_back(assignment);
    }

    // add all string constants known to the link unit
    m_target_string_constants.clear();
    for (size_t i = 1, n = m_target_code->get_string_constant_count(); i < n; ++i)
    {
        Mdl_string_constant constant;
        constant.runtime_string_id = i;
        constant.value = m_target_code->get_string_constant(i);
        m_target_string_constants.push_back(constant);
    }

    // add TEXCOORD_0 to demonstrate renderer driven scene data elements
    // NOTE, if this is added manually, MDL code will not create any runtime function call
    // that with the 'scene_data_id'. Instead, only the render can call this outside of the
    // generated code.
    map_string_constant("TEXCOORD_0");

    // create per material resources, parameter bindings, ...
    // ------------------------------------------------------------

    // ... in parallel, if not forced otherwise
    std::vector<std::thread> tasks;
    std::atomic_bool success = true;
    for (auto mat : m_materials)
    {
        // sequentially
        if (m_app->get_options()->force_single_threading)
        {
            if (!mat.second->on_target_generated(command_list))
                success.store(false);
        }
        // asynchronously
        else
        {
            tasks.emplace_back(std::thread([&, mat]()
            {
                // no not fill command lists from different threads
                D3DCommandList* local_command_list = command_queue->get_command_list();
                if (!mat.second->on_target_generated(local_command_list))
                    success.store(false);
                command_queue->execute_command_list(local_command_list);
            }));
        }
    }

    // wait for all loading tasks
    for (auto &t : tasks)
        t.join();

    // any errors?
    if (!success.load())
    {
        log_error("On generate code callback return with failure.", SRC);
        return false;
    }

    // at this point, we know the number of resources in instances of the materials.
    // Since the root signature for all instances of the "same" material (probably different
    // parameter sets when using MDL class compilation) has to be identical, we go for the
    // maximum amount of occurring resources.
    for (size_t i = 0, n = static_cast<size_t>(Mdl_resource_kind::_Count); i < n; ++i)
        m_material_resource_count[static_cast<Mdl_resource_kind>(i)] = 0;

    visit_materials([&](const Mdl_material* mat)
    {
        for (size_t i = 0, n = static_cast<size_t>(Mdl_resource_kind::_Count); i < n; ++i)
        {
            Mdl_resource_kind kind_i = static_cast<Mdl_resource_kind>(i);
            size_t current = mat->get_resources(kind_i).size();
            m_material_resource_count[kind_i] = std::max(m_material_resource_count[kind_i], current);
        }
        return true;
    });

    // in order to load resources in parallel a continuous block of resource handles
    // for this target_code is allocated
    Descriptor_heap& resource_heap = *m_app->get_resource_descriptor_heap();
    size_t handle_count = 1; // read-only segment

    // if we already have a block on the resource heap (previous generation)
    // we try to reuse is if it fits
    if (m_first_resource_heap_handle.is_valid())
    {
        if (resource_heap.get_block_size(m_first_resource_heap_handle) < handle_count)
            resource_heap.free_views(m_first_resource_heap_handle); // free block
    }

    // reserve a new block of the required size and check if that was successful
    if (!m_first_resource_heap_handle.is_valid())
    {
        m_first_resource_heap_handle = resource_heap.reserve_views(handle_count);
        if (!m_first_resource_heap_handle.is_valid())
            return false;
    }

    // create per target resources
    // --------------------------------------

    // read-only data, all jit back-ends, including HLSL produce zero or one segments
    if (m_target_code->get_ro_data_segment_count() > 0)
    {
        size_t ro_data_seg_index = 0; // assuming one material per target code only
        const char* name = m_target_code->get_ro_data_segment_name(ro_data_seg_index);
        auto read_only_data_segment = new Buffer(
            m_app, m_target_code->get_ro_data_segment_size(ro_data_seg_index),
            "MDL_ReadOnly_" + std::string(name));

        read_only_data_segment->set_data(
            m_target_code->get_ro_data_segment_data(ro_data_seg_index),
            m_target_code->get_ro_data_segment_size(ro_data_seg_index));

        if (!m_read_only_data_segment) delete m_read_only_data_segment;
        m_read_only_data_segment = read_only_data_segment;
    }

    if(m_read_only_data_segment == nullptr)
    {
        m_read_only_data_segment = new Buffer(m_app, 4, "MDL_ReadOnly_nullptr");
        uint32_t zero(0);
        m_read_only_data_segment->set_data(&zero, 1);
    }

    // create resource view on the heap (at the first position of the target codes block)
    if (!resource_heap.create_shader_resource_view(
        m_read_only_data_segment, true, m_first_resource_heap_handle))
        return false;

    // copy data to the GPU
    if (m_read_only_data_segment && !m_read_only_data_segment->upload(command_list))
        return false;


    // prepare descriptor table for all per target resources
    // -------------------------------------------------------------------

    // note that the offset in the heap starts with zero
    // for each target we set 'target_heap_region_start' in the local root signature

    m_resource_descriptor_table.clear();

    // bind read-only data segment to shader
    m_resource_descriptor_table.register_srv(0, 2, 0);

    // generate the actual shader code with the help of some snippets
    m_hlsl_source_code.clear();

    // depending on the functions selected for code generation
    printf("[S] SURFACE SCATTERING       %2d (=1) \n", interface_data.has_surface_scattering);
    printf("[S] SURFACE EMISSION         %2d (=0) \n", interface_data.has_surface_emission);
    printf("[S] BACKFACE SCATTERING      %2d (=0) \n", interface_data.has_backface_scattering);
    printf("[S] BACKFACE EMISSION        %2d (=0) \n", interface_data.has_backface_emission);
    printf("[S] VOLUME ABSORPTION        %2d (=0) \n", interface_data.has_volume_absorption);
    printf("[S] THIN WALLED              %2d (=1) \n", interface_data.can_be_thin_walled);
    printf("[S] SCENE_DATA_ID_TEXCOORD0  %2d (=1) \n", map_string_constant("TEXCOORD_0"));
    printf("[S] MDL_NUM_TEXTURE_RESULTS  %2u (=32)\n", m_app->get_options()->texture_results_cache_size);

    // write to file for debugging purpose
    std::ofstream file_stream;

    // hlsl source code
    file_stream.open(mi::examples::io::get_executable_folder() + "/link_unit_code.hlsl");
    if (file_stream)
    {
        // Only for reference
        std::string hlsl_source_code;
		m_hlsl_source_code += "#include \"content/common.hlsl\"\n";
		m_hlsl_source_code += "#include \"content/mdl_target_code_types.hlsl\"\n";
		m_hlsl_source_code += "#include \"content/mdl_renderer_runtime.hlsl\"\n\n";
		m_hlsl_source_code += m_target_code->get_code();
		m_hlsl_source_code += "\n\n#include \"content/mdl_hit_programs.hlsl\"\n\n";

        file_stream << m_hlsl_source_code.c_str();
        file_stream.close();
    }
    
    // slang source code
    // TODO: compile ray generation and miss shaders with slangc as well...
    file_stream.open(mi::examples::io::get_executable_folder() + "/content/slangified/material.slang");
    if (file_stream)
    {
        std::string slang_source_code;
        // TODO: find methods and add public to them?
        // slang_source_code += "module material;\n";
        // slang_source_code += "\n";
        slang_source_code += "import runtime;\n";
        slang_source_code += "import types;\n";
        slang_source_code += "\n";
        slang_source_code += m_target_code->get_code();
        slang_source_code += "\n";
        slang_source_code += "#include \"hit.slang\"\n";
        slang_source_code += "\n";

        if (!interface_data.can_be_thin_walled)
            slang_source_code += "bool mdl_thin_walled(inout Shading_state_material state) { return true; }\n";

        file_stream << slang_source_code.c_str();
        file_stream.close();
    }

    command_queue->execute_command_list(command_list);

    m_generation_required = false;
    return true;
}

// ------------------------------------------------------------------------------------------------

const mi::neuraylib::ITarget_code* Mdl_material_target::get_generated_target() const
{
    if (!m_target_code)
        return nullptr;

    m_target_code->retain();
    return m_target_code.get();
}

// ------------------------------------------------------------------------------------------------

bool Mdl_material_target::compile()
{
    auto p = m_app->get_profiling().measure("compiling slang shaders (whole)");

    // Global slang state initialization
    if (!g_slang.loaded)
    {
        if (!g_slang.load())
            return false;
    }

    // Generate has be called first
    if (m_generation_required)
    {
        log_error("Compiling target code not possible before generation. Hash: " + m_compiled_material_hash, SRC);
        return false;
    }

    // Always clear the libraries
    m_dxil_compiled_libraries.clear();

    // Module with entry points
    Slang::ComPtr<slang::IBlob> diagnostics;
        
    // materials.slang: includes the raytracing entry point shaders
    slang::IModule* mdl_material = nullptr;
    mdl_material = g_slang.load_module("material");

    // Separate Shader_library objects for each entry point
    for (auto entry : { m_radiance_closest_hit_name,
                        m_radiance_any_hit_name,
                        m_shadow_any_hit_name }) {
        auto p = m_app->get_profiling().measure("compiling slang shaders for " + entry);

        // Find the entry point
        Slang::ComPtr<slang::IEntryPoint> entry_point;

        Slang::Result r_entry = mdl_material->findEntryPointByName(entry.c_str(), entry_point.writeRef());
        if (SLANG_FAILED(r_entry))
        {
            printf("[S] failed to find program: %s\n", entry.c_str());
            return false;
        }

        printf("[S] successfully found entry point: %s\n", entry.c_str());

        // Construct the linking group
        std::vector<slang::IComponentType*> components;
        components.push_back(entry_point);
        if (g_slang.ENABLE_MODULES) {
            components.push_back(g_slang.module_runtime);
            components.push_back(g_slang.module_common);
            components.push_back(g_slang.module_types);
            components.push_back(g_slang.module_lighting);
        }

        Slang::ComPtr<slang::IComponentType> group;
        Slang::Result r_composed = g_slang.session->createCompositeComponentType(
            components.data(), components.size(),
            group.writeRef(), diagnostics.writeRef());

        if (SLANG_FAILED(r_composed))
        {
            printf("[S] failed to construct composed program\n");
            return false;
        }

        printf("[S] successfully constructed composed program\n");

        // Link the entry point 
        Slang::ComPtr<slang::IComponentType> linked;
        Slang::Result r_linked = group->link(linked.writeRef(), diagnostics.writeRef());
        if (SLANG_FAILED(r_linked))
        {
            printf("[S] failed to link modules:\n");
            printf("%s\n", (char *) diagnostics.get()->getBufferPointer());
            return false;
        }

        printf("[S] successfully linked modules\n");

        // Generate the DXIL blob
        Slang::ComPtr<slang::IBlob> dxil_blob;
        Slang::Result r_target_code = linked->getTargetCode(0, dxil_blob.writeRef(), diagnostics.writeRef());
        if (SLANG_FAILED(r_target_code))
        {
            printf("[S] failed to get DXIL target code:\n");
            printf("%s\n", (char *) diagnostics.get()->getBufferPointer());
            return false;
        }

        printf("[S] successfully derived DXIL target code\n");

        // Add to list of libraries
        m_dxil_compiled_libraries.emplace_back((IDxcBlob*) dxil_blob.get(), std::vector <std::string> { entry });
    }

    m_compilation_required = false;

    return true;
}

}}} // mi::examples::mdl_d3d12
