/*
 * Copyright © 2015-2017 Broadcom
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
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include "util/format/u_format.h"
#include "util/u_surface.h"
#include "util/u_blitter.h"
#include "compiler/nir/nir_builder.h"
#include "v3d_context.h"
#include "broadcom/common/v3d_tiling.h"
#include "broadcom/common/v3d_tfu.h"

/**
 * The param @op_blit is used to tell if we are saving state for blitter_blit
 * (if true) or blitter_clear (if false). If other blitter functions are used
 * that require different state we may need something more elaborated than
 * this.
 */

void
v3d_blitter_save(struct v3d_context *v3d, bool op_blit)
{
        util_blitter_save_fragment_constant_buffer_slot(v3d->blitter,
                                                        v3d->constbuf[PIPE_SHADER_FRAGMENT].cb);
        util_blitter_save_vertex_buffer_slot(v3d->blitter, v3d->vertexbuf.vb);
        util_blitter_save_vertex_elements(v3d->blitter, v3d->vtx);
        util_blitter_save_vertex_shader(v3d->blitter, v3d->prog.bind_vs);
        util_blitter_save_geometry_shader(v3d->blitter, v3d->prog.bind_gs);
        util_blitter_save_so_targets(v3d->blitter, v3d->streamout.num_targets,
                                     v3d->streamout.targets);
        util_blitter_save_rasterizer(v3d->blitter, v3d->rasterizer);
        util_blitter_save_viewport(v3d->blitter, &v3d->viewport);
        util_blitter_save_fragment_shader(v3d->blitter, v3d->prog.bind_fs);
        util_blitter_save_blend(v3d->blitter, v3d->blend);
        util_blitter_save_depth_stencil_alpha(v3d->blitter, v3d->zsa);
        util_blitter_save_stencil_ref(v3d->blitter, &v3d->stencil_ref);
        util_blitter_save_sample_mask(v3d->blitter, v3d->sample_mask, 0);
        util_blitter_save_so_targets(v3d->blitter, v3d->streamout.num_targets,
                                     v3d->streamout.targets);

        if (op_blit) {
                util_blitter_save_scissor(v3d->blitter, &v3d->scissor);
                util_blitter_save_framebuffer(v3d->blitter, &v3d->framebuffer);
                util_blitter_save_fragment_sampler_states(v3d->blitter,
                                                          v3d->tex[PIPE_SHADER_FRAGMENT].num_samplers,
                                                          (void **)v3d->tex[PIPE_SHADER_FRAGMENT].samplers);
                util_blitter_save_fragment_sampler_views(v3d->blitter,
                                                         v3d->tex[PIPE_SHADER_FRAGMENT].num_textures,
                                                         v3d->tex[PIPE_SHADER_FRAGMENT].textures);
        }
}

static void
v3d_render_blit(struct pipe_context *ctx, struct pipe_blit_info *info)
{
        struct v3d_context *v3d = v3d_context(ctx);
        struct v3d_resource *src = v3d_resource(info->src.resource);
        struct pipe_resource *tiled = NULL;

        if (!info->mask)
                return;

        if (!src->tiled &&
            info->src.resource->target != PIPE_TEXTURE_1D &&
            info->src.resource->target != PIPE_TEXTURE_1D_ARRAY) {
                struct pipe_box box = {
                        .x = 0,
                        .y = 0,
                        .width = u_minify(info->src.resource->width0,
                                           info->src.level),
                        .height = u_minify(info->src.resource->height0,
                                           info->src.level),
                        .depth = 1,
                };
                struct pipe_resource tmpl = {
                        .target = info->src.resource->target,
                        .format = info->src.resource->format,
                        .width0 = box.width,
                        .height0 = box.height,
                        .depth0 = 1,
                        .array_size = 1,
                };
                tiled = ctx->screen->resource_create(ctx->screen, &tmpl);
                if (!tiled) {
                        fprintf(stderr, "Failed to create tiled blit temp\n");
                        return;
                }
                ctx->resource_copy_region(ctx,
                                          tiled, 0,
                                          0, 0, 0,
                                          info->src.resource, info->src.level,
                                          &box);
                info->src.level = 0;
                info->src.resource = tiled;
        }

        if (!util_blitter_is_blit_supported(v3d->blitter, info)) {
                fprintf(stderr, "blit unsupported %s -> %s\n",
                    util_format_short_name(info->src.format),
                    util_format_short_name(info->dst.format));
                return;
        }

        v3d_blitter_save(v3d, true);
        util_blitter_blit(v3d->blitter, info);

        pipe_resource_reference(&tiled, NULL);
        info->mask = 0;
}

