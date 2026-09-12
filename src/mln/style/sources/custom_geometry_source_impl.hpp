#pragma once

#include <mln/style/source_impl.hpp>
#include <mln/style/sources/custom_geometry_source.hpp>
#include <mln/style/custom_tile_loader.hpp>
#include <mln/actor/actor_ref.hpp>

namespace mln {
namespace style {

class CustomGeometrySource::Impl : public Source::Impl {
public:
    Impl(std::string id, const CustomGeometrySource::Options& options, uint64_t nativeSourceEpoch = 0);
    Impl(const Impl&, const ActorRef<CustomTileLoader>&);

    std::optional<std::string> getAttribution() const final;

    Immutable<CustomGeometrySource::TileOptions> getTileOptions() const;
    Range<uint8_t> getZoomRange() const;
    std::optional<ActorRef<CustomTileLoader>> getTileLoader() const;
    uint64_t getNativeSourceEpoch() const noexcept { return nativeSourceEpoch; }
    bool operator!=(const Impl&) const noexcept;

private:
    Immutable<CustomGeometrySource::TileOptions> tileOptions;
    Range<uint8_t> zoomRange;
    std::optional<ActorRef<CustomTileLoader>> loaderRef;
    uint64_t nativeSourceEpoch;
};

} // namespace style
} // namespace mln
