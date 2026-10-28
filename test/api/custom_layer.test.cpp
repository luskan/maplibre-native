#if MLN_RENDER_BACKEND_OPENGL

#include <mln/test/util.hpp>

#include <mln/gfx/headless_frontend.hpp>
#include <mln/style/layers/custom_layer.hpp>
#include <mln/gl/defines.hpp>
#include <mln/map/map.hpp>
#include <mln/map/map_options.hpp>
#include <mln/platform/gl_functions.hpp>
#include <mln/storage/resource_options.hpp>
#include <mln/style/layers/fill_layer.hpp>
#include <mln/style/style.hpp>
#include <mln/util/io.hpp>
#include <mln/util/mat4.hpp>
#include <mln/util/run_loop.hpp>

#include <cstdlib>
#include <memory>
#include <stdexcept>

using namespace mln;
using namespace mln::style;
using namespace mln::platform;

// Note that custom layers need to draw geometry with a z value of 1 to take
// advantage of depth-based fragment culling.
static const GLchar* vertexShaderSource = R"MBGL_SHADER(
attribute vec2 a_pos;
void main() {
    gl_Position = vec4(a_pos, 1, 1);
}
)MBGL_SHADER";

static const GLchar* fragmentShaderSource = R"MBGL_SHADER(
void main() {
    gl_FragColor = vec4(0, 0.5, 0, 0.5);
}
)MBGL_SHADER";

// Not using any mbgl-specific stuff (other than a basic error-checking macro)
// in the layer implementation because it is intended to reflect how someone
// using custom layers might actually write their own implementation.

class TestLayer : public mln::style::CustomLayerHost {
public:
    void initialize(const mln::style::CustomLayerInitParameters&) override {
        program = MBGL_CHECK_ERROR(glCreateProgram());
        vertexShader = MBGL_CHECK_ERROR(glCreateShader(GL_VERTEX_SHADER));
        fragmentShader = MBGL_CHECK_ERROR(glCreateShader(GL_FRAGMENT_SHADER));

        MBGL_CHECK_ERROR(glShaderSource(vertexShader, 1, &vertexShaderSource, nullptr));
        MBGL_CHECK_ERROR(glCompileShader(vertexShader));
        MBGL_CHECK_ERROR(glAttachShader(program, vertexShader));
        MBGL_CHECK_ERROR(glShaderSource(fragmentShader, 1, &fragmentShaderSource, nullptr));
        MBGL_CHECK_ERROR(glCompileShader(fragmentShader));
        MBGL_CHECK_ERROR(glAttachShader(program, fragmentShader));
        MBGL_CHECK_ERROR(glLinkProgram(program));
        a_pos = MBGL_CHECK_ERROR(glGetAttribLocation(program, "a_pos"));

        GLfloat triangle[] = {0, 0.5, 0.5, -0.5, -0.5, -0.5};
        MBGL_CHECK_ERROR(glGenBuffers(1, &buffer));
        MBGL_CHECK_ERROR(glBindBuffer(GL_ARRAY_BUFFER, buffer));
        MBGL_CHECK_ERROR(glBufferData(GL_ARRAY_BUFFER, 6 * sizeof(GLfloat), triangle, GL_STATIC_DRAW));
    }

    void render(const mln::style::CustomLayerRenderParameters&) override {
        MBGL_CHECK_ERROR(glUseProgram(program));
        MBGL_CHECK_ERROR(glBindBuffer(GL_ARRAY_BUFFER, buffer));
        MBGL_CHECK_ERROR(glEnableVertexAttribArray(a_pos));
        MBGL_CHECK_ERROR(glVertexAttribPointer(a_pos, 2, GL_FLOAT, GL_FALSE, 0, nullptr));
        MBGL_CHECK_ERROR(glDrawArrays(GL_TRIANGLE_STRIP, 0, 3));
    }

    void contextLost() override {}