/* Implement stencil blits by reinterpreting the stencil data as an RGBA8888
 * or R8 texture.
 */
static void
v3d_stencil_blit(struct pipe_context *ctx, struct pipe_blit_info *info)
{
        struct v3d_context *v3d = v3d_context(ctx);
        struct v3d_resource *src = v3d_resource(info->src.resource);
        struct v3d_resource *dst = v3d_resource(info->dst.resource);
        enum pipe_format src_format, dst_format;

        if ((info->mask & PIPE_MASK_S) == 0)
                return;

        if (src->separate_stencil) {
                src = src->separate_stencil;
                src_format = PIPE_FORMAT_R8_UINT;
        } else {
                src_format = PIPE_FORMAT_RGBA8888_UINT;
        }

        if (dst->separate_stencil) {
                dst = dst->separate_stencil;
                dst_format = PIPE_FORMAT_R8_UINT;
        } else {
                dst_format = PIPE_FORMAT_RGBA8888_UINT;
        }

        /* Initialize the surface. */
        struct pipe_surface dst_tmpl = {
                .u.tex = {
                        .level = info->dst.level,
                        .first_layer = info->dst.box.z,
                        .last_layer = info->dst.box.z,
                },
                .format = dst_format,
        };
        struct pipe_surface *dst_surf =
                ctx->create_surface(ctx, &dst->base, &dst_tmpl);

        /* Initialize the sampler view. */
        struct pipe_sampler_view src_tmpl = {
                .target = src->base.target,
                .format = src_format,
                .u.tex = {
                        .first_level = info->src.level,
                        .last_level = info->src.level,
                        .first_layer = 0,
                        .last_layer = (PIPE_TEXTURE_3D ?
                                       u_minify(src->base.depth0,
                                                info->src.level) - 1 :
                                       src->base.array_size - 1),
                },
                .swizzle_r = PIPE_SWIZZLE_X,
                .swizzle_g = PIPE_SWIZZLE_Y,
                .swizzle_b = PIPE_SWIZZLE_Z,
                .swizzle_a = PIPE_SWIZZLE_W,
        };
        struct pipe_sampler_view *src_view =
                ctx->create_sampler_view(ctx, &src->base, &src_tmpl);

        v3d_blitter_save(v3d, true);
        util_blitter_blit_generic(v3d->blitter, dst_surf, &info->dst.box,
                                  src_view, &info->src.box,
                                  src->base.width0, src->base.height0,
                                  PIPE_MASK_R,
                                  PIPE_TEX_FILTER_NEAREST,
                                  info->scissor_enable ? &info->scissor : NULL,
                                  info->alpha_blend, false);

        pipe_surface_reference(&dst_surf, NULL);
        pipe_sampler_view_reference(&src_view, NULL);

        info->mask &= ~PIPE_MASK_S;
}

