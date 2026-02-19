#pragma once

// =============================================================================
// DevelopShader.h - Unified develop shader (lens + crop + LUT in one pass)
// =============================================================================

#include <TrussC.h>
#include <tcLut.h>
#include "shaders/develop.glsl.h"

using namespace std;
using namespace tc;
using namespace tcx::lut;

class DevelopShader {
public:
    DevelopShader() = default;

    ~DevelopShader() {
        destroy();
    }

    bool load() {
        auto desc = develop_develop_shader_desc(sg_query_backend());
        shader_ = sg_make_shader(desc);
        if (sg_query_shader_state(shader_) != SG_RESOURCESTATE_VALID) {
            logError() << "[DevelopShader] Failed to create shader";
            return false;
        }

        // Pipeline
        sg_pipeline_desc pip_desc = {};
        pip_desc.shader = shader_;
        pip_desc.layout.attrs[ATTR_develop_develop_position].format = SG_VERTEXFORMAT_FLOAT2;
        pip_desc.layout.attrs[ATTR_develop_develop_texcoord0].format = SG_VERTEXFORMAT_FLOAT2;
        pip_desc.colors[0].blend.enabled = true;
        pip_desc.colors[0].blend.src_factor_rgb = SG_BLENDFACTOR_SRC_ALPHA;
        pip_desc.colors[0].blend.dst_factor_rgb = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        pip_desc.index_type = SG_INDEXTYPE_UINT16;
        pip_desc.label = "develop_pipeline";
        pipeline_ = sg_make_pipeline(&pip_desc);

        // Fullscreen quad vertices (NDC)
        float vertices[] = {
            -1.0f, -1.0f,  0.0f, 1.0f,
             1.0f, -1.0f,  1.0f, 1.0f,
             1.0f,  1.0f,  1.0f, 0.0f,
            -1.0f,  1.0f,  0.0f, 0.0f,
        };
        sg_buffer_desc vbuf = {};
        vbuf.data = SG_RANGE(vertices);
        vbuf.label = "develop_vbuf";
        vertexBuf_ = sg_make_buffer(&vbuf);

        uint16_t indices[] = { 0, 1, 2, 0, 2, 3 };
        sg_buffer_desc ibuf = {};
        ibuf.usage.index_buffer = true;
        ibuf.data = SG_RANGE(indices);
        ibuf.label = "develop_ibuf";
        indexBuf_ = sg_make_buffer(&ibuf);

        // Default sampler (linear + clamp)
        sg_sampler_desc smp = {};
        smp.min_filter = SG_FILTER_LINEAR;
        smp.mag_filter = SG_FILTER_LINEAR;
        smp.wrap_u = SG_WRAP_CLAMP_TO_EDGE;
        smp.wrap_v = SG_WRAP_CLAMP_TO_EDGE;
        smp.label = "develop_linear_smp";
        linearSmp_ = sg_make_sampler(&smp);

        // Create 1x1 dummy textures for lens/vig (white)
        createDummyTextures();

        loaded_ = true;
        return true;
    }

    // -------------------------------------------------------------------------
    // Source texture
    // -------------------------------------------------------------------------

    void setSourceTexture(sg_view view, sg_sampler sampler, int w, int h) {
        srcView_ = view;
        srcSmp_ = sampler;
        srcW_ = w;
        srcH_ = h;
    }

    void setSourceTexture(const Texture& tex) {
        srcView_ = tex.getView();
        srcSmp_ = tex.getSampler();
        srcW_ = tex.getWidth();
        srcH_ = tex.getHeight();
    }

    // -------------------------------------------------------------------------
    // LUT (camera profile)
    // -------------------------------------------------------------------------

    void setLut(const Lut3D& lut) {
        lutPtr_ = &lut;
    }

    void setLutBlend(float blend) {
        lutBlend_ = clamp(blend, 0.0f, 1.0f);
    }

    float getLutBlend() const { return lutBlend_; }

    void clearLut() {
        lutPtr_ = nullptr;
    }

    // -------------------------------------------------------------------------
    // Lens correction data (1D distortion LUT + 2D vignetting map)
    // -------------------------------------------------------------------------

    // Update 1D distortion+TCA LUT texture (512 RGBA32F entries)
    void updateLensLUT(const float* data, int size) {
        if (lensLutImg_.id) {
            sg_destroy_view(lensLutView_);
            sg_destroy_image(lensLutImg_);
        }

        sg_image_desc desc = {};
        desc.width = size;
        desc.height = 1;
        desc.pixel_format = SG_PIXELFORMAT_RGBA32F;
        desc.data.mip_levels[0].ptr = data;
        desc.data.mip_levels[0].size = size * 4 * sizeof(float);
        desc.label = "lens_lut_img";
        lensLutImg_ = sg_make_image(&desc);

        sg_view_desc vdesc = {};
        vdesc.texture.image = lensLutImg_;
        lensLutView_ = sg_make_view(&vdesc);

        hasLensLut_ = true;
    }

