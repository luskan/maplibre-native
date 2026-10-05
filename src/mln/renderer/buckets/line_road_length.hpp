#pragma once

#include <mln/renderer/buckets/line_bucket.hpp>

namespace mln
{
class LineRoadLengthFeature final : public GeometryTileFeature
{
public:
  LineRoadLengthFeature(GeometryTileFeature const& feature, GeometryCollection const& geometry,
    LineBucket::VertexVector const& vertices, std::size_t first, std::size_t last)
    : feature_(feature), geometry_(geometry), vertices_(vertices), first_(first), last_(last)
  {
  }

  FeatureType getType() const override { return feature_.getType(); }
  FeatureIdentifier getID() const override { return feature_.getID(); }
  PropertyMap const& getProperties() const override { return feature_.getProperties(); }
  GeometryCollection const& getGeometries() const override { return geometry_; }

  std::optional<Value> getValue(std::string const& key) const override
  {
    if(key != "__road_profile_length")
      return feature_.getValue(key);
    if(!scanned_)
    {
      scanned_ = true;
      if(getType() != FeatureType::LineString || geometry_.size() != 1 || first_ >= last_
        || last_ > vertices_.elements() || last_ - first_ > 65535
        || feature_.getValue("mapbox_clip_start") || feature_.getValue("mapbox_clip_end"))
        return length_;
      unsigned previous = 0;
      for(std::size_t i = first_; i < last_; ++i)
      {
        auto const& data = vertices_.at(i).a2;
        unsigned const distance = ((unsigned(data[2]) >> 2) | (unsigned(data[3]) << 6)) * 2;
        if(distance < previous || distance >= 16384 || (i == first_ && distance != 0))
          return length_;
        previous = distance;
      }
      length_ = double(previous);
    }
    return length_;
  }

private:
  GeometryTileFeature const& feature_;
  GeometryCollection const& geometry_;
  LineBucket::VertexVector const& vertices_;
  std::size_t first_, last_;
  mutable bool scanned_ = false;
  mutable double length_ = 0;
};
} // namespace mln