static bool
v3d_tfu(struct pipe_context *pctx,
        struct pipe_resource *pdst,
        struct pipe_resource *psrc,
        unsigned int src_level,
        unsigned int base_level,
        unsigned int last_level,
        unsigned int src_layer,
        unsigned int dst_layer,
        bool for_mipmap)
{
        struct v3d_context *v3d = v3d_context(pctx);
        struct v3d_screen *screen = v3d->screen;
        struct v3d_resource *src = v3d_resource(psrc);
        struct v3d_resource *dst = v3d_resource(pdst);
        struct v3d_resource_slice *src_base_slice = &src->slices[src_level];
        struct v3d_resource_slice *dst_base_slice = &dst->slices[base_level];
        int msaa_scale = pdst->nr_samples > 1 ? 2 : 1;
        int width = u_minify(pdst->width0, base_level) * msaa_scale;
        int height = u_minify(pdst->height0, base_level) * msaa_scale;
        enum pipe_format pformat;

        if (psrc->format != pdst->format)
                return false;
        if (psrc->nr_samples != pdst->nr_samples)
                return false;

        /* Can't write to raster. */
        if (dst_base_slice->tiling == V3D_TILING_RASTER)
                return false;

        /* When using TFU for blit, we are doing exact copies (both input and
         * output format must be the same, no scaling, etc), so there is no
         * pixel format conversions. Thus we can rewrite the format to use one
         * that is TFU compatible based on its texel size.
         */
        if (for_mipmap) {
                pformat = pdst->format;
        } else {
                switch (dst->cpp) {
                case 16: pformat = PIPE_FORMAT_R32G32B32A32_FLOAT;   break;
                case 8:  pformat = PIPE_FORMAT_R16G16B16A16_FLOAT;   break;
                case 4:  pformat = PIPE_FORMAT_R32_FLOAT;            break;
                case 2:  pformat = PIPE_FORMAT_R16_FLOAT;            break;
                case 1:  pformat = PIPE_FORMAT_R8_UNORM;             break;
                default: unreachable("unsupported format bit-size"); break;
                };
        }

        uint32_t tex_format = v3d_get_tex_format(&screen->devinfo, pformat);

        if (!v3d_tfu_supports_tex_format(&screen->devinfo, tex_format, for_mipmap)) {
                assert(for_mipmap);
                return false;
        }

        v3d_flush_jobs_writing_resource(v3d, psrc, V3D_FLUSH_DEFAULT, false);
        v3d_flush_jobs_reading_resource(v3d, pdst, V3D_FLUSH_DEFAULT, false);

        struct drm_v3d_submit_tfu tfu = {
                .ios = (height << 16) | width,
                .bo_handles = {
                        dst->bo->handle,
                        src != dst ? src->bo->handle : 0
                },
                .in_sync = v3d->out_sync,
                .out_sync = v3d->out_sync,
        };
        uint32_t src_offset = (src->bo->offset +
                               v3d_layer_offset(psrc, src_level, src_layer));
        tfu.iia |= src_offset;
        if (src_base_slice->tiling == V3D_TILING_RASTER) {
                tfu.icfg |= (V3D33_TFU_ICFG_FORMAT_RASTER <<
                             V3D33_TFU_ICFG_FORMAT_SHIFT);
        } else {
                tfu.icfg |= ((V3D33_TFU_ICFG_FORMAT_LINEARTILE +
                              (src_base_slice->tiling - V3D_TILING_LINEARTILE)) <<
                             V3D33_TFU_ICFG_FORMAT_SHIFT);
        }

        uint32_t dst_offset = (dst->bo->offset +
                               v3d_layer_offset(pdst, base_level, dst_layer));
        tfu.ioa |= dst_offset;
        if (last_level != base_level)
                tfu.ioa |= V3D33_TFU_IOA_DIMTW;
        tfu.ioa |= ((V3D33_TFU_IOA_FORMAT_LINEARTILE +
                     (dst_base_slice->tiling - V3D_TILING_LINEARTILE)) <<
                    V3D33_TFU_IOA_FORMAT_SHIFT);

        tfu.icfg |= tex_format << V3D33_TFU_ICFG_TTYPE_SHIFT;
        tfu.icfg |= (last_level - base_level) << V3D33_TFU_ICFG_NUMMM_SHIFT;

        switch (src_base_slice->tiling) {
        case V3D_TILING_UIF_NO_XOR:
        case V3D_TILING_UIF_XOR:
                tfu.iis |= (src_base_slice->padded_height /
                            (2 * v3d_utile_height(src->cpp)));
                break;
        case V3D_TILING_RASTER:
                tfu.iis |= src_base_slice->stride / src->cpp;
                break;
        case V3D_TILING_LINEARTILE:
        case V3D_TILING_UBLINEAR_1_COLUMN:
        case V3D_TILING_UBLINEAR_2_COLUMN:
                break;
       }

        /* If we're writing level 0 (!IOA_DIMTW), then we need to supply the
         * OPAD field for the destination (how many extra UIF blocks beyond
         * those necessary to cover the height).  When filling mipmaps, the
         * miplevel 1+ tiling state is inferred.
         */
        if (dst_base_slice->tiling == V3D_TILING_UIF_NO_XOR ||
            dst_base_slice->tiling == V3D_TILING_UIF_XOR) {
                int uif_block_h = 2 * v3d_utile_height(dst->cpp);
                int implicit_padded_height = align(height, uif_block_h);

                tfu.icfg |= (((dst_base_slice->padded_height -
                               implicit_padded_height) / uif_block_h) <<
                             V3D33_TFU_ICFG_OPAD_SHIFT);
        }

        int ret = v3d_ioctl(screen->fd, DRM_IOCTL_V3D_SUBMIT_TFU, &tfu);
        if (ret != 0) {
                fprintf(stderr, "Failed to submit TFU job: %d\n", ret);
                return false;
        }

        dst->writes++;

        return true;
}