    // Update 2D vignetting gain map (R32F)
    void updateVigMap(const float* data, int rows, int cols) {
        if (vigImg_.id) {
            sg_destroy_view(vigView_);
            sg_destroy_image(vigImg_);
        }

        sg_image_desc desc = {};
        desc.width = cols;
        desc.height = rows;
        desc.pixel_format = SG_PIXELFORMAT_R32F;
        desc.data.mip_levels[0].ptr = data;
        desc.data.mip_levels[0].size = rows * cols * sizeof(float);
        desc.label = "vig_map_img";
        vigImg_ = sg_make_image(&desc);

        sg_view_desc vdesc = {};
        vdesc.texture.image = vigImg_;
        vigView_ = sg_make_view(&vdesc);

        hasVigMap_ = true;
    }

    void setLensParams(bool enabled, float autoScale,
                       float cropOX, float cropOY, float cropSX, float cropSY,
                       float optCX, float optCY, float invDiag,
                       float imgW, float imgH) {
        lensEnabled_ = enabled;
        autoScale_ = autoScale;
        cropRect_[0] = cropOX;
        cropRect_[1] = cropOY;
        cropRect_[2] = cropSX;
        cropRect_[3] = cropSY;
        opticalCenter_[0] = optCX;
        opticalCenter_[1] = optCY;
        invDiag_ = invDiag;
        imageSize_[0] = imgW;
        imageSize_[1] = imgH;
    }

    void setLensEnabled(bool enabled) {
        lensEnabled_ = enabled;
    }

    void clearLensData() {
        hasLensLut_ = false;
        hasVigMap_ = false;
        lensEnabled_ = false;
    }

    // -------------------------------------------------------------------------
    // Draw
    // -------------------------------------------------------------------------

    void draw(float x, float y, float w, float h) {
        if (!loaded_ || !srcView_.id) return;

        // Flush sokol_gl before custom drawing
        sgl_draw();

        sg_apply_pipeline(pipeline_);

        float dpi = sapp_dpi_scale();
        float winW = (float)sapp_width();
        float winH = (float)sapp_height();

        sg_apply_viewportf(x * dpi, y * dpi, w * dpi, h * dpi, true);
        sg_apply_scissor_rectf(x * dpi, y * dpi, w * dpi, h * dpi, true);

        // Bindings
        sg_bindings bind = {};
        bind.vertex_buffers[0] = vertexBuf_;
        bind.index_buffer = indexBuf_;

        // Texture bindings (must match shader slot numbers)
        bind.views[VIEW_develop_srcTex] = srcView_;
        bind.samplers[SMP_develop_srcSmp] = srcSmp_;

        // LUT (3D)
        if (lutPtr_ && lutPtr_->isAllocated() && lutBlend_ > 0.0f) {
            bind.views[VIEW_develop_lutTex] = lutPtr_->getView();
            bind.samplers[SMP_develop_lutSmp] = lutPtr_->getSampler();
        } else {
            bind.views[VIEW_develop_lutTex] = dummyLut3DView_;
            bind.samplers[SMP_develop_lutSmp] = linearSmp_;
        }

        // Lens LUT (1D as 2D)
        if (hasLensLut_) {
            bind.views[VIEW_develop_lensLutTex] = lensLutView_;
            bind.samplers[SMP_develop_lensLutSmp] = linearSmp_;
        } else {
            bind.views[VIEW_develop_lensLutTex] = dummyView_;
            bind.samplers[SMP_develop_lensLutSmp] = linearSmp_;
        }

        // Vignetting map
        if (hasVigMap_) {
            bind.views[VIEW_develop_vigTex] = vigView_;
            bind.samplers[SMP_develop_vigSmp] = linearSmp_;
        } else {
            bind.views[VIEW_develop_vigTex] = dummyView_;
            bind.samplers[SMP_develop_vigSmp] = linearSmp_;
        }

        sg_apply_bindings(&bind);

        // Uniforms
        develop_fs_develop_params_t params = {};
        params.lutSize = (lutPtr_ && lutPtr_->isAllocated()) ? (float)lutPtr_->getSize() : 1.0f;
        params.lutBlend = (lutPtr_ && lutPtr_->isAllocated()) ? lutBlend_ : 0.0f;
        params.lensEnabled = (lensEnabled_ && hasLensLut_) ? 1.0f : 0.0f;
        params.autoScale = autoScale_;
        for (int i = 0; i < 4; i++) params.cropRect[i] = cropRect_[i];
        params.opticalCenter[0] = opticalCenter_[0];
        params.opticalCenter[1] = opticalCenter_[1];
        params.invDiag = invDiag_;
        params.vigEnabled = (lensEnabled_ && hasVigMap_) ? 1.0f : 0.0f;
        params._imageSize[0] = imageSize_[0];
        params._imageSize[1] = imageSize_[1];

        sg_range range = { &params, sizeof(params) };
        sg_apply_uniforms(UB_develop_fs_develop_params, &range);

        sg_draw(0, 6, 1);

        // Restore viewport + scissor to full window
        sg_apply_viewportf(0, 0, winW, winH, true);
        sg_apply_scissor_rectf(0, 0, winW, winH, true);

        // Restore sokol_gl state (pipeline + projection)
        // Must be called from global draw context, NOT from Node tree draw()
        sgl_defaults();
        sgl_matrix_mode_projection();
        sgl_ortho(0.0f, winW / dpi, winH / dpi, 0.0f, -10000.0f, 10000.0f);
        sgl_matrix_mode_modelview();
        sgl_load_identity();
    }

private:
    bool loaded_ = false;

