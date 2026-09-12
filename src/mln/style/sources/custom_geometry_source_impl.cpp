#include <mln/style/sources/custom_geometry_source_impl.hpp>
#include <mln/style/source_observer.hpp>

namespace mln {
namespace style {

CustomGeometrySource::Impl::Impl(std::string id_, const CustomGeometrySource::Options& options, uint64_t nativeSourceEpoch_)
    : Source::Impl(SourceType::CustomVector, std::move(id_)),
      tileOptions(makeMutable<CustomGeometrySource::TileOptions>(options.tileOptions)),
      zoomRange(options.zoomRange),
      loaderRef({}), nativeSourceEpoch(nativeSourceEpoch_) {}

CustomGeometrySource::Impl::Impl(const Impl& impl, const ActorRef<CustomTileLoader>& loaderRef_)
    : Source::Impl(impl),
      tileOptions(impl.tileOptions),
      zoomRange(impl.zoomRange),
      loaderRef(loaderRef_), nativeSourceEpoch(impl.nativeSourceEpoch) {}

bool CustomGeometrySource::Impl::operator!=(const Impl& other) const noexcept {
    return tileOptions != other.tileOptions || zoomRange != other.zoomRange || bool(loaderRef) != bool(other.loaderRef)
        || nativeSourceEpoch != other.nativeSourceEpoch;
}

std::optional<std::string> CustomGeometrySource::Impl::getAttribution() const {
    return {};
}

Immutable<CustomGeometrySource::TileOptions> CustomGeometrySource::Impl::getTileOptions() const {
    return tileOptions;
}

Range<uint8_t> CustomGeometrySource::Impl::getZoomRange() const {
    return zoomRange;
}

std::optional<ActorRef<CustomTileLoader>> CustomGeometrySource::Impl::getTileLoader() const {
    return loaderRef;
}

} // namespace style
} // namespace mln
