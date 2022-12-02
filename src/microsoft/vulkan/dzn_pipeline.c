/*
 * Copyright © Microsoft Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include "dzn_private.h"

#include "spirv/nir_spirv.h"

#include "dxil_nir.h"
#include "nir_to_dxil.h"
#include "dxil_spirv_nir.h"
#include "spirv_to_dxil.h"

#include "dxil_validator.h"

#include "vk_alloc.h"
#include "vk_util.h"
#include "vk_format.h"

#include "util/u_debug.h"

#define D3D12_PIPELINE_STATE_STREAM_DESC_SIZE(__type) \
   ALIGN_POT(ALIGN_POT(sizeof(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE), alignof(__type)) + sizeof(__type), alignof(void *))

#define MAX_GFX_PIPELINE_STATE_STREAM_SIZE \
   D3D12_PIPELINE_STATE_STREAM_DESC_SIZE(ID3D12RootSignature *) + \
   (D3D12_PIPELINE_STATE_STREAM_DESC_SIZE(D3D12_SHADER_BYTECODE) * 5) + /* VS, PS, DS, HS, GS */ \
   D3D12_PIPELINE_STATE_STREAM_DESC_SIZE(D3D12_STREAM_OUTPUT_DESC) + \
   D3D12_PIPELINE_STATE_STREAM_DESC_SIZE(D3D12_BLEND_DESC) + \
   D3D12_PIPELINE_STATE_STREAM_DESC_SIZE(UINT) + /* SampleMask */ \
   D3D12_PIPELINE_STATE_STREAM_DESC_SIZE(D3D12_RASTERIZER_DESC) + \
   D3D12_PIPELINE_STATE_STREAM_DESC_SIZE(D3D12_INPUT_LAYOUT_DESC) + \
   D3D12_PIPELINE_STATE_STREAM_DESC_SIZE(D3D12_INDEX_BUFFER_STRIP_CUT_VALUE) + \
   D3D12_PIPELINE_STATE_STREAM_DESC_SIZE(D3D12_PRIMITIVE_TOPOLOGY_TYPE) + \
   D3D12_PIPELINE_STATE_STREAM_DESC_SIZE(struct D3D12_RT_FORMAT_ARRAY) + \
   D3D12_PIPELINE_STATE_STREAM_DESC_SIZE(DXGI_FORMAT) + /* DS format */ \
   D3D12_PIPELINE_STATE_STREAM_DESC_SIZE(DXGI_SAMPLE_DESC) + \
   D3D12_PIPELINE_STATE_STREAM_DESC_SIZE(D3D12_NODE_MASK) + \
   D3D12_PIPELINE_STATE_STREAM_DESC_SIZE(D3D12_CACHED_PIPELINE_STATE) + \
   D3D12_PIPELINE_STATE_STREAM_DESC_SIZE(D3D12_PIPELINE_STATE_FLAGS) + \
   D3D12_PIPELINE_STATE_STREAM_DESC_SIZE(D3D12_DEPTH_STENCIL_DESC1) + \
   D3D12_PIPELINE_STATE_STREAM_DESC_SIZE(D3D12_VIEW_INSTANCING_DESC)

#define MAX_COMPUTE_PIPELINE_STATE_STREAM_SIZE \
   D3D12_PIPELINE_STATE_STREAM_DESC_SIZE(ID3D12RootSignature *) + \
   D3D12_PIPELINE_STATE_STREAM_DESC_SIZE(D3D12_SHADER_BYTECODE)

