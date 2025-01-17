/*
 * Copyright 2023 Rive
 */

#include "rive/pls/gl/pls_render_context_gl_impl.hpp"

#include "rive/pls/gl/pls_render_target_gl.hpp"
#include "shaders/constants.glsl"
#include "rive/pls/gl/gl_utils.hpp"

#include "generated/shaders/glsl.exports.h"

namespace rive::pls
{
using DrawBufferMask = PLSRenderTargetGL::DrawBufferMask;

static bool needs_coalesced_atomic_resolve_and_transfer(const pls::FlushDescriptor& desc)
{
    return (desc.combinedShaderFeatures & ShaderFeatures::ENABLE_ADVANCED_BLEND) &&
           lite_rtti_cast<FramebufferRenderTargetGL*>(
               static_cast<PLSRenderTargetGL*>(desc.renderTarget)) != nullptr;
}

class PLSRenderContextGLImpl::PLSImplRWTexture : public PLSRenderContextGLImpl::PLSImpl
{
    bool supportsRasterOrdering(const GLCapabilities& capabilities) const override
    {
        return capabilities.ARB_fragment_shader_interlock ||
               capabilities.INTEL_fragment_shader_ordering;
    }

    void activatePixelLocalStorage(PLSRenderContextGLImpl* plsContextImpl,
                                   const FlushDescriptor& desc) override
    {
        auto renderTarget = static_cast<PLSRenderTargetGL*>(desc.renderTarget);
        renderTarget->allocateInternalPLSTextures(desc.interlockMode);

        bool renderDirectToRasterPipeline =
            desc.interlockMode == InterlockMode::atomics &&
            !(desc.combinedShaderFeatures & ShaderFeatures::ENABLE_ADVANCED_BLEND);
        if (renderDirectToRasterPipeline)
        {
            plsContextImpl->state()->setBlendEquation(BlendMode::srcOver);
        }
        else if (auto framebufferRenderTarget =
                     lite_rtti_cast<FramebufferRenderTargetGL*>(renderTarget))
        {
            // We're targeting an external FBO but can't render to it directly. Make sure to
            // allocate and attach an offscreen target texture.
            framebufferRenderTarget->allocateOffscreenTargetTexture();
            if (desc.colorLoadAction == pls::LoadAction::preserveRenderTarget)
            {
                // Copy the framebuffer's contents to our offscreen texture.
                framebufferRenderTarget->bindDestinationFramebuffer(GL_READ_FRAMEBUFFER);
                framebufferRenderTarget->bindInternalFramebuffer(GL_DRAW_FRAMEBUFFER,
                                                                 DrawBufferMask::color);
                glutils::BlitFramebuffer(desc.renderTargetUpdateBounds, renderTarget->height());
            }
        }

        // Clear the necessary textures.
        auto rwTexBuffers = DrawBufferMask::coverage;
        if (desc.interlockMode == pls::InterlockMode::rasterOrdering)
        {
            rwTexBuffers |= DrawBufferMask::color | DrawBufferMask::scratchColor;
        }
        else if (desc.combinedShaderFeatures & ShaderFeatures::ENABLE_ADVANCED_BLEND)
        {
            rwTexBuffers |= DrawBufferMask::color;
        }
        if (desc.combinedShaderFeatures & pls::ShaderFeatures::ENABLE_CLIPPING)
        {
            rwTexBuffers |= DrawBufferMask::clip;
        }
        renderTarget->bindInternalFramebuffer(GL_FRAMEBUFFER, rwTexBuffers);
        if (desc.colorLoadAction == pls::LoadAction::clear &&
            (rwTexBuffers & DrawBufferMask::color))
        {
            // If the color buffer is not a storage texture, we will clear it once the main
            // framebuffer gets bound.
            float clearColor4f[4];
            UnpackColorToRGBA32F(desc.clearColor, clearColor4f);
            glClearBufferfv(GL_COLOR, COLOR_PLANE_IDX, clearColor4f);
        }
        {
            GLuint coverageClear[4]{desc.coverageClearValue};
            glClearBufferuiv(GL_COLOR, COVERAGE_PLANE_IDX, coverageClear);
        }
        if (desc.combinedShaderFeatures & pls::ShaderFeatures::ENABLE_CLIPPING)
        {
            constexpr static GLuint kZeroClear[4]{};
            glClearBufferuiv(GL_COLOR, CLIP_PLANE_IDX, kZeroClear);
        }

        switch (desc.interlockMode)
        {
            case pls::InterlockMode::rasterOrdering:
                // rasterOrdering mode renders by storing to an image texture. Bind a framebuffer
                // with no color attachments.
                renderTarget->bindHeadlessFramebuffer(plsContextImpl->m_capabilities);
                break;
            case pls::InterlockMode::atomics:
                renderTarget->bindDestinationFramebuffer(GL_FRAMEBUFFER);
                if (desc.colorLoadAction == pls::LoadAction::clear &&
                    !(rwTexBuffers & DrawBufferMask::color))
                {
                    // We're rendering directly to the main framebuffer. Clear it now.
                    float cc[4];
                    UnpackColorToRGBA32F(desc.clearColor, cc);
                    glClearColor(cc[0], cc[1], cc[2], cc[3]);
                    glClear(GL_COLOR_BUFFER_BIT);
                }
                else if (needs_coalesced_atomic_resolve_and_transfer(desc))
                {
                    // When rendering to an offscreen atomic texture, still bind the target
                    // framebuffer, but disable color writes until it's time to resolve.
                    plsContextImpl->state()->setWriteMasks(false, true, 0xff);
                }
                break;
            default:
                RIVE_UNREACHABLE();
        }

        renderTarget->bindAsImageTextures(rwTexBuffers);

        glMemoryBarrierByRegion(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
    }

    pls::ShaderMiscFlags atomicResolveShaderMiscFlags(
        const pls::FlushDescriptor& desc) const override
    {
        assert(desc.interlockMode == pls::InterlockMode::atomics);
        return needs_coalesced_atomic_resolve_and_transfer(desc)
                   ? pls::ShaderMiscFlags::coalescedResolveAndTransfer
                   : pls::ShaderMiscFlags::none;
    }

    void setupAtomicResolve(PLSRenderContextGLImpl* plsContextImpl,
                            const pls::FlushDescriptor& desc) override
    {
        assert(desc.interlockMode == pls::InterlockMode::atomics);
        if (needs_coalesced_atomic_resolve_and_transfer(desc))
        {
            // Turn the color mask back on now that we're about to resolve.
            plsContextImpl->state()->setWriteMasks(true, true, 0xff);
        }
    }

    void deactivatePixelLocalStorage(PLSRenderContextGLImpl*, const FlushDescriptor& desc) override
    {
        glMemoryBarrierByRegion(GL_ALL_BARRIER_BITS);

        // atomic mode never needs to copy anything here because it transfers the offscreen texture
        // during resolve.
        if (desc.interlockMode == pls::InterlockMode::rasterOrdering)
        {
            if (auto framebufferRenderTarget = lite_rtti_cast<FramebufferRenderTargetGL*>(
                    static_cast<PLSRenderTargetGL*>(desc.renderTarget)))
            {
                // We rendered to an offscreen texture. Copy back to the external target
                // framebuffer.
                framebufferRenderTarget->bindInternalFramebuffer(GL_READ_FRAMEBUFFER,
                                                                 DrawBufferMask::color);
                framebufferRenderTarget->bindDestinationFramebuffer(GL_DRAW_FRAMEBUFFER);
                glutils::BlitFramebuffer(desc.renderTargetUpdateBounds,
                                         framebufferRenderTarget->height());
            }
        }
    }

    void pushShaderDefines(pls::InterlockMode, std::vector<const char*>* defines) const override
    {
        defines->push_back(GLSL_PLS_IMPL_STORAGE_TEXTURE);
        defines->push_back(GLSL_USING_PLS_STORAGE_TEXTURES);
    }

    void onBarrier(const pls::FlushDescriptor&) override
    {
        return glMemoryBarrierByRegion(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
    }
};

std::unique_ptr<PLSRenderContextGLImpl::PLSImpl> PLSRenderContextGLImpl::MakePLSImplRWTexture()
{
    return std::make_unique<PLSImplRWTexture>();
}
} // namespace rive::pls
