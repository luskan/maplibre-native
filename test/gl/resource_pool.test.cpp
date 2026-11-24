#if MLN_RENDER_BACKEND_OPENGL
#include <mln/test/util.hpp>

#include <unordered_set>

#include <mln/gfx/backend_scope.hpp>
#include <mln/gfx/types.hpp>
#include <mln/gl/headless_backend.hpp>
#include <mln/gl/context.hpp>
#include <mln/gl/defines.hpp>
#include <mln/gl/texture2d.hpp>
#include <mln/platform/gl_functions.hpp>

using namespace mln;

namespace {

constexpr auto oneMB = 1024 * 1024;

constexpr gl::Texture2DDesc makeDescOneMB() {
    return {{1024, 1024}, gfx::TexturePixelType::Luminance, gfx::TextureChannelDataType::UnsignedByte};
}

} // namespace

TEST(ResourcePool, MipmappedAtlasUsesTrilinearMinification) {
    gl::HeadlessBackend backend{{32, 32}};
    gfx::BackendScope scope{backend};
    gl::Context context{backend};
    gl::Texture2D texture(context);
    texture.setSamplerConfiguration({.filter = gfx::TextureFilterType::Linear, .mipmapped = true});
    std::array<uint8_t, 3 * 4 * 4> pixels{};
    texture.upload(pixels.data(), {3, 4});
    platform::GLint filter = 0;
    platform::glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, &filter);
    EXPECT_EQ(GL_LINEAR_MIPMAP_LINEAR, filter);
    platform::glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, &filter);
    EXPECT_EQ(GL_LINEAR, filter);
    EXPECT_EQ(GL_NO_ERROR, platform::glGetError());
}

TEST(ResourcePool, FailedAllocationLeavesThePoolAndTextureCountUnchanged) {
    gl::HeadlessBackend backend{{32, 32}};
    gfx::BackendScope scope{backend};
    gl::Context context{backend};
    platform::GLint limit = 0;
    platform::glGetIntegerv(GL_MAX_TEXTURE_SIZE, &limit);
    ASSERT_GT(limit, 0);
    const auto count = context.renderingStats().numActiveTextures;
    const auto bytes = context.getTexturePool().storage();
    EXPECT_THROW(context.createUniqueTexture({static_cast<uint32_t>(limit) + 1, 1},
                                             gfx::TexturePixelType::RGBA, gfx::TextureChannelDataType::UnsignedByte),
                 std::bad_alloc);
    EXPECT_EQ(count, context.renderingStats().numActiveTextures);
    EXPECT_EQ(bytes, context.getTexturePool().storage());
}

TEST(ResourcePool, TexturePool) {
    gl::HeadlessBackend backend{{32, 32}};
    gfx::BackendScope scope{backend};

    gl::Context context{backend};
    EXPECT_TRUE(context.empty(true));

    auto& pool = context.getTexturePool();
    EXPECT_LE(oneMB, pool.maxStorage());

    EXPECT_EQ(pool.usedStorage(), 0);
    EXPECT_EQ(pool.unusedStorage(), 0);
    EXPECT_EQ(pool.storage(), 0);
    EXPECT_FALSE(pool.isUsed(3));
    EXPECT_FALSE(pool.isUnused(3));
    EXPECT_FALSE(pool.isPooled(3));

    auto id = pool.alloc(makeDescOneMB());
    EXPECT_EQ(pool.usedStorage(), oneMB);
    EXPECT_EQ(pool.unusedStorage(), 0);
    EXPECT_EQ(pool.storage(), oneMB);
    EXPECT_TRUE(pool.isUsed(id));
    EXPECT_FALSE(pool.isUnused(id));
    EXPECT_TRUE(pool.isPooled(id));

    pool.release(id);
    EXPECT_EQ(pool.usedStorage(), 0u);
    EXPECT_EQ(pool.unusedStorage(), oneMB);
    EXPECT_EQ(pool.storage(), oneMB);
    EXPECT_FALSE(pool.isUsed(id));
    EXPECT_TRUE(pool.isUnused(id));
    EXPECT_TRUE(pool.isPooled(id));

    auto reused_id = pool.alloc(makeDescOneMB());
    EXPECT_EQ(reused_id, id);
    EXPECT_EQ(pool.usedStorage(), oneMB);
    EXPECT_EQ(pool.unusedStorage(), 0);
    EXPECT_EQ(pool.storage(), oneMB);
    EXPECT_TRUE(pool.isUsed(id));
    EXPECT_FALSE(pool.isUnused(id));
    EXPECT_TRUE(pool.isPooled(id));

    std::unordered_set<gl::TextureID> ids = {id};
    while (pool.storage() <= pool.maxStorage()) {
        auto new_id = pool.alloc(makeDescOneMB());
        EXPECT_TRUE(ids.find(new_id) == ids.end());
        ids.insert(new_id);
        EXPECT_EQ(pool.usedStorage(), oneMB * ids.size());
        EXPECT_EQ(pool.unusedStorage(), 0);
        EXPECT_EQ(pool.storage(), pool.usedStorage());
        EXPECT_TRUE(pool.isUsed(new_id));
        EXPECT_FALSE(pool.isUnused(new_id));
        EXPECT_TRUE(pool.isPooled(new_id));
    }

    EXPECT_LE(pool.maxStorage(), pool.usedStorage());
    EXPECT_EQ(pool.usedStorage(), oneMB * ids.size());
    EXPECT_EQ(pool.unusedStorage(), 0);
    EXPECT_EQ(pool.storage(), pool.usedStorage());
    EXPECT_TRUE(pool.isUsed(id));
    EXPECT_FALSE(pool.isUnused(id));
    EXPECT_TRUE(pool.isPooled(id));

    pool.release(id);
    ids.erase(id);
    EXPECT_GE(pool.maxStorage(), pool.usedStorage());
    EXPECT_EQ(pool.usedStorage(), oneMB * ids.size());
    EXPECT_EQ(pool.unusedStorage(), 0);
    EXPECT_EQ(pool.storage(), pool.usedStorage());
    EXPECT_FALSE(pool.isUsed(id));
    EXPECT_FALSE(pool.isUnused(id));
    EXPECT_FALSE(pool.isPooled(id));

    size_t used = pool.usedStorage();
    size_t unused = 0;
    while (!ids.empty()) {
        auto i = *ids.begin();
        pool.release(i);
        ids.erase(i);
        used -= oneMB;
        unused += oneMB;
        EXPECT_EQ(pool.usedStorage(), used);
        EXPECT_EQ(pool.unusedStorage(), unused);
        EXPECT_EQ(pool.storage(), used + unused);
        EXPECT_FALSE(pool.isUsed(i));
        EXPECT_TRUE(pool.isUnused(i));
        EXPECT_TRUE(pool.isPooled(i));
    }

    context.reduceMemoryUsage();
    context.renderingStats().numActiveTextures = 0;
    EXPECT_EQ(pool.usedStorage(), 0);
    EXPECT_EQ(pool.unusedStorage(), 0);
    EXPECT_EQ(pool.storage(), 0);
    EXPECT_FALSE(pool.isUsed(id));
    EXPECT_FALSE(pool.isUnused(id));
    EXPECT_FALSE(pool.isPooled(id));
    EXPECT_TRUE(context.empty(true));
}

#endif
