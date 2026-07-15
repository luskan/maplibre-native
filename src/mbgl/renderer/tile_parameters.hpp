#pragma once

#include <mbgl/map/mode.hpp>
#include <mbgl/actor/scheduler.hpp>

#include <cmath>
#include <cstdint>
#include <memory>
#include <numbers>
#include <optional>

#include <mapbox/std/weak.hpp>

namespace mbgl {

class TransformState;
class FileSource;
class AnnotationManager;
class ImageManager;
class GlyphManager;

struct GeometryTileZoomState {
    std::optional<float> paintZoom;
    float fallbackPaintZoomBias = 0;
    float fallbackLayerZoomBias = 0;

    bool operator==(const GeometryTileZoomState& rhs) const {
        return paintZoom == rhs.paintZoom && fallbackPaintZoomBias == rhs.fallbackPaintZoomBias &&
               fallbackLayerZoomBias == rhs.fallbackLayerZoomBias;
    }

    bool operator!=(const GeometryTileZoomState& rhs) const { return !(*this == rhs); }
};

inline GeometryTileZoomState calculateGeometryTileZoomState(float evaluationZoom,
                                                            bool evaluationZoomBiasEnabled,
                                                            float evaluationZoomBiasStatic,
                                                            double tileLodZoomShift) {
    if (evaluationZoomBiasEnabled) {
        return {std::floor(evaluationZoom), 0, 0};
    }

    return {std::nullopt, evaluationZoomBiasStatic - static_cast<float>(tileLodZoomShift), evaluationZoomBiasStatic};
}

inline float geometryTilePaintZoomBias(const GeometryTileZoomState& state, int32_t tileZoom) {
    return state.paintZoom ? *state.paintZoom - static_cast<float>(tileZoom) : state.fallbackPaintZoomBias;
}

inline float geometryTileLayerZoomBias(const GeometryTileZoomState& state, int32_t tileZoom) {
    return state.paintZoom ? *state.paintZoom - static_cast<float>(tileZoom) : state.fallbackLayerZoomBias;
}

namespace gfx {
class DynamicTextureAtlas;
using DynamicTextureAtlasPtr = std::shared_ptr<gfx::DynamicTextureAtlas>;
} // namespace gfx

class TileParameters {
public:
    const float pixelRatio;
    const MapDebugOptions debugOptions;
    const TransformState& transformState;
    std::shared_ptr<FileSource> fileSource;
    const MapMode mode;
    mapbox::base::WeakPtr<AnnotationManager> annotationManager;
    std::shared_ptr<ImageManager> imageManager;
    std::shared_ptr<GlyphManager> glyphManager;
    const uint8_t prefetchZoomDelta;
    TaggedScheduler threadPool;
    double tileLodMinRadius = 3;
    double tileLodScale = 1;
    double tileLodPitchThreshold = (60.0 / 180.0) * std::numbers::pi;
    double tileLodZoomShift = 0;
    gfx::DynamicTextureAtlasPtr dynamicTextureAtlas;
    GeometryTileZoomState geometryTileZoomState;
};

} // namespace mbgl