    void deinitialize() override {
        if (program) {
            MBGL_CHECK_ERROR(glDeleteBuffers(1, &buffer));
            MBGL_CHECK_ERROR(glDetachShader(program, vertexShader));
            MBGL_CHECK_ERROR(glDetachShader(program, fragmentShader));
            MBGL_CHECK_ERROR(glDeleteShader(vertexShader));
            MBGL_CHECK_ERROR(glDeleteShader(fragmentShader));
            MBGL_CHECK_ERROR(glDeleteProgram(program));
        }
    }

    GLuint program = 0;
    GLuint vertexShader = 0;
    GLuint fragmentShader = 0;
    GLuint buffer = 0;
    GLuint a_pos = 0;
};

TEST(CustomLayer, Basic) {
    if (gfx::Backend::GetType() != gfx::Backend::Type::OpenGL) {
        return;
    }

    util::RunLoop loop;

    HeadlessFrontend frontend{1};
    Map map(frontend,
            MapObserver::nullObserver(),
            MapOptions().withMapMode(MapMode::Static).withSize(frontend.getSize()),
            ResourceOptions().withCachePath(":memory:").withAssetPath("test/fixtures/api/assets"));
    map.getStyle().loadJSON(util::read_file("test/fixtures/api/water.json"));
    map.jumpTo(CameraOptions().withCenter(LatLng{37.8, -122.5}).withZoom(10.0));
    map.getStyle().addLayer(std::make_unique<CustomLayer>("custom", std::make_unique<TestLayer>()));

    auto layer = std::make_unique<FillLayer>("landcover", "mapbox");
    layer->setSourceLayer("landcover");
    layer->setFillColor(Color{1.0, 1.0, 0.0, 1.0});
    map.getStyle().addLayer(std::move(layer));

    test::checkImage("test/fixtures/custom_layer/basic", frontend.render(map).image, 0.0006, 0.1);
}

TEST(CustomLayer, InLayerOrderBasic) {
    if (gfx::Backend::GetType() != gfx::Backend::Type::OpenGL) {
        return;
    }

    class InLayerOrderLayer final : public TestLayer {
    public:
        bool drawInLayerOrder() const override { return true; }
    };

    util::RunLoop loop;

    HeadlessFrontend frontend{1};
    Map map(frontend,
            MapObserver::nullObserver(),
            MapOptions().withMapMode(MapMode::Static).withSize(frontend.getSize()),
            ResourceOptions().withCachePath(":memory:").withAssetPath("test/fixtures/api/assets"));
    map.getStyle().loadJSON(util::read_file("test/fixtures/api/water.json"));
    map.jumpTo(CameraOptions().withCenter(LatLng{37.8, -122.5}).withZoom(10.0));
    map.getStyle().addLayer(std::make_unique<CustomLayer>("custom", std::make_unique<InLayerOrderLayer>()));

    auto layer = std::make_unique<FillLayer>("landcover", "mapbox");
    layer->setSourceLayer("landcover");
    layer->setFillColor(Color{1.0, 1.0, 0.0, 1.0});
    map.getStyle().addLayer(std::move(layer));

    // Drawn in its place, under the landcover layer, as in the expected image.
    test::checkImage("test/fixtures/custom_layer/basic", frontend.render(map).image, 0.0006, 0.1);
}

