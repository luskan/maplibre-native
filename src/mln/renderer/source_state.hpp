#pragma once

#include <mln/style/conversion.hpp>
#include <mln/util/feature.hpp>

namespace mln {

class RenderTile;
enum class FeatureStateUpdate { Delta, Snapshot };

class SourceFeatureState {
public:
    SourceFeatureState() = default;
    ~SourceFeatureState() = default;

    void updateState(const std::optional<std::string>& sourceLayerID,
                     const std::string& featureID,
                     const FeatureState& newState);
    void getState(FeatureState& result,
                  const std::optional<std::string>& sourceLayerID,
                  const std::string& featureID) const;
    void removeState(const std::optional<std::string>& sourceLayerID,
                     const std::optional<std::string>& featureID,
                     const std::optional<std::string>& stateKey);

    void coalesceChanges(std::vector<RenderTile>& tiles);

private:
    // Only live features are stored here. Empty removals belong to the frame delta.
    LayerFeatureStates currentStates;
    LayerFeatureStates stateChanges;
    LayerFeatureStates deletedStates;
    // Bumped in each coalesceChanges call that has changes.
    uint64_t version = 0;
};

} // namespace mln