bool
v3d_generate_mipmap(struct pipe_context *pctx,
                    struct pipe_resource *prsc,
                    enum pipe_format format,
                    unsigned int base_level,
                    unsigned int last_level,
                    unsigned int first_layer,
                    unsigned int last_layer)
{
        if (format != prsc->format)
                return false;

        /* We could maybe support looping over layers for array textures, but
         * we definitely don't support 3D.
         */
        if (first_layer != last_layer)
                return false;

        return v3d_tfu(pctx,
                       prsc, prsc,
                       base_level,
                       base_level, last_level,
                       first_layer, first_layer,
                       true);
}

static void
v3d_tfu_blit(struct pipe_context *pctx, struct pipe_blit_info *info)
{
        int dst_width = u_minify(info->dst.resource->width0, info->dst.level);
        int dst_height = u_minify(info->dst.resource->height0, info->dst.level);

        if ((info->mask & PIPE_MASK_RGBA) == 0)
                return;

        if (info->scissor_enable ||
            info->dst.box.x != 0 ||
            info->dst.box.y != 0 ||
            info->dst.box.width != dst_width ||
            info->dst.box.height != dst_height ||
            info->dst.box.depth != 1 ||
            info->src.box.x != 0 ||
            info->src.box.y != 0 ||
            info->src.box.width != info->dst.box.width ||
            info->src.box.height != info->dst.box.height ||
            info->src.box.depth != 1) {
                return;
        }

        if (info->dst.format != info->src.format)
                return;

        if (v3d_tfu(pctx, info->dst.resource, info->src.resource,
                    info->src.level,
                    info->dst.level, info->dst.level,
                    info->src.box.z, info->dst.box.z,
                    false)) {
                info->mask &= ~PIPE_MASK_RGBA;
        }
}

static struct pipe_surface *
v3d_get_blit_surface(struct pipe_context *pctx,
                     struct pipe_resource *prsc,
                     enum pipe_format format,
                     unsigned level,
                     int16_t layer)
{
        struct pipe_surface tmpl;

        tmpl.format = format;
        tmpl.u.tex.level = level;
        tmpl.u.tex.first_layer = layer;
        tmpl.u.tex.last_layer = layer;

        return pctx->create_surface(pctx, prsc, &tmpl);
}

static bool
is_tile_unaligned(unsigned size, unsigned tile_size)
{
        return size & (tile_size - 1);
}