TEST(CustomLayer, InLayerOrderClearsStencilForLayersAbove) {
    if (gfx::Backend::GetType() != gfx::Backend::Type::OpenGL) {
        return;
    }

    // Leaves a stencil buffer and a scissor that the tile clip masks drawn before it do not match.
    class StencilWritingLayer final : public TestLayer {
    public:
        explicit StencilWritingLayer(bool dirty_)
            : dirty(dirty_) {}
        bool drawInLayerOrder() const override { return true; }
        void render(const CustomLayerRenderParameters& parameters) override {
            TestLayer::render(parameters);
            if (dirty) {
                MBGL_CHECK_ERROR(glStencilMask(0xFF));
                MBGL_CHECK_ERROR(glClearStencil(0x55));
                MBGL_CHECK_ERROR(glClear(GL_STENCIL_BUFFER_BIT));
                MBGL_CHECK_ERROR(glEnable(GL_SCISSOR_TEST));
                MBGL_CHECK_ERROR(glScissor(0, 0, 1, 1));
            }
        }

    private:
        bool dirty;
    };

    // The water fill draws the clip masks in the opaque pass, the translucent landcover above the host reuses them.
    const auto render = [](bool dirty, bool layerAbove) {
        util::RunLoop loop;
        HeadlessFrontend frontend{1};
        Map map(frontend,
                MapObserver::nullObserver(),
                MapOptions().withMapMode(MapMode::Static).withSize(frontend.getSize()),
                ResourceOptions().withCachePath(":memory:").withAssetPath("test/fixtures/api/assets"));
        map.getStyle().loadJSON(util::read_file("test/fixtures/api/water.json"));
        map.jumpTo(CameraOptions().withCenter(LatLng{37.8, -122.5}).withZoom(10.0));
        map.getStyle().addLayer(std::make_unique<CustomLayer>("custom", std::make_unique<StencilWritingLayer>(dirty)));
        if (layerAbove) {
            auto layer = std::make_unique<FillLayer>("landcover", "mapbox");
            layer->setSourceLayer("landcover");
            layer->setFillColor(Color{1.0, 1.0, 0.0, 1.0});
            layer->setFillOpacity(0.5f);
            map.getStyle().addLayer(std::move(layer));
        }
        return frontend.render(map).image;
    };
    const auto differentPixels = [](const PremultipliedImage& a, const PremultipliedImage& b) {
        size_t count = 0;
        for (size_t i = 0; i < a.bytes(); i += 4) {
            for (size_t c = 0; c < 4; ++c) {
                if (std::abs(int(a.data[i + c]) - int(b.data[i + c])) > 2) {
                    ++count;
                    break;
                }
            }
        }
        return count;
    };

    const auto clean = render(false, true);
    const auto dirty = render(true, true);
    const auto withoutLayerAbove = render(false, false);
    ASSERT_EQ(clean.size, dirty.size);
    ASSERT_EQ(clean.size, withoutLayerAbove.size);
    EXPECT_GT(differentPixels(clean, withoutLayerAbove), 0u);
    EXPECT_EQ(differentPixels(clean, dirty), 0u);
}

namespace {

struct HostCalls {
    bool failInitialization = true;
    unsigned initializationAttempts = 0;
    unsigned renderCalls = 0;
    unsigned deinitializeCalls = 0;
};

// The calls are kept outside the host, the host can be deleted before the test reads them.
class CountingHost final : public CustomLayerHost {
public:
    CountingHost(std::shared_ptr<HostCalls> calls_, bool inLayerOrder_)
        : calls(std::move(calls_)),
          inLayerOrder(inLayerOrder_) {}

