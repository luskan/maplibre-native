#pragma once

#include <mln/style/source.hpp>
#include <mln/util/geo.hpp>
#include <mln/util/geojson.hpp>
#include <mln/util/range.hpp>
#include <mln/util/constants.hpp>
#include <mln/style/native_tile_request.hpp>

#include <memory>
#include <mln/util/tile_trace.hpp>

namespace mln {

class OverscaledTileID;
class CanonicalTileID;
template <class T>
class Actor;
class ThreadPool;
class GeometryMemoObserver;
class NativeTilePayload;
class NativeRequestState;
class RenderOptimizationState;

namespace style {

using TileFunction = std::function<void(const CanonicalTileID&)>;

// Callbacks run on the loader thread and must not wait for its actor.
// The resolver only reads immutable input metadata and must not perform I/O.
struct NativeTileCallbacks
{
    std::function<NativeRequestBinding(const CanonicalTileID&)> resolve;
    std::function<void(const CanonicalTileID&, const NativeRequestTicket&, tiletrace::Context)> fetch;
    std::function<void(const CanonicalTileID&, const NativeRequestTicket&)> cancel;
};

class CustomTileLoader;

// NOTE: Any derived class must invalidate `weakFactory` in the destructor
class CustomGeometrySource final : public Source {
public:
    enum class TileDataType { LegacyFeatures, NativeGeometry };
    struct TileOptions {
        double tolerance = 0.375;
        uint16_t tileSize = util::tileSize_I;
        uint16_t buffer = 128;
        bool clip = false;
        bool wrap = false;
        tiletrace::ID traceMap = 0, traceSource = 0;
        // These options affect geometry retention and diagnostics, not conversion output.
        bool memoizeGeometry = false;
        std::shared_ptr<GeometryMemoObserver> geometryMemoObserver = nullptr;
        TileDataType dataType = TileDataType::LegacyFeatures;
        std::shared_ptr<RenderOptimizationState> renderOptimizations;
    };

    struct Options {
        TileFunction fetchTileFunction;
        TileFunction cancelTileFunction;
        Range<uint8_t> zoomRange = {0, 18};
        TileOptions tileOptions;
        std::function<void(const CanonicalTileID&, tiletrace::Context)> tracedFetch;
        NativeTileCallbacks nativeCallbacks;
    };

public:
    CustomGeometrySource(std::string id, const CustomGeometrySource::Options& options);
    ~CustomGeometrySource() final;
    void loadDescription(FileSource&) final;
    void setTileData(const CanonicalTileID&, const GeoJSON&);
    void setTileFeatures(const CanonicalTileID&, const std::shared_ptr<const FeatureCollection>&);
    void setTracedTileFeatures(const CanonicalTileID&, const std::shared_ptr<const FeatureCollection>&,
                              tiletrace::Context);
    void setTracedTilePayload(const CanonicalTileID&, const NativeRequestTicket&,
                             std::shared_ptr<const NativeTilePayload>, tiletrace::Context = {});
    void setNativeTileError(const CanonicalTileID&, const NativeRequestTicket&,
                           std::exception_ptr, tiletrace::Context = {});
    void invalidateTile(const CanonicalTileID&);
    void invalidateRegion(const LatLngBounds&);
    void clearTileCache();
    // Private implementation
    class Impl;
    const Impl& impl() const;
    bool supportsLayerType(const mln::style::LayerTypeInfo*) const override;
    mapbox::base::WeakPtr<Source> makeWeakPtr() override { return weakFactory.makeWeakPtr(); }

protected:
    Mutable<Source::Impl> createMutable() const noexcept final;

private:
    CustomGeometrySource(std::string, const Options&, std::shared_ptr<NativeRequestState>);
    std::shared_ptr<ThreadPool> threadPool;
    std::shared_ptr<NativeRequestState> nativeState;
    std::unique_ptr<Actor<CustomTileLoader>> loader;
    mapbox::base::WeakPtrFactory<Source> weakFactory{this};
    // Do not add members here, see `WeakPtrFactory`
};

template <>
inline bool Source::is<CustomGeometrySource>() const {
    return getType() == SourceType::CustomVector;
}

} // namespace style
} // namespace mln