static void
v3d_tlb_blit(struct pipe_context *pctx, struct pipe_blit_info *info)
{
        struct v3d_context *v3d = v3d_context(pctx);
        struct v3d_screen *screen = v3d->screen;

        if (screen->devinfo.ver < 40 || !info->mask)
                return;

        bool is_color_blit = info->mask & PIPE_MASK_RGBA;
        bool is_depth_blit = info->mask & PIPE_MASK_Z;
        bool is_stencil_blit = info->mask & PIPE_MASK_S;

        /* We should receive either a depth/stencil blit, or color blit, but
         * not both.
         */
        assert ((is_color_blit && !is_depth_blit && !is_stencil_blit) ||
                (!is_color_blit && (is_depth_blit || is_stencil_blit)));

        if (info->scissor_enable)
                return;

        if (info->src.box.x != info->dst.box.x ||
            info->src.box.y != info->dst.box.y ||
            info->src.box.width != info->dst.box.width ||
            info->src.box.height != info->dst.box.height)
                return;

        if (is_color_blit &&
            util_format_is_depth_or_stencil(info->dst.format))
                return;

        if (!v3d_rt_format_supported(&screen->devinfo, info->src.format))
                return;

        if (v3d_get_rt_format(&screen->devinfo, info->src.format) !=
            v3d_get_rt_format(&screen->devinfo, info->dst.format))
                return;

        bool msaa = (info->src.resource->nr_samples > 1 ||
                     info->dst.resource->nr_samples > 1);
        bool is_msaa_resolve = (info->src.resource->nr_samples > 1 &&
                                info->dst.resource->nr_samples < 2);

        if (is_msaa_resolve &&
            !v3d_format_supports_tlb_msaa_resolve(&screen->devinfo, info->src.format))
                return;

        v3d_flush_jobs_writing_resource(v3d, info->src.resource, V3D_FLUSH_DEFAULT, false);

        struct pipe_surface *dst_surf =
           v3d_get_blit_surface(pctx, info->dst.resource, info->dst.format, info->dst.level, info->dst.box.z);
        struct pipe_surface *src_surf =
           v3d_get_blit_surface(pctx, info->src.resource, info->src.format, info->src.level, info->src.box.z);

        struct pipe_surface *surfaces[V3D_MAX_DRAW_BUFFERS] = { 0 };
        if (is_color_blit)
                surfaces[0] = dst_surf;

        bool double_buffer =
                 unlikely(V3D_DEBUG & V3D_DEBUG_DOUBLE_BUFFER) && !msaa;

        uint32_t tile_width, tile_height, max_bpp;
        v3d_get_tile_buffer_size(msaa, double_buffer,
                                 is_color_blit ? 1 : 0, surfaces, src_surf,
                                 &tile_width, &tile_height, &max_bpp);

        int dst_surface_width = u_minify(info->dst.resource->width0,
                                         info->dst.level);
        int dst_surface_height = u_minify(info->dst.resource->height0,
                                         info->dst.level);
        if (is_tile_unaligned(info->dst.box.x, tile_width) ||
            is_tile_unaligned(info->dst.box.y, tile_height) ||
            (is_tile_unaligned(info->dst.box.width, tile_width) &&
             info->dst.box.x + info->dst.box.width != dst_surface_width) ||
            (is_tile_unaligned(info->dst.box.height, tile_height) &&
             info->dst.box.y + info->dst.box.height != dst_surface_height)) {
                pipe_surface_reference(&dst_surf, NULL);
                pipe_surface_reference(&src_surf, NULL);
                return;
        }

        struct v3d_job *job = v3d_get_job(v3d,
                                          is_color_blit ? 1u : 0u,
                                          surfaces,
                                          is_color_blit ? NULL : dst_surf,
                                          src_surf);
        job->msaa = msaa;
        job->double_buffer = double_buffer;
        job->tile_width = tile_width;
        job->tile_height = tile_height;
        job->internal_bpp = max_bpp;
        job->draw_min_x = info->dst.box.x;
        job->draw_min_y = info->dst.box.y;
        job->draw_max_x = info->dst.box.x + info->dst.box.width;
        job->draw_max_y = info->dst.box.y + info->dst.box.height;
        job->scissor.disabled = false;

        /* The simulator complains if we do a TLB load from a source with a
         * stride that is smaller than the destination's, so we program the
         * 'frame region' to match the smallest dimensions of the two surfaces.
         * This should be fine because we only get here if the src and dst boxes
         * match, so we know the blit involves the same tiles on both surfaces.
         */
        job->draw_width = MIN2(dst_surf->width, src_surf->width);
        job->draw_height = MIN2(dst_surf->height, src_surf->height);
        job->draw_tiles_x = DIV_ROUND_UP(job->draw_width,
                                         job->tile_width);
        job->draw_tiles_y = DIV_ROUND_UP(job->draw_height,
                                         job->tile_height);

        job->needs_flush = true;
        job->num_layers = info->dst.box.depth;

        job->store = 0;
        if (is_color_blit) {
                job->store |= PIPE_CLEAR_COLOR0;
                info->mask &= ~PIPE_MASK_RGBA;
        }
        if (is_depth_blit) {
                job->store |= PIPE_CLEAR_DEPTH;
                info->mask &= ~PIPE_MASK_Z;
        }
        if (is_stencil_blit){
                job->store |= PIPE_CLEAR_STENCIL;
                info->mask &= ~PIPE_MASK_S;
        }

        v3d41_start_binning(v3d, job);

        v3d_job_submit(v3d, job);

        pipe_surface_reference(&dst_surf, NULL);
        pipe_surface_reference(&src_surf, NULL);
}