    void initialize(const CustomLayerInitParameters&) override {
        ++calls->initializationAttempts;
        if (calls->failInitialization) {
            throw std::runtime_error("custom layer initialization failed");
        }
    }
    void render(const CustomLayerRenderParameters&) override { ++calls->renderCalls; }
    bool drawInLayerOrder() const override { return inLayerOrder; }
    void contextLost() override {}
    void deinitialize() override { ++calls->deinitializeCalls; }

private:
    std::shared_ptr<HostCalls> calls;
    bool inLayerOrder;
};

void checkFailedInitializationDoesNotRender(bool inLayerOrder) {
    util::RunLoop loop;
    HeadlessFrontend frontend{1};
    Map map(frontend,
            MapObserver::nullObserver(),
            MapOptions().withMapMode(MapMode::Static).withSize(frontend.getSize()),
            ResourceOptions().withCachePath(":memory:"));
    map.getStyle().loadJSON(R"({"version":8,"sources":{},"layers":[]})");

    auto hostState = std::make_shared<HostCalls>();
    map.getStyle().addLayer(
        std::make_unique<CustomLayer>("custom", std::make_unique<CountingHost>(hostState, inLayerOrder)));
    frontend.render(map);
    EXPECT_GT(hostState->initializationAttempts, 0u);
    EXPECT_EQ(hostState->renderCalls, 0u);

    hostState->failInitialization = false;
    frontend.render(map);
    EXPECT_GT(hostState->renderCalls, 0u);

    map.getStyle().removeLayer("custom");
    auto replacementState = std::make_shared<HostCalls>();
    map.getStyle().addLayer(
        std::make_unique<CustomLayer>("custom", std::make_unique<CountingHost>(replacementState, inLayerOrder)));
    frontend.render(map);
    EXPECT_GT(replacementState->initializationAttempts, 0u);
    EXPECT_EQ(replacementState->renderCalls, 0u);

    replacementState->failInitialization = false;
    frontend.render(map);
    EXPECT_GT(replacementState->renderCalls, 0u);
}

// After a replacement whose initialize throws, the old host gets no render and the retry draws the new host.
void checkFailedReplacementDoesNotRenderOldHost(bool inLayerOrder) {
    util::RunLoop loop;
    HeadlessFrontend frontend{1};
    Map map(frontend,
            MapObserver::nullObserver(),
            MapOptions().withMapMode(MapMode::Static).withSize(frontend.getSize()),
            ResourceOptions().withCachePath(":memory:"));
    map.getStyle().loadJSON(R"({"version":8,"sources":{},"layers":[]})");

    auto oldState = std::make_shared<HostCalls>();
    oldState->failInitialization = false;
    map.getStyle().addLayer(
        std::make_unique<CustomLayer>("custom", std::make_unique<CountingHost>(oldState, inLayerOrder)));
    frontend.render(map);
    ASSERT_GT(oldState->renderCalls, 0u);
    const unsigned oldRenderCalls = oldState->renderCalls;

    map.getStyle().removeLayer("custom");
    auto newState = std::make_shared<HostCalls>();
    map.getStyle().addLayer(
        std::make_unique<CustomLayer>("custom", std::make_unique<CountingHost>(newState, inLayerOrder)));
    frontend.render(map);
    EXPECT_GT(newState->initializationAttempts, 0u);
    EXPECT_EQ(newState->renderCalls, 0u);
    EXPECT_EQ(oldState->deinitializeCalls, 1u);
    EXPECT_EQ(oldState->renderCalls, oldRenderCalls);

    newState->failInitialization = false;
    frontend.render(map);
    EXPECT_GT(newState->renderCalls, 0u);
    EXPECT_EQ(oldState->renderCalls, oldRenderCalls);
}

} // namespace

TEST(CustomLayer, FailedInitializationDoesNotRender) {
    if (gfx::Backend::GetType() != gfx::Backend::Type::OpenGL) {
        return;
    }
    checkFailedInitializationDoesNotRender(false);
}

TEST(CustomLayer, InLayerOrderFailedInitializationDoesNotRender) {
    if (gfx::Backend::GetType() != gfx::Backend::Type::OpenGL) {
        return;
    }
    checkFailedInitializationDoesNotRender(true);
}

TEST(CustomLayer, FailedReplacementDoesNotRenderOldHost) {
    if (gfx::Backend::GetType() != gfx::Backend::Type::OpenGL) {
        return;
    }
    checkFailedReplacementDoesNotRenderOldHost(false);
}

TEST(CustomLayer, InLayerOrderFailedReplacementDoesNotRenderOldHost) {
    if (gfx::Backend::GetType() != gfx::Backend::Type::OpenGL) {
        return;
    }
    checkFailedReplacementDoesNotRenderOldHost(true);
}

#endif
