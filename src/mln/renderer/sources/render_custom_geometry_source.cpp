#include <mln/renderer/sources/render_custom_geometry_source.hpp>
#include <mln/renderer/render_tile.hpp>
#include <mln/renderer/paint_parameters.hpp>
#include <mln/tile/custom_geometry_tile.hpp>
#include <mln/tile/native_tile_cache.hpp>

namespace mln {

using namespace style;

RenderCustomGeometrySource::RenderCustomGeometrySource(Immutable<style::CustomGeometrySource::Impl> impl_,
                                                       const TaggedScheduler& threadPool_)
    : RenderTileSource(std::move(impl_), threadPool_) {
    tilePyramid.setObserver(this);
}

const style::CustomGeometrySource::Impl& RenderCustomGeometrySource::impl() const {
    return static_cast<const style::CustomGeometrySource::Impl&>(*baseImpl);
}

void RenderCustomGeometrySource::reduceNativeTileMemoryUse() {
  const auto& options = *impl().getTileOptions();
  if (options.dataType == CustomGeometrySource::TileDataType::NativeGeometry
      && options.nativeTileCache && options.nativeTileCache->enabled()) {
    tilePyramid.reduceMemoryUse();
  }
}

void RenderCustomGeometrySource::update(Immutable<style::Source::Impl> baseImpl_,
                                        const std::vector<Immutable<style::LayerProperties>>& layers,
                                        const bool needsRendering,
                                        const bool needsRelayout,
                                        const TileParameters& parameters) {
    if (baseImpl != baseImpl_) {
        std::swap(baseImpl, baseImpl_);

        // Clear tile pyramid only if updated source has different tile options,
        // zoom range or initialization state for a custom tile loader.
        auto newImpl = staticImmutableCast<style::CustomGeometrySource::Impl>(baseImpl);
        auto currentImpl = staticImmutableCast<style::CustomGeometrySource::Impl>(baseImpl_);
        if (*newImpl != *currentImpl) {
            tilePyramid.clearAll();
        }
    }

    enabled = needsRendering;

    auto tileLoader = impl().getTileLoader();
    if (!tileLoader) {
        return;
    }

    tilePyramid.update(layers,
                       needsRendering,
                       needsRelayout,
                       parameters,
                       *baseImpl,
                       impl().getTileOptions()->tileSize,
                       impl().getZoomRange(),
                       {},
                       [&](const OverscaledTileID& tileID, TileObserver* observer_) {
                           return std::make_unique<CustomGeometryTile>(
                               tileID, impl().id, parameters, impl().getTileOptions(), *tileLoader, observer_,
                               impl().getNativeSourceEpoch());
                       });
}

} // namespace mln