/**
 * Creates the VS of the custom blit shader to convert YUV plane from
 * the NV12 format with BROADCOM_SAND_COL128 modifier to UIF tiled format.
 * This vertex shader is mostly a pass-through VS.
 */
static void *
v3d_get_sand8_vs(struct pipe_context *pctx)
{
        struct v3d_context *v3d = v3d_context(pctx);
        struct pipe_screen *pscreen = pctx->screen;

        if (v3d->sand8_blit_vs)
                return v3d->sand8_blit_vs;

        const struct nir_shader_compiler_options *options =
                pscreen->get_compiler_options(pscreen,
                                              PIPE_SHADER_IR_NIR,
                                              PIPE_SHADER_VERTEX);

        nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_VERTEX,
                                                       options,
                                                       "sand8_blit_vs");

        const struct glsl_type *vec4 = glsl_vec4_type();
        nir_variable *pos_in = nir_variable_create(b.shader,
                                                   nir_var_shader_in,
                                                   vec4, "pos");

        nir_variable *pos_out = nir_variable_create(b.shader,
                                                    nir_var_shader_out,
                                                    vec4, "gl_Position");
        pos_out->data.location = VARYING_SLOT_POS;
        nir_store_var(&b, pos_out, nir_load_var(&b, pos_in), 0xf);

        struct pipe_shader_state shader_tmpl = {
                .type = PIPE_SHADER_IR_NIR,
                .ir.nir = b.shader,
        };

        v3d->sand8_blit_vs = pctx->create_vs_state(pctx, &shader_tmpl);

        return v3d->sand8_blit_vs;
}
/**
 * Creates the FS of the custom blit shader to convert YUV plane from
 * the NV12 format with BROADCOM_SAND_COL128 modifier to UIF tiled format.
 * The result texture is equivalent to a chroma (cpp=2) or luma (cpp=1)
 * plane for a NV12 format without the SAND modifier.
 */