#define d3d12_pipeline_state_stream_new_desc(__stream, __pipetype, __id, __type, __desc) \
   __type *__desc; \
   do { \
      struct { \
         D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type; \
         __type desc; \
      } *__wrapper; \
      (__stream)->SizeInBytes = ALIGN_POT((__stream)->SizeInBytes, alignof(void *)); \
      __wrapper = (void *)((uint8_t *)(__stream)->pPipelineStateSubobjectStream + (__stream)->SizeInBytes); \
      (__stream)->SizeInBytes += sizeof(*__wrapper); \
      assert((__stream)->SizeInBytes < MAX_ ## __pipetype ## _PIPELINE_STATE_STREAM_SIZE); \
      __wrapper->type = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ ## __id; \
      __desc = &__wrapper->desc; \
      memset(__desc, 0, sizeof(*__desc)); \
   } while (0)

#define d3d12_gfx_pipeline_state_stream_new_desc(__stream, __id, __type, __desc) \
   d3d12_pipeline_state_stream_new_desc(__stream, GFX, __id, __type, __desc)

#define d3d12_compute_pipeline_state_stream_new_desc(__stream, __id, __type, __desc) \
   d3d12_pipeline_state_stream_new_desc(__stream, COMPUTE, __id, __type, __desc)

static dxil_spirv_shader_stage
to_dxil_shader_stage(VkShaderStageFlagBits in)
{
   switch (in) {
   case VK_SHADER_STAGE_VERTEX_BIT: return DXIL_SPIRV_SHADER_VERTEX;
   case VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT: return DXIL_SPIRV_SHADER_TESS_CTRL;
   case VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT: return DXIL_SPIRV_SHADER_TESS_EVAL;
   case VK_SHADER_STAGE_GEOMETRY_BIT: return DXIL_SPIRV_SHADER_GEOMETRY;
   case VK_SHADER_STAGE_FRAGMENT_BIT: return DXIL_SPIRV_SHADER_FRAGMENT;
   case VK_SHADER_STAGE_COMPUTE_BIT: return DXIL_SPIRV_SHADER_COMPUTE;
   default: unreachable("Unsupported stage");
   }
}

static VkResult
dzn_pipeline_get_nir_shader(struct dzn_device *device,
                            const struct dzn_pipeline_layout *layout,
                            const VkPipelineShaderStageCreateInfo *stage_info,
                            gl_shader_stage stage,
                            enum dxil_spirv_yz_flip_mode yz_flip_mode,
                            uint16_t y_flip_mask, uint16_t z_flip_mask,
                            bool force_sample_rate_shading,
                            const nir_shader_compiler_options *nir_opts,
                            nir_shader **nir)
{
   struct dzn_instance *instance =
      container_of(device->vk.physical->instance, struct dzn_instance, vk);
   const VkSpecializationInfo *spec_info = stage_info->pSpecializationInfo;
   VK_FROM_HANDLE(vk_shader_module, module, stage_info->module);
   struct spirv_to_nir_options spirv_opts = {
      .caps = {
         .draw_parameters = true,
      },
      .ubo_addr_format = nir_address_format_32bit_index_offset,
      .ssbo_addr_format = nir_address_format_32bit_index_offset,
      .shared_addr_format = nir_address_format_32bit_offset_as_64bit,

      /* use_deref_buffer_array_length + nir_lower_explicit_io force
       * get_ssbo_size to take in the return from load_vulkan_descriptor
       * instead of vulkan_resource_index. This makes it much easier to
       * get the DXIL handle for the SSBO.
       */
      .use_deref_buffer_array_length = true
   };

   VkResult result =
      vk_shader_module_to_nir(&device->vk, module, stage,
                              stage_info->pName, stage_info->pSpecializationInfo,
                              &spirv_opts, nir_opts, NULL, nir);
   if (result != VK_SUCCESS)
      return result;

   struct dxil_spirv_runtime_conf conf = {
      .runtime_data_cbv = {
         .register_space = DZN_REGISTER_SPACE_SYSVALS,
         .base_shader_register = 0,
      },
      .push_constant_cbv = {
         .register_space = DZN_REGISTER_SPACE_PUSH_CONSTANT,
         .base_shader_register = 0,
      },
      .descriptor_set_count = layout->set_count,
      .descriptor_sets = layout->binding_translation,
      .zero_based_vertex_instance_id = false,
      .yz_flip = {
         .mode = yz_flip_mode,
         .y_mask = y_flip_mask,
         .z_mask = z_flip_mask,
      },
      .read_only_images_as_srvs = true,
      .force_sample_rate_shading = force_sample_rate_shading,
   };

   bool requires_runtime_data;
   dxil_spirv_nir_passes(*nir, &conf, &requires_runtime_data);
   return VK_SUCCESS;
}

static VkResult
dzn_pipeline_compile_shader(struct dzn_device *device,
                            nir_shader *nir,
                            D3D12_SHADER_BYTECODE *slot)
{
   struct dzn_instance *instance =
      container_of(device->vk.physical->instance, struct dzn_instance, vk);
   struct nir_to_dxil_options opts = {
      .environment = DXIL_ENVIRONMENT_VULKAN,
   };
   struct blob dxil_blob;
   VkResult result = VK_SUCCESS;

   if (instance->debug_flags & DZN_DEBUG_NIR)
      nir_print_shader(nir, stderr);

   if (nir_to_dxil(nir, &opts, &dxil_blob)) {
      blob_finish_get_buffer(&dxil_blob, (void **)&slot->pShaderBytecode,
                             &slot->BytecodeLength);
   } else {
      result = vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
   }

   if (dxil_blob.allocated)
      blob_finish(&dxil_blob);

   if (result != VK_SUCCESS)
      return result;

   char *err;
   bool res = dxil_validate_module(instance->dxil_validator,
                                   (void *)slot->pShaderBytecode,
                                   slot->BytecodeLength, &err);

   if (instance->debug_flags & DZN_DEBUG_DXIL) {
      char *disasm = dxil_disasm_module(instance->dxil_validator,
                                        (void *)slot->pShaderBytecode,
                                        slot->BytecodeLength);
      if (disasm) {
         fprintf(stderr,
                 "== BEGIN SHADER ============================================\n"
                 "%s\n"
                 "== END SHADER ==============================================\n",
                  disasm);
         ralloc_free(disasm);
      }
   }

   if (!res) {
      if (err) {
         fprintf(stderr,
               "== VALIDATION ERROR =============================================\n"
               "%s\n"
               "== END ==========================================================\n",
               err);
         ralloc_free(err);
      }
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
   }

   return VK_SUCCESS;
}

static D3D12_SHADER_BYTECODE *
dzn_pipeline_get_gfx_shader_slot(D3D12_PIPELINE_STATE_STREAM_DESC *stream,
                                 gl_shader_stage in)
{
   switch (in) {
   case MESA_SHADER_VERTEX: {
      d3d12_gfx_pipeline_state_stream_new_desc(stream, VS, D3D12_SHADER_BYTECODE, desc);
      return desc;
   }
   case MESA_SHADER_TESS_CTRL: {
      d3d12_gfx_pipeline_state_stream_new_desc(stream, DS, D3D12_SHADER_BYTECODE, desc);
      return desc;
   }
   case MESA_SHADER_TESS_EVAL: {
      d3d12_gfx_pipeline_state_stream_new_desc(stream, HS, D3D12_SHADER_BYTECODE, desc);
      return desc;
   }
   case MESA_SHADER_GEOMETRY: {
      d3d12_gfx_pipeline_state_stream_new_desc(stream, GS, D3D12_SHADER_BYTECODE, desc);
      return desc;
   }
   case MESA_SHADER_FRAGMENT: {
      d3d12_gfx_pipeline_state_stream_new_desc(stream, PS, D3D12_SHADER_BYTECODE, desc);
      return desc;
   }
   default: unreachable("Unsupported stage");
   }
}

static VkResult
dzn_graphics_pipeline_compile_shaders(struct dzn_device *device,
                                      const struct dzn_graphics_pipeline *pipeline,
                                      const struct dzn_pipeline_layout *layout,
                                      D3D12_PIPELINE_STATE_STREAM_DESC *out,
                                      D3D12_INPUT_ELEMENT_DESC *attribs,
                                      D3D12_INPUT_ELEMENT_DESC *inputs,
                                      D3D12_SHADER_BYTECODE **shaders,
                                      const VkGraphicsPipelineCreateInfo *info)
{
   const VkPipelineViewportStateCreateInfo *vp_info =
      info->pRasterizationState->rasterizerDiscardEnable ?
      NULL : info->pViewportState;
   struct {
      const VkPipelineShaderStageCreateInfo *info;
      nir_shader *nir;
   } stages[MESA_VULKAN_SHADER_STAGES] = { 0 };
   gl_shader_stage yz_flip_stage = MESA_SHADER_NONE;
   uint32_t active_stage_mask = 0;
   VkResult ret = VK_SUCCESS;

   /* First step: collect stage info in a table indexed by gl_shader_stage
    * so we can iterate over stages in pipeline order or reverse pipeline
    * order.
    */
   for (uint32_t i = 0; i < info->stageCount; i++) {
      gl_shader_stage stage;

      switch (info->pStages[i].stage) {
      case VK_SHADER_STAGE_VERTEX_BIT:
         stage = MESA_SHADER_VERTEX;
         if (yz_flip_stage < stage)
            yz_flip_stage = stage;
         break;
      case VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT:
         stage = MESA_SHADER_TESS_CTRL;
         break;
      case VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT:
         stage = MESA_SHADER_TESS_EVAL;
         if (yz_flip_stage < stage)
            yz_flip_stage = stage;
         break;
      case VK_SHADER_STAGE_GEOMETRY_BIT:
         stage = MESA_SHADER_GEOMETRY;
         if (yz_flip_stage < stage)
            yz_flip_stage = stage;
         break;
      case VK_SHADER_STAGE_FRAGMENT_BIT:
         stage = MESA_SHADER_FRAGMENT;
         break;
      default:
         unreachable("Unsupported stage");
      }

      if (stage == MESA_SHADER_FRAGMENT &&
          info->pRasterizationState &&
          (info->pRasterizationState->rasterizerDiscardEnable ||
           info->pRasterizationState->cullMode == VK_CULL_MODE_FRONT_AND_BACK)) {
         /* Disable rasterization (AKA leave fragment shader NULL) when
          * front+back culling or discard is set.
          */
         continue;
      }

      stages[stage].info = &info->pStages[i];
      active_stage_mask |= BITFIELD_BIT(stage);
   }

   /* Second step: get NIR shaders for all stages. */
   nir_shader_compiler_options nir_opts = *dxil_get_nir_compiler_options();
   nir_opts.lower_base_vertex = true;
   u_foreach_bit(stage, active_stage_mask) {
      enum dxil_spirv_yz_flip_mode yz_flip_mode = DXIL_SPIRV_YZ_FLIP_NONE;
      uint16_t y_flip_mask = 0, z_flip_mask = 0;

      if (stage == yz_flip_stage) {
         if (pipeline->vp.dynamic) {
            yz_flip_mode = DXIL_SPIRV_YZ_FLIP_CONDITIONAL;
         } else if (vp_info) {
            for (uint32_t i = 0; vp_info->pViewports && i < vp_info->viewportCount; i++) {
               if (vp_info->pViewports[i].height > 0)
                  y_flip_mask |= BITFIELD_BIT(i);

               if (vp_info->pViewports[i].minDepth > vp_info->pViewports[i].maxDepth)
                  z_flip_mask |= BITFIELD_BIT(i);
            }

            if (y_flip_mask && z_flip_mask)
               yz_flip_mode = DXIL_SPIRV_YZ_FLIP_UNCONDITIONAL;
            else if (z_flip_mask)
               yz_flip_mode = DXIL_SPIRV_Z_FLIP_UNCONDITIONAL;
            else if (y_flip_mask)
               yz_flip_mode = DXIL_SPIRV_Y_FLIP_UNCONDITIONAL;
         }
      }

      bool force_sample_rate_shading =
         stages[stage].info->stage == VK_SHADER_STAGE_FRAGMENT_BIT &&
         info->pMultisampleState &&
         info->pMultisampleState->sampleShadingEnable;

      ret = dzn_pipeline_get_nir_shader(device, layout,
                                        stages[stage].info, stage,
                                        yz_flip_mode, y_flip_mask, z_flip_mask,
                                        force_sample_rate_shading,
                                        &nir_opts, &stages[stage].nir);
      if (ret != VK_SUCCESS)
         goto out;
   }

   /* Third step: link those NIR shaders. We iterate in reverse order
    * so we can eliminate outputs that are never read by the next stage.
    */
   uint32_t link_mask = active_stage_mask;
   while (link_mask != 0) {
      gl_shader_stage stage = util_last_bit(link_mask) - 1;
      link_mask &= ~BITFIELD_BIT(stage);
      gl_shader_stage prev_stage = util_last_bit(link_mask) - 1;

      assert(stages[stage].nir);
      dxil_spirv_nir_link(stages[stage].nir,
                          prev_stage != MESA_SHADER_NONE ?
                          stages[prev_stage].nir : NULL);
   } while (link_mask != 0);

   if (stages[MESA_SHADER_VERTEX].nir) {
      /* Now, declare one D3D12_INPUT_ELEMENT_DESC per VS input variable, so
       * we can handle location overlaps properly.
       */
      unsigned drv_loc = 0;
      nir_foreach_shader_in_variable(var, stages[MESA_SHADER_VERTEX].nir) {
         assert(var->data.location >= VERT_ATTRIB_GENERIC0);
         unsigned loc = var->data.location - VERT_ATTRIB_GENERIC0;
         assert(drv_loc < D3D12_VS_INPUT_REGISTER_COUNT);
         assert(loc < MAX_VERTEX_GENERIC_ATTRIBS);

	 inputs[drv_loc] = attribs[loc];
         inputs[drv_loc].SemanticIndex = drv_loc;
         var->data.driver_location = drv_loc++;
      }

      if (drv_loc > 0) {
         d3d12_gfx_pipeline_state_stream_new_desc(out, INPUT_LAYOUT, D3D12_INPUT_LAYOUT_DESC, desc);
         desc->pInputElementDescs = inputs;
         desc->NumElements = drv_loc;
      }
   }

   /* Last step: translate NIR shaders into DXIL modules */
   u_foreach_bit(stage, active_stage_mask) {
      D3D12_SHADER_BYTECODE *slot =
         dzn_pipeline_get_gfx_shader_slot(out, stage);

      ret = dzn_pipeline_compile_shader(device, stages[stage].nir, slot);
      if (ret != VK_SUCCESS)
         goto out;

      shaders[stage] = slot;
   }

out:
   u_foreach_bit(stage, active_stage_mask)
      ralloc_free(stages[stage].nir);

   return ret;
}

static VkResult
dzn_graphics_pipeline_translate_vi(struct dzn_graphics_pipeline *pipeline,
                                   const VkAllocationCallbacks *alloc,
                                   const VkGraphicsPipelineCreateInfo *in,
                                   D3D12_INPUT_ELEMENT_DESC *inputs)
{
   struct dzn_device *device =
      container_of(pipeline->base.base.device, struct dzn_device, vk);
   const VkPipelineVertexInputStateCreateInfo *in_vi =
      in->pVertexInputState;
   const VkPipelineVertexInputDivisorStateCreateInfoEXT *divisors =
      (const VkPipelineVertexInputDivisorStateCreateInfoEXT *)
      vk_find_struct_const(in_vi, PIPELINE_VERTEX_INPUT_DIVISOR_STATE_CREATE_INFO_EXT);

   if (!in_vi->vertexAttributeDescriptionCount)
      return VK_SUCCESS;

   D3D12_INPUT_CLASSIFICATION slot_class[MAX_VBS];

   pipeline->vb.count = 0;
   for (uint32_t i = 0; i < in_vi->vertexBindingDescriptionCount; i++) {
      const struct VkVertexInputBindingDescription *bdesc =
         &in_vi->pVertexBindingDescriptions[i];

      pipeline->vb.count = MAX2(pipeline->vb.count, bdesc->binding + 1);
      pipeline->vb.strides[bdesc->binding] = bdesc->stride;
      if (bdesc->inputRate == VK_VERTEX_INPUT_RATE_INSTANCE) {
         slot_class[bdesc->binding] = D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA;
      } else {
         assert(bdesc->inputRate == VK_VERTEX_INPUT_RATE_VERTEX);
         slot_class[bdesc->binding] = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
      }
   }

   for (uint32_t i = 0; i < in_vi->vertexAttributeDescriptionCount; i++) {
      const VkVertexInputAttributeDescription *attr =
         &in_vi->pVertexAttributeDescriptions[i];
      const VkVertexInputBindingDivisorDescriptionEXT *divisor = NULL;

      if (slot_class[attr->binding] == D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA &&
          divisors) {
         for (uint32_t d = 0; d < divisors->vertexBindingDivisorCount; d++) {
            if (attr->binding == divisors->pVertexBindingDivisors[d].binding) {
               divisor = &divisors->pVertexBindingDivisors[d];
               break;
            }
         }
      }

      /* nir_to_dxil() name all vertex inputs as TEXCOORDx */
      inputs[attr->location] = (D3D12_INPUT_ELEMENT_DESC) {
         .SemanticName = "TEXCOORD",
         .Format = dzn_buffer_get_dxgi_format(attr->format),
         .InputSlot = attr->binding,
         .InputSlotClass = slot_class[attr->binding],
         .InstanceDataStepRate =
            divisor ? divisor->divisor :
            slot_class[attr->binding] == D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA ? 1 : 0,
         .AlignedByteOffset = attr->offset,
      };
   }

   return VK_SUCCESS;
}

static D3D12_PRIMITIVE_TOPOLOGY_TYPE
to_prim_topology_type(VkPrimitiveTopology in)
{
   switch (in) {
   case VK_PRIMITIVE_TOPOLOGY_POINT_LIST:
      return D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
   case VK_PRIMITIVE_TOPOLOGY_LINE_LIST:
   case VK_PRIMITIVE_TOPOLOGY_LINE_STRIP:
   case VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY:
   case VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY:
      return D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
   case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST:
   case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP:
   case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN:
   case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST_WITH_ADJACENCY:
   case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP_WITH_ADJACENCY:
      return D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
   case VK_PRIMITIVE_TOPOLOGY_PATCH_LIST:
      return D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH;
   default: unreachable("Invalid primitive topology");
   }
}

static D3D12_PRIMITIVE_TOPOLOGY
to_prim_topology(VkPrimitiveTopology in, unsigned patch_control_points)
{
   switch (in) {
   case VK_PRIMITIVE_TOPOLOGY_POINT_LIST: return D3D_PRIMITIVE_TOPOLOGY_POINTLIST;
   case VK_PRIMITIVE_TOPOLOGY_LINE_LIST: return D3D_PRIMITIVE_TOPOLOGY_LINELIST;
   case VK_PRIMITIVE_TOPOLOGY_LINE_STRIP: return D3D_PRIMITIVE_TOPOLOGY_LINESTRIP;
   case VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY: return D3D_PRIMITIVE_TOPOLOGY_LINELIST_ADJ;
   case VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY: return D3D_PRIMITIVE_TOPOLOGY_LINESTRIP_ADJ;
   case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST: return D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
   case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP: return D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
   /* Triangle fans are emulated using an intermediate index buffer. */
   case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN: return D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
   case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST_WITH_ADJACENCY: return D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST_ADJ;
   case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP_WITH_ADJACENCY: return D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP_ADJ;
   case VK_PRIMITIVE_TOPOLOGY_PATCH_LIST:
      assert(patch_control_points);
      return (D3D12_PRIMITIVE_TOPOLOGY)(D3D_PRIMITIVE_TOPOLOGY_1_CONTROL_POINT_PATCHLIST + patch_control_points - 1);
   default: unreachable("Invalid primitive topology");
   }
}

static void
dzn_graphics_pipeline_translate_ia(struct dzn_graphics_pipeline *pipeline,
                                   D3D12_PIPELINE_STATE_STREAM_DESC *out,
                                   const VkGraphicsPipelineCreateInfo *in)
{
   const VkPipelineInputAssemblyStateCreateInfo *in_ia =
      in->pInputAssemblyState;
   bool has_tes = false;
   for (uint32_t i = 0; i < in->stageCount; i++) {
      if (in->pStages[i].stage == VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT ||
          in->pStages[i].stage == VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT) {
         has_tes = true;
         break;
      }
   }
   const VkPipelineTessellationStateCreateInfo *in_tes =
      has_tes ? in->pTessellationState : NULL;

   d3d12_gfx_pipeline_state_stream_new_desc(out, PRIMITIVE_TOPOLOGY, D3D12_PRIMITIVE_TOPOLOGY_TYPE, prim_top_type);
   *prim_top_type = to_prim_topology_type(in_ia->topology);
   pipeline->ia.triangle_fan = in_ia->topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
   pipeline->ia.topology =
      to_prim_topology(in_ia->topology, in_tes ? in_tes->patchControlPoints : 0);

   /* FIXME: does that work for u16 index buffers? */
   d3d12_gfx_pipeline_state_stream_new_desc(out, IB_STRIP_CUT_VALUE, D3D12_INDEX_BUFFER_STRIP_CUT_VALUE, ib_strip_cut);
   if (in_ia->primitiveRestartEnable)
      *ib_strip_cut = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFFFFFF;
   else
      *ib_strip_cut = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED;
}

static D3D12_FILL_MODE
translate_polygon_mode(VkPolygonMode in)
{
   switch (in) {
   case VK_POLYGON_MODE_FILL: return D3D12_FILL_MODE_SOLID;
   case VK_POLYGON_MODE_LINE: return D3D12_FILL_MODE_WIREFRAME;
   default: unreachable("Unsupported polygon mode");
   }
}

static D3D12_CULL_MODE
translate_cull_mode(VkCullModeFlags in)
{
   switch (in) {
   case VK_CULL_MODE_NONE: return D3D12_CULL_MODE_NONE;
   case VK_CULL_MODE_FRONT_BIT: return D3D12_CULL_MODE_FRONT;
   case VK_CULL_MODE_BACK_BIT: return D3D12_CULL_MODE_BACK;
   /* Front+back face culling is equivalent to 'rasterization disabled' */
   case VK_CULL_MODE_FRONT_AND_BACK: return D3D12_CULL_MODE_NONE;
   default: unreachable("Unsupported cull mode");
   }
}

static void
dzn_graphics_pipeline_translate_rast(struct dzn_graphics_pipeline *pipeline,
                                     D3D12_PIPELINE_STATE_STREAM_DESC *out,
                                     const VkGraphicsPipelineCreateInfo *in)
{
   const VkPipelineRasterizationStateCreateInfo *in_rast =
      in->pRasterizationState;
   const VkPipelineViewportStateCreateInfo *in_vp =
      in_rast->rasterizerDiscardEnable ? NULL : in->pViewportState;

   if (in_vp) {
      pipeline->vp.count = in_vp->viewportCount;
      if (in_vp->pViewports) {
         for (uint32_t i = 0; in_vp->pViewports && i < in_vp->viewportCount; i++)
            dzn_translate_viewport(&pipeline->vp.desc[i], &in_vp->pViewports[i]);
      }

      pipeline->scissor.count = in_vp->scissorCount;
      if (in_vp->pScissors) {
         for (uint32_t i = 0; i < in_vp->scissorCount; i++)
            dzn_translate_rect(&pipeline->scissor.desc[i], &in_vp->pScissors[i]);
      }
   }

   d3d12_gfx_pipeline_state_stream_new_desc(out, RASTERIZER, D3D12_RASTERIZER_DESC, desc);
   desc->DepthClipEnable = !in_rast->depthClampEnable;
   desc->FillMode = translate_polygon_mode(in_rast->polygonMode);
   desc->CullMode = translate_cull_mode(in_rast->cullMode);
   desc->FrontCounterClockwise =
      in_rast->frontFace == VK_FRONT_FACE_COUNTER_CLOCKWISE;
   if (in_rast->depthBiasEnable) {
      desc->DepthBias = in_rast->depthBiasConstantFactor;
      desc->SlopeScaledDepthBias = in_rast->depthBiasSlopeFactor;
      desc->DepthBiasClamp = in_rast->depthBiasClamp;
   }

   assert(in_rast->lineWidth == 1.0f);
}

static void
dzn_graphics_pipeline_translate_ms(struct dzn_graphics_pipeline *pipeline,
                                   D3D12_PIPELINE_STATE_STREAM_DESC *out,
                                   const VkGraphicsPipelineCreateInfo *in)
{
   const VkPipelineRasterizationStateCreateInfo *in_rast =
      in->pRasterizationState;
   const VkPipelineMultisampleStateCreateInfo *in_ms =
      in_rast->rasterizerDiscardEnable ? NULL : in->pMultisampleState;

   if (!in_ms)
      return;

   /* TODO: minSampleShading (use VRS), alphaToOneEnable */
   d3d12_gfx_pipeline_state_stream_new_desc(out, SAMPLE_DESC, DXGI_SAMPLE_DESC, desc);
   desc->Count = in_ms ? in_ms->rasterizationSamples : 1;
   desc->Quality = 0;

   if (!in_ms->pSampleMask)
      return;

   d3d12_gfx_pipeline_state_stream_new_desc(out, SAMPLE_MASK, UINT, mask);
   *mask = *in_ms->pSampleMask;
}

static D3D12_STENCIL_OP
translate_stencil_op(VkStencilOp in)
{
   switch (in) {
   case VK_STENCIL_OP_KEEP: return D3D12_STENCIL_OP_KEEP;
   case VK_STENCIL_OP_ZERO: return D3D12_STENCIL_OP_ZERO;
   case VK_STENCIL_OP_REPLACE: return D3D12_STENCIL_OP_REPLACE;
   case VK_STENCIL_OP_INCREMENT_AND_CLAMP: return D3D12_STENCIL_OP_INCR_SAT;
   case VK_STENCIL_OP_DECREMENT_AND_CLAMP: return D3D12_STENCIL_OP_DECR_SAT;
   case VK_STENCIL_OP_INCREMENT_AND_WRAP: return D3D12_STENCIL_OP_INCR;
   case VK_STENCIL_OP_DECREMENT_AND_WRAP: return D3D12_STENCIL_OP_DECR;
   case VK_STENCIL_OP_INVERT: return D3D12_STENCIL_OP_INVERT;
   default: unreachable("Invalid stencil op");
   }
}

static void
translate_stencil_test(struct dzn_graphics_pipeline *pipeline,
                       D3D12_DEPTH_STENCIL_DESC1 *out,
                       const VkGraphicsPipelineCreateInfo *in)
{
   const VkPipelineDepthStencilStateCreateInfo *in_zsa =
      in->pDepthStencilState;

   bool front_test_uses_ref =
      !(in->pRasterizationState->cullMode & VK_CULL_MODE_FRONT_BIT) &&
      in_zsa->front.compareOp != VK_COMPARE_OP_NEVER &&
      in_zsa->front.compareOp != VK_COMPARE_OP_ALWAYS &&
      (pipeline->zsa.stencil_test.dynamic_compare_mask ||
       in_zsa->front.compareMask != 0);
   bool back_test_uses_ref =
      !(in->pRasterizationState->cullMode & VK_CULL_MODE_BACK_BIT) &&
      in_zsa->back.compareOp != VK_COMPARE_OP_NEVER &&
      in_zsa->back.compareOp != VK_COMPARE_OP_ALWAYS &&
      (pipeline->zsa.stencil_test.dynamic_compare_mask ||
       in_zsa->back.compareMask != 0);

   if (front_test_uses_ref && pipeline->zsa.stencil_test.dynamic_compare_mask)
      pipeline->zsa.stencil_test.front.compare_mask = UINT32_MAX;
   else if (front_test_uses_ref)
      pipeline->zsa.stencil_test.front.compare_mask = in_zsa->front.compareMask;
   else
      pipeline->zsa.stencil_test.front.compare_mask = 0;

   if (back_test_uses_ref && pipeline->zsa.stencil_test.dynamic_compare_mask)
      pipeline->zsa.stencil_test.back.compare_mask = UINT32_MAX;
   else if (back_test_uses_ref)
      pipeline->zsa.stencil_test.back.compare_mask = in_zsa->back.compareMask;
   else
      pipeline->zsa.stencil_test.back.compare_mask = 0;

   bool diff_wr_mask =
      in->pRasterizationState->cullMode == VK_CULL_MODE_NONE &&
      (pipeline->zsa.stencil_test.dynamic_write_mask ||
       in_zsa->back.writeMask != in_zsa->front.writeMask);
   bool diff_ref =
      in->pRasterizationState->cullMode == VK_CULL_MODE_NONE &&
      (pipeline->zsa.stencil_test.dynamic_ref ||
       in_zsa->back.reference != in_zsa->front.reference);
   bool diff_cmp_mask =
      back_test_uses_ref && front_test_uses_ref &&
      (pipeline->zsa.stencil_test.dynamic_compare_mask ||
       pipeline->zsa.stencil_test.front.compare_mask != pipeline->zsa.stencil_test.back.compare_mask);

   if (diff_cmp_mask || diff_wr_mask)
      pipeline->zsa.stencil_test.independent_front_back = true;

   bool back_wr_uses_ref =
      !(in->pRasterizationState->cullMode & VK_CULL_MODE_BACK_BIT) &&
      (in_zsa->back.compareOp != VK_COMPARE_OP_ALWAYS &&
       in_zsa->back.failOp == VK_STENCIL_OP_REPLACE) ||
      (in_zsa->back.compareOp != VK_COMPARE_OP_NEVER &&
       (!in_zsa->depthTestEnable || in_zsa->depthCompareOp != VK_COMPARE_OP_NEVER) &&
       in_zsa->back.passOp == VK_STENCIL_OP_REPLACE) ||
      (in_zsa->depthTestEnable &&
       in_zsa->depthCompareOp != VK_COMPARE_OP_ALWAYS &&
       in_zsa->back.depthFailOp == VK_STENCIL_OP_REPLACE);
   bool front_wr_uses_ref =
      !(in->pRasterizationState->cullMode & VK_CULL_MODE_FRONT_BIT) &&
      (in_zsa->front.compareOp != VK_COMPARE_OP_ALWAYS &&
       in_zsa->front.failOp == VK_STENCIL_OP_REPLACE) ||
      (in_zsa->front.compareOp != VK_COMPARE_OP_NEVER &&
       (!in_zsa->depthTestEnable || in_zsa->depthCompareOp != VK_COMPARE_OP_NEVER) &&
       in_zsa->front.passOp == VK_STENCIL_OP_REPLACE) ||
      (in_zsa->depthTestEnable &&
       in_zsa->depthCompareOp != VK_COMPARE_OP_ALWAYS &&
       in_zsa->front.depthFailOp == VK_STENCIL_OP_REPLACE);

   pipeline->zsa.stencil_test.front.write_mask =
      (pipeline->zsa.stencil_test.dynamic_write_mask ||
       (in->pRasterizationState->cullMode & VK_CULL_MODE_FRONT_BIT)) ?
      0 : in_zsa->front.writeMask;
   pipeline->zsa.stencil_test.back.write_mask =
      (pipeline->zsa.stencil_test.dynamic_write_mask ||
       (in->pRasterizationState->cullMode & VK_CULL_MODE_BACK_BIT)) ?
      0 : in_zsa->back.writeMask;

   pipeline->zsa.stencil_test.front.uses_ref = front_test_uses_ref || front_wr_uses_ref;
   pipeline->zsa.stencil_test.back.uses_ref = back_test_uses_ref || back_wr_uses_ref;

   if (diff_ref &&
       pipeline->zsa.stencil_test.front.uses_ref &&
       pipeline->zsa.stencil_test.back.uses_ref)
      pipeline->zsa.stencil_test.independent_front_back = true;

   pipeline->zsa.stencil_test.front.ref =
      pipeline->zsa.stencil_test.dynamic_ref ? 0 : in_zsa->front.reference;
   pipeline->zsa.stencil_test.back.ref =
      pipeline->zsa.stencil_test.dynamic_ref ? 0 : in_zsa->back.reference;

   /* FIXME: We don't support independent {compare,write}_mask and stencil
    * reference. Until we have proper support for independent front/back
    * stencil test, let's prioritize the front setup when both are active.
    */
   out->StencilReadMask =
      front_test_uses_ref ?
      pipeline->zsa.stencil_test.front.compare_mask :
      back_test_uses_ref ?
      pipeline->zsa.stencil_test.back.compare_mask : 0;
   out->StencilWriteMask =
      pipeline->zsa.stencil_test.front.write_mask ?
      pipeline->zsa.stencil_test.front.write_mask :
      pipeline->zsa.stencil_test.back.write_mask;

   assert(!pipeline->zsa.stencil_test.independent_front_back);
}

static void
dzn_graphics_pipeline_translate_zsa(struct dzn_graphics_pipeline *pipeline,
                                    D3D12_PIPELINE_STATE_STREAM_DESC *out,
                                    const VkGraphicsPipelineCreateInfo *in)
{
   const VkPipelineRasterizationStateCreateInfo *in_rast =
      in->pRasterizationState;
   const VkPipelineDepthStencilStateCreateInfo *in_zsa =
      in_rast->rasterizerDiscardEnable ? NULL : in->pDepthStencilState;

   if (!in_zsa)
      return;

   d3d12_gfx_pipeline_state_stream_new_desc(out, DEPTH_STENCIL1, D3D12_DEPTH_STENCIL_DESC1, desc);

   desc->DepthEnable = in_zsa->depthTestEnable;
   desc->DepthWriteMask =
      in_zsa->depthWriteEnable ?
      D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
   desc->DepthFunc =
      dzn_translate_compare_op(in_zsa->depthCompareOp);
   pipeline->zsa.depth_bounds.enable = in_zsa->depthBoundsTestEnable;
   pipeline->zsa.depth_bounds.min = in_zsa->minDepthBounds;
   pipeline->zsa.depth_bounds.max = in_zsa->maxDepthBounds;
   desc->DepthBoundsTestEnable = in_zsa->depthBoundsTestEnable;
   desc->StencilEnable = in_zsa->stencilTestEnable;
   if (in_zsa->stencilTestEnable) {
      desc->FrontFace.StencilFailOp =
        translate_stencil_op(in_zsa->front.failOp);
      desc->FrontFace.StencilDepthFailOp =
        translate_stencil_op(in_zsa->front.depthFailOp);
      desc->FrontFace.StencilPassOp =
        translate_stencil_op(in_zsa->front.passOp);
      desc->FrontFace.StencilFunc =
        dzn_translate_compare_op(in_zsa->front.compareOp);
      desc->BackFace.StencilFailOp =
        translate_stencil_op(in_zsa->back.failOp);
      desc->BackFace.StencilDepthFailOp =
        translate_stencil_op(in_zsa->back.depthFailOp);
      desc->BackFace.StencilPassOp =
        translate_stencil_op(in_zsa->back.passOp);
      desc->BackFace.StencilFunc =
        dzn_translate_compare_op(in_zsa->back.compareOp);

      pipeline->zsa.stencil_test.enable = true;

      translate_stencil_test(pipeline, desc, in);
   }
}

static D3D12_BLEND
translate_blend_factor(VkBlendFactor in, bool is_alpha)
{
   switch (in) {
   case VK_BLEND_FACTOR_ZERO: return D3D12_BLEND_ZERO;
   case VK_BLEND_FACTOR_ONE: return D3D12_BLEND_ONE;
   case VK_BLEND_FACTOR_SRC_COLOR:
      return is_alpha ? D3D12_BLEND_SRC_ALPHA : D3D12_BLEND_SRC_COLOR;
   case VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR:
      return is_alpha ? D3D12_BLEND_INV_SRC_ALPHA : D3D12_BLEND_INV_SRC_COLOR;
   case VK_BLEND_FACTOR_DST_COLOR:
      return is_alpha ? D3D12_BLEND_DEST_ALPHA : D3D12_BLEND_DEST_COLOR;
   case VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR:
      return is_alpha ? D3D12_BLEND_INV_DEST_ALPHA : D3D12_BLEND_INV_DEST_COLOR;
   case VK_BLEND_FACTOR_SRC_ALPHA: return D3D12_BLEND_SRC_ALPHA;
   case VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA: return D3D12_BLEND_INV_SRC_ALPHA;
   case VK_BLEND_FACTOR_DST_ALPHA: return D3D12_BLEND_DEST_ALPHA;
   case VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA: return D3D12_BLEND_INV_DEST_ALPHA;
   /* FIXME: no way to isolate the alpla and color constants */
   case VK_BLEND_FACTOR_CONSTANT_COLOR:
   case VK_BLEND_FACTOR_CONSTANT_ALPHA:
      return D3D12_BLEND_BLEND_FACTOR;
   case VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR:
   case VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA:
      return D3D12_BLEND_INV_BLEND_FACTOR;
   case VK_BLEND_FACTOR_SRC1_COLOR:
      return is_alpha ? D3D12_BLEND_SRC1_ALPHA : D3D12_BLEND_SRC1_COLOR;
   case VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR:
      return is_alpha ? D3D12_BLEND_INV_SRC1_ALPHA : D3D12_BLEND_INV_SRC1_COLOR;
   case VK_BLEND_FACTOR_SRC1_ALPHA: return D3D12_BLEND_SRC1_ALPHA;
   case VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA: return D3D12_BLEND_INV_SRC1_ALPHA;
   case VK_BLEND_FACTOR_SRC_ALPHA_SATURATE: return D3D12_BLEND_SRC_ALPHA_SAT;
   default: unreachable("Invalid blend factor");
   }
}

static D3D12_BLEND_OP
translate_blend_op(VkBlendOp in)
{
   switch (in) {
   case VK_BLEND_OP_ADD: return D3D12_BLEND_OP_ADD;
   case VK_BLEND_OP_SUBTRACT: return D3D12_BLEND_OP_SUBTRACT;
   case VK_BLEND_OP_REVERSE_SUBTRACT: return D3D12_BLEND_OP_REV_SUBTRACT;
   case VK_BLEND_OP_MIN: return D3D12_BLEND_OP_MIN;
   case VK_BLEND_OP_MAX: return D3D12_BLEND_OP_MAX;
   default: unreachable("Invalid blend op");
   }
}

static D3D12_LOGIC_OP
translate_logic_op(VkLogicOp in)
{
   switch (in) {
   case VK_LOGIC_OP_CLEAR: return D3D12_LOGIC_OP_CLEAR;
   case VK_LOGIC_OP_AND: return D3D12_LOGIC_OP_AND;
   case VK_LOGIC_OP_AND_REVERSE: return D3D12_LOGIC_OP_AND_REVERSE;
   case VK_LOGIC_OP_COPY: return D3D12_LOGIC_OP_COPY;
   case VK_LOGIC_OP_AND_INVERTED: return D3D12_LOGIC_OP_AND_INVERTED;
   case VK_LOGIC_OP_NO_OP: return D3D12_LOGIC_OP_NOOP;
   case VK_LOGIC_OP_XOR: return D3D12_LOGIC_OP_XOR;
   case VK_LOGIC_OP_OR: return D3D12_LOGIC_OP_OR;
   case VK_LOGIC_OP_NOR: return D3D12_LOGIC_OP_NOR;
   case VK_LOGIC_OP_EQUIVALENT: return D3D12_LOGIC_OP_EQUIV;
   case VK_LOGIC_OP_INVERT: return D3D12_LOGIC_OP_INVERT;
   case VK_LOGIC_OP_OR_REVERSE: return D3D12_LOGIC_OP_OR_REVERSE;
   case VK_LOGIC_OP_COPY_INVERTED: return D3D12_LOGIC_OP_COPY_INVERTED;
   case VK_LOGIC_OP_OR_INVERTED: return D3D12_LOGIC_OP_OR_INVERTED;
   case VK_LOGIC_OP_NAND: return D3D12_LOGIC_OP_NAND;
   case VK_LOGIC_OP_SET: return D3D12_LOGIC_OP_SET;
   default: unreachable("Invalid logic op");
   }
}

static void
dzn_graphics_pipeline_translate_blend(struct dzn_graphics_pipeline *pipeline,
                                      D3D12_PIPELINE_STATE_STREAM_DESC *out,
                                      const VkGraphicsPipelineCreateInfo *in)
{
   const VkPipelineRasterizationStateCreateInfo *in_rast =
      in->pRasterizationState;
   const VkPipelineColorBlendStateCreateInfo *in_blend =
      in_rast->rasterizerDiscardEnable ? NULL : in->pColorBlendState;
   const VkPipelineMultisampleStateCreateInfo *in_ms =
      in_rast->rasterizerDiscardEnable ? NULL : in->pMultisampleState;

   if (!in_blend || !in_ms)
      return;

   d3d12_gfx_pipeline_state_stream_new_desc(out, BLEND, D3D12_BLEND_DESC, desc);
   D3D12_LOGIC_OP logicop =
      in_blend->logicOpEnable ?
      translate_logic_op(in_blend->logicOp) : D3D12_LOGIC_OP_NOOP;
   desc->AlphaToCoverageEnable = in_ms->alphaToCoverageEnable;
   memcpy(pipeline->blend.constants, in_blend->blendConstants,
          sizeof(pipeline->blend.constants));

   for (uint32_t i = 0; i < in_blend->attachmentCount; i++) {
      if (i > 0 &&
          !memcmp(&in_blend->pAttachments[i - 1], &in_blend->pAttachments[i],
                  sizeof(*in_blend->pAttachments)))
         desc->IndependentBlendEnable = true;

      desc->RenderTarget[i].BlendEnable =
         in_blend->pAttachments[i].blendEnable;
         in_blend->logicOpEnable;
      desc->RenderTarget[i].RenderTargetWriteMask =
         in_blend->pAttachments[i].colorWriteMask;

      if (in_blend->logicOpEnable) {
         desc->RenderTarget[i].LogicOpEnable = true;
         desc->RenderTarget[i].LogicOp = logicop;
      } else {
         desc->RenderTarget[i].SrcBlend =
            translate_blend_factor(in_blend->pAttachments[i].srcColorBlendFactor, false);
         desc->RenderTarget[i].DestBlend =
            translate_blend_factor(in_blend->pAttachments[i].dstColorBlendFactor, false);
         desc->RenderTarget[i].BlendOp =
            translate_blend_op(in_blend->pAttachments[i].colorBlendOp);
         desc->RenderTarget[i].SrcBlendAlpha =
            translate_blend_factor(in_blend->pAttachments[i].srcAlphaBlendFactor, true);
         desc->RenderTarget[i].DestBlendAlpha =
            translate_blend_factor(in_blend->pAttachments[i].dstAlphaBlendFactor, true);
         desc->RenderTarget[i].BlendOpAlpha =
            translate_blend_op(in_blend->pAttachments[i].alphaBlendOp);
      }
   }
}


static void
dzn_pipeline_init(struct dzn_pipeline *pipeline,
                  struct dzn_device *device,
                  VkPipelineBindPoint type,
                  struct dzn_pipeline_layout *layout)
{
   pipeline->type = type;
   pipeline->root.sets_param_count = layout->root.sets_param_count;
   pipeline->root.sysval_cbv_param_idx = layout->root.sysval_cbv_param_idx;
   pipeline->root.push_constant_cbv_param_idx = layout->root.push_constant_cbv_param_idx;
   STATIC_ASSERT(sizeof(pipeline->root.type) == sizeof(layout->root.type));
   memcpy(pipeline->root.type, layout->root.type, sizeof(pipeline->root.type));
   pipeline->root.sig = layout->root.sig;
   ID3D12RootSignature_AddRef(pipeline->root.sig);

   STATIC_ASSERT(sizeof(layout->desc_count) == sizeof(pipeline->desc_count));
   memcpy(pipeline->desc_count, layout->desc_count, sizeof(pipeline->desc_count));

   STATIC_ASSERT(sizeof(layout->sets) == sizeof(pipeline->sets));
   memcpy(pipeline->sets, layout->sets, sizeof(pipeline->sets));
   vk_object_base_init(&device->vk, &pipeline->base, VK_OBJECT_TYPE_PIPELINE);
}

static void
dzn_pipeline_finish(struct dzn_pipeline *pipeline)
{
   if (pipeline->state)
      ID3D12PipelineState_Release(pipeline->state);
   if (pipeline->root.sig)
      ID3D12RootSignature_Release(pipeline->root.sig);

   vk_object_base_finish(&pipeline->base);
}

static void
dzn_graphics_pipeline_destroy(struct dzn_graphics_pipeline *pipeline,
                              const VkAllocationCallbacks *alloc)
{
   if (!pipeline)
      return;

   for (uint32_t i = 0; i < ARRAY_SIZE(pipeline->indirect_cmd_sigs); i++) {
      if (pipeline->indirect_cmd_sigs[i])
         ID3D12CommandSignature_Release(pipeline->indirect_cmd_sigs[i]);
   }

   dzn_pipeline_finish(&pipeline->base);
   vk_free2(&pipeline->base.base.device->alloc, alloc, pipeline);
}

static VkResult
dzn_graphics_pipeline_create(struct dzn_device *device,
                             VkPipelineCache cache,
                             const VkGraphicsPipelineCreateInfo *pCreateInfo,
                             const VkAllocationCallbacks *pAllocator,
                             VkPipeline *out)
{
   const VkPipelineRenderingCreateInfo *ri = (const VkPipelineRenderingCreateInfo *)
      vk_find_struct_const(pCreateInfo, PIPELINE_RENDERING_CREATE_INFO);
   VK_FROM_HANDLE(vk_render_pass, pass, pCreateInfo->renderPass);
   VK_FROM_HANDLE(dzn_pipeline_layout, layout, pCreateInfo->layout);
   uint32_t color_count = 0;
   VkFormat color_fmts[MAX_RTS] = { 0 };
   VkFormat zs_fmt = VK_FORMAT_UNDEFINED;
   VkResult ret;
   HRESULT hres = 0;

   struct dzn_graphics_pipeline *pipeline =
      vk_zalloc2(&device->vk.alloc, pAllocator, sizeof(*pipeline), 8,
                 VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!pipeline)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   uintptr_t state_buf[MAX_GFX_PIPELINE_STATE_STREAM_SIZE / sizeof(uintptr_t)];
   D3D12_PIPELINE_STATE_STREAM_DESC stream_desc = {
      .pPipelineStateSubobjectStream = state_buf,
   };

   dzn_pipeline_init(&pipeline->base, device,
                     VK_PIPELINE_BIND_POINT_GRAPHICS,
                     layout);
   D3D12_INPUT_ELEMENT_DESC attribs[MAX_VERTEX_GENERIC_ATTRIBS] = { 0 };
   D3D12_INPUT_ELEMENT_DESC inputs[D3D12_VS_INPUT_REGISTER_COUNT] = { 0 };
   D3D12_SHADER_BYTECODE *shaders[MESA_VULKAN_SHADER_STAGES] = { 0 };

   const VkPipelineViewportStateCreateInfo *vp_info =
      pCreateInfo->pRasterizationState->rasterizerDiscardEnable ?
      NULL : pCreateInfo->pViewportState;

   ret = dzn_graphics_pipeline_translate_vi(pipeline, pAllocator, pCreateInfo, attribs);
   if (ret != VK_SUCCESS)
      goto out;

   if (pCreateInfo->pDynamicState) {
      for (uint32_t i = 0; i < pCreateInfo->pDynamicState->dynamicStateCount; i++) {
         switch (pCreateInfo->pDynamicState->pDynamicStates[i]) {
         case VK_DYNAMIC_STATE_VIEWPORT:
            pipeline->vp.dynamic = true;
            break;
         case VK_DYNAMIC_STATE_SCISSOR:
            pipeline->scissor.dynamic = true;
            break;
         case VK_DYNAMIC_STATE_STENCIL_REFERENCE:
            pipeline->zsa.stencil_test.dynamic_ref = true;
            break;
         case VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK:
            pipeline->zsa.stencil_test.dynamic_compare_mask = true;
            break;
         case VK_DYNAMIC_STATE_STENCIL_WRITE_MASK:
            pipeline->zsa.stencil_test.dynamic_write_mask = true;
            break;
         case VK_DYNAMIC_STATE_BLEND_CONSTANTS:
            pipeline->blend.dynamic_constants = true;
            break;
         case VK_DYNAMIC_STATE_DEPTH_BOUNDS:
            pipeline->zsa.depth_bounds.dynamic = true;
            break;
         default: unreachable("Unsupported dynamic state");
         }
      }
   }

   dzn_graphics_pipeline_translate_ia(pipeline, &stream_desc, pCreateInfo);
   dzn_graphics_pipeline_translate_rast(pipeline, &stream_desc, pCreateInfo);
   dzn_graphics_pipeline_translate_ms(pipeline, &stream_desc, pCreateInfo);
   dzn_graphics_pipeline_translate_zsa(pipeline, &stream_desc, pCreateInfo);
   dzn_graphics_pipeline_translate_blend(pipeline, &stream_desc, pCreateInfo);

   if (pass) {
      const struct vk_subpass *subpass = &pass->subpasses[pCreateInfo->subpass];
      color_count = subpass->color_count;
      for (uint32_t i = 0; i < subpass->color_count; i++) {
         uint32_t idx = subpass->color_attachments[i].attachment;

         if (idx == VK_ATTACHMENT_UNUSED) continue;

         const struct vk_render_pass_attachment *attachment =
            &pass->attachments[idx];

         color_fmts[i] = attachment->format;
      }

      if (subpass->depth_stencil_attachment &&
          subpass->depth_stencil_attachment->attachment != VK_ATTACHMENT_UNUSED) {
         const struct vk_render_pass_attachment *attachment =
            &pass->attachments[subpass->depth_stencil_attachment->attachment];

	 zs_fmt = attachment->format;
      }
   } else if (ri) {
      color_count = ri->colorAttachmentCount;
      memcpy(color_fmts, ri->pColorAttachmentFormats,
             sizeof(color_fmts[0]) * color_count);
      if (ri->depthAttachmentFormat != VK_FORMAT_UNDEFINED)
         zs_fmt = ri->depthAttachmentFormat;
      else if (ri->stencilAttachmentFormat != VK_FORMAT_UNDEFINED)
         zs_fmt = ri->stencilAttachmentFormat;
   }

   if (color_count > 0) {
      d3d12_gfx_pipeline_state_stream_new_desc(&stream_desc, RENDER_TARGET_FORMATS, struct D3D12_RT_FORMAT_ARRAY, rts);
      rts->NumRenderTargets = color_count;
      for (uint32_t i = 0; i < color_count; i++) {
         rts->RTFormats[i] =
            dzn_image_get_dxgi_format(color_fmts[i],
                                      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                                      VK_IMAGE_ASPECT_COLOR_BIT);
      }
   }

   if (zs_fmt != VK_FORMAT_UNDEFINED) {
      d3d12_gfx_pipeline_state_stream_new_desc(&stream_desc, DEPTH_STENCIL_FORMAT, DXGI_FORMAT, ds_fmt);
      *ds_fmt =
         dzn_image_get_dxgi_format(zs_fmt,
                                   VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                                   VK_IMAGE_ASPECT_DEPTH_BIT |
                                   VK_IMAGE_ASPECT_STENCIL_BIT);
   }

   ret = dzn_graphics_pipeline_compile_shaders(device, pipeline, layout,
                                               &stream_desc,
                                               attribs, inputs, shaders,
                                               pCreateInfo);
   if (ret != VK_SUCCESS)
      goto out;

   d3d12_gfx_pipeline_state_stream_new_desc(&stream_desc, ROOT_SIGNATURE, ID3D12RootSignature*, root_sig);
   *root_sig = pipeline->base.root.sig;

   hres = ID3D12Device2_CreatePipelineState(device->dev, &stream_desc,
                                            &IID_ID3D12PipelineState,
                                            (void **)&pipeline->base.state);
   if (FAILED(hres)) {
      ret = vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
      goto out;
   }

   ret = VK_SUCCESS;

out:
   for (uint32_t i = 0; i < ARRAY_SIZE(shaders); i++) {
      if (shaders[i])
         free((void *)shaders[i]->pShaderBytecode);
   }

   if (ret != VK_SUCCESS)
      dzn_graphics_pipeline_destroy(pipeline, pAllocator);
   else
      *out = dzn_graphics_pipeline_to_handle(pipeline);

   return ret;
}

#define DZN_INDIRECT_CMD_SIG_MAX_ARGS 4

ID3D12CommandSignature *
dzn_graphics_pipeline_get_indirect_cmd_sig(struct dzn_graphics_pipeline *pipeline,
                                           enum dzn_indirect_draw_cmd_sig_type type)
{
   assert(type < DZN_NUM_INDIRECT_DRAW_CMD_SIGS);

   struct dzn_device *device =
      container_of(pipeline->base.base.device, struct dzn_device, vk);
   ID3D12CommandSignature *cmdsig = pipeline->indirect_cmd_sigs[type];

   if (cmdsig)
      return cmdsig;

   bool triangle_fan = type == DZN_INDIRECT_DRAW_TRIANGLE_FAN_CMD_SIG;
   bool indexed = type == DZN_INDIRECT_INDEXED_DRAW_CMD_SIG || triangle_fan;

   uint32_t cmd_arg_count = 0;
   D3D12_INDIRECT_ARGUMENT_DESC cmd_args[DZN_INDIRECT_CMD_SIG_MAX_ARGS];

   if (triangle_fan) {
      cmd_args[cmd_arg_count++] = (D3D12_INDIRECT_ARGUMENT_DESC) {
         .Type = D3D12_INDIRECT_ARGUMENT_TYPE_INDEX_BUFFER_VIEW,
      };
   }

   cmd_args[cmd_arg_count++] = (D3D12_INDIRECT_ARGUMENT_DESC) {
      .Type = D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT,
      .Constant = {
         .RootParameterIndex = pipeline->base.root.sysval_cbv_param_idx,
         .DestOffsetIn32BitValues = offsetof(struct dxil_spirv_vertex_runtime_data, first_vertex) / 4,
         .Num32BitValuesToSet = 2,
      },
   };

   cmd_args[cmd_arg_count++] = (D3D12_INDIRECT_ARGUMENT_DESC) {
      .Type = D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT,
      .Constant = {
         .RootParameterIndex = pipeline->base.root.sysval_cbv_param_idx,
         .DestOffsetIn32BitValues = offsetof(struct dxil_spirv_vertex_runtime_data, draw_id) / 4,
         .Num32BitValuesToSet = 1,
      },
   };

   cmd_args[cmd_arg_count++] = (D3D12_INDIRECT_ARGUMENT_DESC) {
      .Type = indexed ?
              D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED :
              D3D12_INDIRECT_ARGUMENT_TYPE_DRAW,
   };

   assert(cmd_arg_count <= ARRAY_SIZE(cmd_args));
   assert(offsetof(struct dxil_spirv_vertex_runtime_data, first_vertex) == 0);

   D3D12_COMMAND_SIGNATURE_DESC cmd_sig_desc = {
      .ByteStride =
         triangle_fan ?
         sizeof(struct dzn_indirect_triangle_fan_draw_exec_params) :
         sizeof(struct dzn_indirect_draw_exec_params),
      .NumArgumentDescs = cmd_arg_count,
      .pArgumentDescs = cmd_args,
   };
   HRESULT hres =
      ID3D12Device1_CreateCommandSignature(device->dev, &cmd_sig_desc,
                                           pipeline->base.root.sig,
                                           &IID_ID3D12CommandSignature,
                                           (void **)&cmdsig);
   if (FAILED(hres))
      return NULL;

   pipeline->indirect_cmd_sigs[type] = cmdsig;
   return cmdsig;
}

VKAPI_ATTR VkResult VKAPI_CALL
dzn_CreateGraphicsPipelines(VkDevice dev,
                            VkPipelineCache pipelineCache,
                            uint32_t count,
                            const VkGraphicsPipelineCreateInfo *pCreateInfos,
                            const VkAllocationCallbacks *pAllocator,
                            VkPipeline *pPipelines)
{
   VK_FROM_HANDLE(dzn_device, device, dev);
   VkResult result = VK_SUCCESS;

   unsigned i;
   for (i = 0; i < count; i++) {
      result = dzn_graphics_pipeline_create(device,
                                            pipelineCache,
                                            &pCreateInfos[i],
                                            pAllocator,
                                            &pPipelines[i]);
      if (result != VK_SUCCESS) {
         pPipelines[i] = VK_NULL_HANDLE;

         /* Bail out on the first error != VK_PIPELINE_COMPILE_REQUIRED_EX as it
          * is not obvious what error should be report upon 2 different failures.
          */
         if (result != VK_PIPELINE_COMPILE_REQUIRED_EXT)
            break;

         if (pCreateInfos[i].flags & VK_PIPELINE_CREATE_EARLY_RETURN_ON_FAILURE_BIT_EXT)
            break;
      }
   }

   for (; i < count; i++)
      pPipelines[i] = VK_NULL_HANDLE;

   return result;
}

static void
dzn_compute_pipeline_destroy(struct dzn_compute_pipeline *pipeline,
                             const VkAllocationCallbacks *alloc)
{
   if (!pipeline)
      return;

   if (pipeline->indirect_cmd_sig)
      ID3D12CommandSignature_Release(pipeline->indirect_cmd_sig);

   dzn_pipeline_finish(&pipeline->base);
   vk_free2(&pipeline->base.base.device->alloc, alloc, pipeline);
}

static VkResult
dzn_compute_pipeline_create(struct dzn_device *device,
                            VkPipelineCache cache,
                            const VkComputePipelineCreateInfo *pCreateInfo,
                            const VkAllocationCallbacks *pAllocator,
                            VkPipeline *out)
{
   VK_FROM_HANDLE(dzn_pipeline_layout, layout, pCreateInfo->layout);

   struct dzn_compute_pipeline *pipeline =
      vk_zalloc2(&device->vk.alloc, pAllocator, sizeof(*pipeline), 8,
                 VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!pipeline)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   dzn_pipeline_init(&pipeline->base, device,
                     VK_PIPELINE_BIND_POINT_COMPUTE,
                     layout);

   uintptr_t state_buf[MAX_COMPUTE_PIPELINE_STATE_STREAM_SIZE / sizeof(uintptr_t)];
   D3D12_PIPELINE_STATE_STREAM_DESC stream_desc = {
      .pPipelineStateSubobjectStream = state_buf,
   };

   d3d12_gfx_pipeline_state_stream_new_desc(&stream_desc, CS, D3D12_SHADER_BYTECODE, shader);
   d3d12_gfx_pipeline_state_stream_new_desc(&stream_desc, ROOT_SIGNATURE, ID3D12RootSignature *, root_sig);
   *root_sig = pipeline->base.root.sig;

   nir_shader *nir = NULL;
   VkResult ret =
      dzn_pipeline_get_nir_shader(device, layout,
                                  &pCreateInfo->stage, MESA_SHADER_COMPUTE,
                                  DXIL_SPIRV_YZ_FLIP_NONE, 0, 0,
                                  false,
                                  dxil_get_nir_compiler_options(), &nir);
   if (ret != VK_SUCCESS)
      goto out;

   ret = dzn_pipeline_compile_shader(device, nir, shader);
   if (ret != VK_SUCCESS)
      goto out;


   if (FAILED(ID3D12Device2_CreatePipelineState(device->dev, &stream_desc,
                                                &IID_ID3D12PipelineState,
                                                (void **)&pipeline->base.state))) {
      ret = vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
      goto out;
   }

out:
   ralloc_free(nir);
   free((void *)shader->pShaderBytecode);
   if (ret != VK_SUCCESS)
      dzn_compute_pipeline_destroy(pipeline, pAllocator);
   else
      *out = dzn_compute_pipeline_to_handle(pipeline);

   return ret;
}

ID3D12CommandSignature *
dzn_compute_pipeline_get_indirect_cmd_sig(struct dzn_compute_pipeline *pipeline)
{
   if (pipeline->indirect_cmd_sig)
      return pipeline->indirect_cmd_sig;

   struct dzn_device *device =
      container_of(pipeline->base.base.device, struct dzn_device, vk);

   D3D12_INDIRECT_ARGUMENT_DESC indirect_dispatch_args[] = {
      {
         .Type = D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT,
         .Constant = {
            .RootParameterIndex = pipeline->base.root.sysval_cbv_param_idx,
            .DestOffsetIn32BitValues = 0,
            .Num32BitValuesToSet = 3,
         },
      },
      {
         .Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH,
      },
   };

   D3D12_COMMAND_SIGNATURE_DESC indirect_dispatch_desc = {
      .ByteStride = sizeof(D3D12_DISPATCH_ARGUMENTS) * 2,
      .NumArgumentDescs = ARRAY_SIZE(indirect_dispatch_args),
      .pArgumentDescs = indirect_dispatch_args,
   };

   HRESULT hres =
      ID3D12Device1_CreateCommandSignature(device->dev, &indirect_dispatch_desc,
                                           pipeline->base.root.sig,
                                           &IID_ID3D12CommandSignature,
                                           (void **)&pipeline->indirect_cmd_sig);
   if (FAILED(hres))
      return NULL;

   return pipeline->indirect_cmd_sig;
}

VKAPI_ATTR VkResult VKAPI_CALL
dzn_CreateComputePipelines(VkDevice dev,
                           VkPipelineCache pipelineCache,
                           uint32_t count,
                           const VkComputePipelineCreateInfo *pCreateInfos,
                           const VkAllocationCallbacks *pAllocator,
                           VkPipeline *pPipelines)
{
   VK_FROM_HANDLE(dzn_device, device, dev);
   VkResult result = VK_SUCCESS;

   unsigned i;
   for (i = 0; i < count; i++) {
      result = dzn_compute_pipeline_create(device,
                                           pipelineCache,
                                           &pCreateInfos[i],
                                           pAllocator,
                                           &pPipelines[i]);
      if (result != VK_SUCCESS) {
         pPipelines[i] = VK_NULL_HANDLE;

         /* Bail out on the first error != VK_PIPELINE_COMPILE_REQUIRED_EX as it
          * is not obvious what error should be report upon 2 different failures.
          */
         if (result != VK_PIPELINE_COMPILE_REQUIRED_EXT)
            break;

         if (pCreateInfos[i].flags & VK_PIPELINE_CREATE_EARLY_RETURN_ON_FAILURE_BIT_EXT)
            break;
      }
   }

   for (; i < count; i++)
      pPipelines[i] = VK_NULL_HANDLE;

   return result;
}

VKAPI_ATTR void VKAPI_CALL
dzn_DestroyPipeline(VkDevice device,
                    VkPipeline pipeline,
                    const VkAllocationCallbacks *pAllocator)
{
   VK_FROM_HANDLE(dzn_pipeline, pipe, pipeline);

   if (!pipe)
      return;

   if (pipe->type == VK_PIPELINE_BIND_POINT_GRAPHICS) {
      struct dzn_graphics_pipeline *gfx = container_of(pipe, struct dzn_graphics_pipeline, base);
      dzn_graphics_pipeline_destroy(gfx, pAllocator);
   } else {
      assert(pipe->type == VK_PIPELINE_BIND_POINT_COMPUTE);
      struct dzn_compute_pipeline *compute = container_of(pipe, struct dzn_compute_pipeline, base);
      dzn_compute_pipeline_destroy(compute, pAllocator);
   }
}
