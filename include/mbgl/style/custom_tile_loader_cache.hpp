#pragma once

#include <cstddef>
#include <cstdint>

namespace mbgl
{
namespace style
{

struct CustomTileLoaderDataCacheStats
{
    bool enabled = true;
    uint64_t hits = 0;
    uint64_t stores = 0;
    uint64_t bypasses = 0;
    std::size_t tileCount = 0;
};

void setCustomTileLoaderDataCacheEnabled(bool enabled);
bool isCustomTileLoaderDataCacheEnabled();
CustomTileLoaderDataCacheStats getCustomTileLoaderDataCacheStats();

} // namespace style
} // namespace mbgl