static void *
v3d_get_sand8_fs(struct pipe_context *pctx, int cpp)
{
        struct v3d_context *v3d = v3d_context(pctx);
        struct pipe_screen *pscreen = pctx->screen;
        struct pipe_shader_state **cached_shader;
        const char *name;

        if (cpp == 1) {
                cached_shader = &v3d->sand8_blit_fs_luma;
                name = "sand8_blit_fs_luma";
        } else {
                cached_shader = &v3d->sand8_blit_fs_chroma;
                name = "sand8_blit_fs_chroma";
        }

        if (*cached_shader)
                return *cached_shader;

        const struct nir_shader_compiler_options *options =
                pscreen->get_compiler_options(pscreen,
                                              PIPE_SHADER_IR_NIR,
                                              PIPE_SHADER_FRAGMENT);

        nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_FRAGMENT,
                                                       options, "%s", name);
        const struct glsl_type *vec4 = glsl_vec4_type();

        const struct glsl_type *glsl_int = glsl_int_type();

        nir_variable *color_out =
                nir_variable_create(b.shader, nir_var_shader_out,
                                    vec4, "f_color");
        color_out->data.location = FRAG_RESULT_COLOR;

        nir_variable *pos_in =
                nir_variable_create(b.shader, nir_var_shader_in, vec4, "pos");
        pos_in->data.location = VARYING_SLOT_POS;
        nir_ssa_def *pos = nir_load_var(&b, pos_in);

        nir_ssa_def *zero = nir_imm_int(&b, 0);
        nir_ssa_def *one = nir_imm_int(&b, 1);
        nir_ssa_def *two = nir_imm_int(&b, 2);
        nir_ssa_def *six = nir_imm_int(&b, 6);
        nir_ssa_def *seven = nir_imm_int(&b, 7);
        nir_ssa_def *eight = nir_imm_int(&b, 8);

        nir_ssa_def *x = nir_f2i32(&b, nir_channel(&b, pos, 0));
        nir_ssa_def *y = nir_f2i32(&b, nir_channel(&b, pos, 1));

        nir_variable *stride_in =
                nir_variable_create(b.shader, nir_var_uniform, glsl_int,
                                    "sand8_stride");
        nir_ssa_def *stride =
                nir_load_uniform(&b, 1, 32, zero,
                                 .base = stride_in->data.driver_location,
                                 .range = 1,
                                 .dest_type = nir_type_int32);

        nir_ssa_def *x_offset;
        nir_ssa_def *y_offset;

        /* UIF tiled format is composed by UIF blocks, Each block has
         * four 64 byte microtiles. Inside each microtile pixels are stored
         * in raster format. But microtiles have different dimensions
         * based in the bits per pixel of the image.
         *
         *   8bpp microtile dimensions are 8x8
         *  16bpp microtile dimensions are 8x4
         *  32bpp microtile dimensions are 4x4
         *
         * As we are reading and writing with 32bpp to optimize
         * the number of texture operations during the blit. We need
         * to adjust the offsets were we read and write as data will
         * be later read using 8bpp (luma) and 16bpp (chroma).
         *
         * For chroma 8x4 16bpp raster order is compatible with 4x4
         * 32bpp. In both layouts each line has 8*2 == 4*4 == 16 bytes.
         * But luma 8x8 8bpp raster order is not compatible
         * with 4x4 32bpp. 8bpp has 8 bytes per line, and 32bpp has
         * 16 bytes per line. So if we read a 8bpp texture that was
         * written as 32bpp texture. Bytes would be misplaced.
         *
         * inter/intra_utile_x_offests takes care of mapping the offsets
         * between microtiles to deal with this issue for luma planes.
         */
        if (cpp == 1) {
                nir_ssa_def *intra_utile_x_offset =
                        nir_ishl(&b, nir_iand_imm(&b, x, 1), two);
                nir_ssa_def *inter_utile_x_offset =
                        nir_ishl(&b, nir_iand_imm(&b, x, 60), one);
                nir_ssa_def *stripe_offset=
                        nir_ishl(&b,nir_imul(&b,nir_ishr_imm(&b, x, 6),
                                             stride),
                                 seven);

                x_offset = nir_iadd(&b, stripe_offset,
                                        nir_iadd(&b, intra_utile_x_offset,
                                                     inter_utile_x_offset));
                y_offset = nir_iadd(&b,
                                    nir_ishl(&b, nir_iand_imm(&b, x, 2), six),
                                    nir_ishl(&b, y, eight));
        } else  {
                nir_ssa_def *stripe_offset=
                        nir_ishl(&b,nir_imul(&b,nir_ishr_imm(&b, x, 5),
                                                stride),
                                 seven);
                x_offset = nir_iadd(&b, stripe_offset,
                               nir_ishl(&b, nir_iand_imm(&b, x, 31), two));
                y_offset = nir_ishl(&b, y, seven);
        }
        nir_ssa_def *ubo_offset = nir_iadd(&b, x_offset, y_offset);
        nir_ssa_def *load =
        nir_load_ubo(&b, 1, 32, one, ubo_offset,
                    .align_mul = 4,
                    .align_offset = 0,
                    .range_base = 0,
                    .range = ~0);

        nir_ssa_def *output = nir_unpack_unorm_4x8(&b, load);

        nir_store_var(&b, color_out,
                      output,
                      0xF);

        struct pipe_shader_state shader_tmpl = {
                .type = PIPE_SHADER_IR_NIR,
                .ir.nir = b.shader,
        };

        *cached_shader = pctx->create_fs_state(pctx, &shader_tmpl);

        return *cached_shader;
}

/**
 * Turns NV12 with SAND8 format modifier from raster-order with interleaved
 * luma and chroma 128-byte-wide-columns to tiled format for luma and chroma.
 *
 * This implementation is based on vc4_yuv_blit.
 */
