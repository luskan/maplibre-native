#include <mln/renderer/render_tile.hpp>
#include <mln/renderer/source_state.hpp>
#include <mln/style/conversion_impl.hpp>
#include <mln/util/instrumentation.hpp>
#include <mln/util/logging.hpp>

#include <bit>

namespace mln {
namespace {

bool sameScalarState(const Value& current, const Value& next) {
    if (const auto* number = current.getDouble()) {
        const auto* other = next.getDouble();
        return other && std::bit_cast<uint64_t>(*number) == std::bit_cast<uint64_t>(*other);
    }
    return !current.getArray() && !current.getObject() && current == next;
}

} // namespace

void SourceFeatureState::updateState(const std::optional<std::string>& sourceLayerID,
                                     const std::string& featureID,
                                     const FeatureState& newState) {
    std::string sourceLayer = sourceLayerID.value_or(std::string());
    for (const auto& state : newState) {
        auto& layerStates = stateChanges[sourceLayer];
        auto& featureStates = layerStates[featureID];
        featureStates[state.first] = state.second;
    }
}

void SourceFeatureState::getState(FeatureState& result,
                                  const std::optional<std::string>& sourceLayerID,
                                  const std::string& featureID) const {
    std::string sourceLayer = sourceLayerID.value_or(std::string());
    FeatureState current;
    FeatureState changes;
    auto layerStates = currentStates.find(sourceLayer);
    if (layerStates != currentStates.end()) {
        const auto currentStateEntry = layerStates->second.find(featureID);
        if (currentStateEntry != layerStates->second.end()) {
            current = currentStateEntry->second;
        }
    }

    layerStates = stateChanges.find(sourceLayer);
    if (layerStates != stateChanges.end()) {
        const auto stateChangesEntry = layerStates->second.find(featureID);
        if (stateChangesEntry != layerStates->second.end()) {
            changes = stateChangesEntry->second;
        }
    }
    result = std::move(changes);
    result.insert(current.begin(), current.end());
}

void SourceFeatureState::coalesceChanges(std::vector<RenderTile>& tiles) {
    MLN_TRACE_FUNC();

    LayerFeatureStates changes;
    for (const auto& [sourceLayer, layerUpdates] : stateChanges) {
        auto& currentLayer = currentStates[sourceLayer];
        for (const auto& [featureID, updates] : layerUpdates) {
            auto& current = currentLayer[featureID];
            bool changed = false;
            for (const auto& [key, value] : updates) {
                auto [entry, inserted] = current.try_emplace(key, value);
                if (inserted) {
                    changed = true;
                } else if (!sameScalarState(entry->second, value)) {
                    entry->second = value;
                    changed = true;
                }
            }
            if (changed) {
                changes[sourceLayer].emplace(featureID, current);
            }
        }
    }

    for (const auto& [sourceLayer, removals] : deletedStates) {
        const auto layer = currentStates.find(sourceLayer);
        if (layer == currentStates.end()) {
            continue;
        }
        auto& features = layer->second;
        if (removals.empty()) {
            auto& removed = changes[sourceLayer];
            for (const auto& [id, state] : features) {
                removed[id].clear();
            }
            currentStates.erase(layer);
            continue;
        }
        for (const auto& [id, keys] : removals) {
            const auto feature = features.find(id);
            if (feature == features.end()) {
                continue;
            }
            auto& current = feature->second;
            bool changed = keys.empty();
            if (changed) {
                current.clear();
            } else {
                for (const auto& [key, value] : keys) {
                    changed |= current.erase(key) != 0;
                }
            }
            if (changed) {
                changes[sourceLayer].insert_or_assign(id, current);
                if (current.empty()) {
                    features.erase(feature);
                }
            }
        }
        if (features.empty()) {
            currentStates.erase(layer);
        }
    }

    stateChanges.clear();
    deletedStates.clear();

    if (!changes.empty()) {
        ++version;
    }

    // A tile that holds the previous version gets this frame's changes. A tile with new buckets, or one that was
    // not rendered in a frame with changes, gets the whole state. A tile that is up to date gets nothing.
    for (auto& tile : tiles) {
        auto& tileVersion = tile.featureStateVersion();
        if (tileVersion == version) {
            continue;
        }
        const bool previous = !changes.empty() && tileVersion != 0 && tileVersion + 1 == version;
        if (previous) {
            tile.setFeatureState(changes, FeatureStateUpdate::Delta);
        } else {
            tile.setFeatureState(currentStates, FeatureStateUpdate::Snapshot);
        }
        tileVersion = version;
    }
}

void SourceFeatureState::removeState(const std::optional<std::string>& sourceLayerID,
                                     const std::optional<std::string>& featureID,
                                     const std::optional<std::string>& stateKey) {
    std::string sourceLayer = sourceLayerID.value_or(std::string());

    bool sourceLayerDeleted = deletedStates.contains(sourceLayer) && deletedStates[sourceLayer].empty();
    if (sourceLayerDeleted) {
        return;
    }

    if (stateKey && featureID) {
        auto [feature, inserted] = deletedStates[sourceLayer].try_emplace(*featureID);
        if (inserted || !feature->second.empty()) {
            feature->second.try_emplace(*stateKey, Value{});
        }
    } else if (featureID) {
        bool updateInQueue = stateChanges.contains(sourceLayer) && stateChanges[sourceLayer].contains(*featureID);
        if (updateInQueue) {
            for (const auto& changeEntry : stateChanges[sourceLayer][*featureID]) {
                deletedStates[sourceLayer][*featureID][changeEntry.first] = {};
            }
        } else {
            deletedStates[sourceLayer][*featureID] = {};
        }
    } else {
        deletedStates[sourceLayer] = {};
    }
}

} // namespace mln
