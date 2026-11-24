#include <mln/test/util.hpp>
#include <mln/gfx/backend_scope.hpp>
#include <mln/gfx/command_encoder.hpp>
#include <mln/gfx/upload_pass.hpp>
#include <mln/gl/context.hpp>
#include <mln/gl/defines.hpp>
#include <mln/gl/headless_backend.hpp>
#include <mln/gl/vertex_attribute_gl.hpp>

TEST(GLUploadPass, BindsOnlyLocationsUsedByTheProgram) {
    using namespace mln;
    gl::HeadlessBackend backend({32, 32});
    gfx::BackendScope scope{backend};
    gl::Context context{backend};
    auto encoder = context.createCommandEncoder();
    auto upload = encoder->createUploadPass("upload", backend.getDefaultRenderable());
    gl::VertexAttributeArrayGL defaults, overrides;
    std::vector<std::unique_ptr<gfx::VertexBufferResource>> buffers;
    const auto build = [&] {
        return upload->buildAttributeBindings(1, gfx::AttributeDataType::Float, size_t(-1), {}, defaults,
                                              overrides, gfx::BufferUsageType::StaticDraw, std::nullopt, buffers);
    };

    EXPECT_TRUE(build().empty());
    auto& attribute = defaults.set(shaders::idLinePathDistanceVertexAttribute, 2, gfx::AttributeDataType::Float, 1);
    ASSERT_TRUE(attribute);
    static_cast<gl::VertexAttributeGL&>(*attribute).setGLType(GL_FLOAT);
    attribute->set(0, 1.0f);
    const auto bindings = build();
    ASSERT_EQ(3u, bindings.size());
    EXPECT_FALSE(bindings[0]);
    EXPECT_FALSE(bindings[1]);
    EXPECT_TRUE(bindings[2]);
}