static void
v3d_sand8_blit(struct pipe_context *pctx, struct pipe_blit_info *info)
{
        struct v3d_context *v3d = v3d_context(pctx);
        struct v3d_resource *src = v3d_resource(info->src.resource);
        ASSERTED struct v3d_resource *dst = v3d_resource(info->dst.resource);

        if (!src->sand_col128_stride)
                return;
        if (src->tiled)
                return;
        if (src->base.format != PIPE_FORMAT_R8_UNORM &&
            src->base.format != PIPE_FORMAT_R8G8_UNORM)
                return;
        if (!(info->mask & PIPE_MASK_RGBA))
                return;

        assert(dst->base.format == src->base.format);
        assert(dst->tiled);

        assert(info->src.box.x == 0 && info->dst.box.x == 0);
        assert(info->src.box.y == 0 && info->dst.box.y == 0);
        assert(info->src.box.width == info->dst.box.width);
        assert(info->src.box.height == info->dst.box.height);

        v3d_blitter_save(v3d, true);

        struct pipe_surface dst_tmpl;
        util_blitter_default_dst_texture(&dst_tmpl, info->dst.resource,
                                         info->dst.level, info->dst.box.z);
        /* Although the src textures are cpp=1 or cpp=2, the dst texture
         * uses a cpp=4 dst texture. So, all read/write texture ops will
         * be done using 32-bit read and writes.
         */
        dst_tmpl.format = PIPE_FORMAT_R8G8B8A8_UNORM;
        struct pipe_surface *dst_surf =
                pctx->create_surface(pctx, info->dst.resource, &dst_tmpl);
        if (!dst_surf) {
                fprintf(stderr, "Failed to create YUV dst surface\n");
                util_blitter_unset_running_flag(v3d->blitter);
                return;
        }

        uint32_t sand8_stride = src->sand_col128_stride;

        /* Adjust the dimensions of dst luma/chroma to match src
         * size now we are using a cpp=4 format. Next dimension take into
         * account the UIF microtile layouts.
         */
        dst_surf->width = align(dst_surf->width, 8) / 2;
        if (src->cpp == 1)
                dst_surf->height /= 2;

        /* Set the constant buffer. */
        struct pipe_constant_buffer cb_uniforms = {
                .user_buffer = &sand8_stride,
                .buffer_size = sizeof(sand8_stride),
        };

        pctx->set_constant_buffer(pctx, PIPE_SHADER_FRAGMENT, 0, false,
                                  &cb_uniforms);
        struct pipe_constant_buffer cb_src = {
                .buffer = info->src.resource,
                .buffer_offset = src->slices[info->src.level].offset,
                .buffer_size = (src->bo->size -
                                src->slices[info->src.level].offset),
        };
        pctx->set_constant_buffer(pctx, PIPE_SHADER_FRAGMENT, 2, false,
                                  &cb_src);
        /* Unbind the textures, to make sure we don't try to recurse into the
         * shadow blit.
         */
        pctx->set_sampler_views(pctx, PIPE_SHADER_FRAGMENT, 0, 0, 0, false, NULL);
        pctx->bind_sampler_states(pctx, PIPE_SHADER_FRAGMENT, 0, 0, NULL);

        util_blitter_custom_shader(v3d->blitter, dst_surf,
                                   v3d_get_sand8_vs(pctx),
                                   v3d_get_sand8_fs(pctx, src->cpp));

        util_blitter_restore_textures(v3d->blitter);
        util_blitter_restore_constant_buffer_state(v3d->blitter);

        /* Restore cb1 (util_blitter doesn't handle this one). */
        struct pipe_constant_buffer cb_disabled = { 0 };
        pctx->set_constant_buffer(pctx, PIPE_SHADER_FRAGMENT, 1, false,
                                  &cb_disabled);

        pipe_surface_reference(&dst_surf, NULL);

        info->mask &= ~PIPE_MASK_RGBA;
        return;
}


/* Optimal hardware path for blitting pixels.
 * Scaling, format conversion, up- and downsampling (resolve) are allowed.
 */
void
v3d_blit(struct pipe_context *pctx, const struct pipe_blit_info *blit_info)
{
        struct v3d_context *v3d = v3d_context(pctx);
        struct pipe_blit_info info = *blit_info;

        v3d_sand8_blit(pctx, &info);

        v3d_tfu_blit(pctx, &info);

        v3d_tlb_blit(pctx, &info);

        v3d_stencil_blit(pctx, &info);

        v3d_render_blit(pctx, &info);

        /* Flush our blit jobs immediately.  They're unlikely to get reused by
         * normal drawing or other blits, and without flushing we can easily
         * run into unexpected OOMs when blits are used for a large series of
         * texture uploads before using the textures.
         */
        v3d_flush_jobs_writing_resource(v3d, info.dst.resource,
                                        V3D_FLUSH_DEFAULT, false);
}
