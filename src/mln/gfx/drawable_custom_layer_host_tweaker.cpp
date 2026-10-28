#include <mln/gfx/drawable_custom_layer_host_tweaker.hpp>
#include <mln/renderer/paint_parameters.hpp>
#include <mln/gfx/renderer_backend.hpp>
#include <mln/gfx/renderable.hpp>

#include <mln/gfx/drawable.hpp>
#include <mln/gfx/context.hpp>

#if MLN_RENDER_BACKEND_METAL
#include <mln/style/layers/mtl/custom_layer_render_parameters.hpp>
#include <mln/mtl/render_pass.hpp>
#elif MLN_RENDER_BACKEND_VULKAN
#include <mln/style/layers/vulkan/custom_layer_render_parameters.hpp>
#include <mln/vulkan/render_pass.hpp>
#elif MLN_RENDER_BACKEND_OPENGL
#include <mln/gl/context.hpp>
#endif

#include <memory>

namespace mln {
namespace gfx {

void DrawableCustomLayerHostTweaker::execute([[maybe_unused]] gfx::Drawable& drawable,
                                             mln::PaintParameters& paintParameters) {
    // custom drawing
    auto& context = paintParameters.context;
    context.resetState(paintParameters.depthModeForSublayer(0, gfx::DepthMaskType::ReadOnly),
                       paintParameters.colorModeForRenderPass());

#if MLN_RENDER_BACKEND_METAL
    const auto& mtlRenderPass = static_cast<mtl::RenderPass*>(paintParameters.renderPass.get());
    mtlRenderPass->resetState();

    style::mtl::CustomLayerRenderParameters parameters(paintParameters);
#elif MLN_RENDER_BACKEND_VULKAN
    style::vulkan::CustomLayerRenderParameters parameters(paintParameters);
#else
    style::CustomLayerRenderParameters parameters(paintParameters);
#endif

    host->render(parameters);

    // Reset the view back to our original one, just in case the CustomLayer
    // changed the viewport or Framebuffer.
    paintParameters.backend.getDefaultRenderable().getResource<gfx::RenderableResource>().bind();

    context.setDirtyState();

#if MLN_RENDER_BACKEND_OPENGL
    // The host can write the stencil buffer, so the tile clip masks may be lost. Clear it with
    // a full write mask and no scissor, the layers above draw their masks again.
    auto& glContext = static_cast<gl::Context&>(context);
    glContext.setStencilMode(gfx::StencilMode{.test = gfx::StencilMode::Always(),
                                              .ref = 0,
                                              .mask = 0b11111111,
                                              .fail = gfx::StencilOpType::Keep,
                                              .depthFail = gfx::StencilOpType::Keep,
                                              .pass = gfx::StencilOpType::Keep});
    glContext.setScissorTest({0, 0, 0, 0});
    paintParameters.clearStencil();
#endif

    context.bindGlobalUniformBuffers(*paintParameters.renderPass);
}

} // namespace gfx
} // namespace mln