    sg_shader shader_ = {};
    sg_pipeline pipeline_ = {};
    sg_buffer vertexBuf_ = {};
    sg_buffer indexBuf_ = {};
    sg_sampler linearSmp_ = {};

    // Source
    sg_view srcView_ = {};
    sg_sampler srcSmp_ = {};
    int srcW_ = 0, srcH_ = 0;

    // LUT
    const Lut3D* lutPtr_ = nullptr;
    float lutBlend_ = 1.0f;

    // Lens correction textures
    sg_image lensLutImg_ = {};
    sg_view lensLutView_ = {};
    bool hasLensLut_ = false;

    sg_image vigImg_ = {};
    sg_view vigView_ = {};
    bool hasVigMap_ = false;

    // Lens uniform state
    bool lensEnabled_ = false;
    float autoScale_ = 1.0f;
    float cropRect_[4] = {0, 0, 1, 1};
    float opticalCenter_[2] = {0.5f, 0.5f};
    float invDiag_ = 0;
    float imageSize_[2] = {0, 0};

    // Dummy textures (1x1 white for missing bindings)
    sg_image dummyImg_ = {};
    sg_view dummyView_ = {};
    sg_image dummyLut3DImg_ = {};
    sg_view dummyLut3DView_ = {};

    void createDummyTextures() {
        // 2D white pixel (for lens LUT / vig when disabled)
        float white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
        sg_image_desc desc2d = {};
        desc2d.width = 1;
        desc2d.height = 1;
        desc2d.pixel_format = SG_PIXELFORMAT_RGBA32F;
        desc2d.data.mip_levels[0].ptr = white;
        desc2d.data.mip_levels[0].size = sizeof(white);
        desc2d.label = "develop_dummy_2d";
        dummyImg_ = sg_make_image(&desc2d);

        sg_view_desc vd2 = {};
        vd2.texture.image = dummyImg_;
        dummyView_ = sg_make_view(&vd2);

        // 3D 1x1x1 white (for LUT when disabled)
        unsigned char lut3d[4] = {255, 255, 255, 255};
        sg_image_desc desc3d = {};
        desc3d.type = SG_IMAGETYPE_3D;
        desc3d.width = 1;
        desc3d.height = 1;
        desc3d.num_slices = 1;
        desc3d.pixel_format = SG_PIXELFORMAT_RGBA8;
        desc3d.data.mip_levels[0].ptr = lut3d;
        desc3d.data.mip_levels[0].size = sizeof(lut3d);
        desc3d.label = "develop_dummy_lut3d";
        dummyLut3DImg_ = sg_make_image(&desc3d);

        sg_view_desc vd3 = {};
        vd3.texture.image = dummyLut3DImg_;
        dummyLut3DView_ = sg_make_view(&vd3);
    }

    void destroy() {
        if (loaded_) {
            sg_destroy_pipeline(pipeline_);
            sg_destroy_shader(shader_);
            sg_destroy_buffer(vertexBuf_);
            sg_destroy_buffer(indexBuf_);
            sg_destroy_sampler(linearSmp_);
            sg_destroy_view(dummyView_);
            sg_destroy_image(dummyImg_);
            sg_destroy_view(dummyLut3DView_);
            sg_destroy_image(dummyLut3DImg_);
        }
        if (lensLutImg_.id) {
            sg_destroy_view(lensLutView_);
            sg_destroy_image(lensLutImg_);
        }
        if (vigImg_.id) {
            sg_destroy_view(vigView_);
            sg_destroy_image(vigImg_);
        }
    }
};
