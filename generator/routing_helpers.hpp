#pragma once

#include "base/geo_object_id.hpp"

#include <cstdint>
#include <map>
#include <string>

namespace routing
{
// Adds feature id and corresponding |osmId| to |osmIdToFeatureId|.
// Note. In general, one |featureId| may correspond to several osm ids.
// But for a road feature |featureId| corresponds to exactly one osm id.
void AddFeatureId(base::GeoObjectId osmId, uint32_t featureId,
                  std::map<base::GeoObjectId, uint32_t> & osmIdToFeatureId);

// Parses comma separated text file with line in following format:
// <feature id>, <osm id 1 corresponding feature id>, <osm id 2 corresponding feature id>, and so
// on
// For example:
// 137999, 5170186,
// 138000, 5170209, 5143342,
// 138001, 5170228,
bool ParseRoadsOsmIdToFeatureIdMapping(std::string const & osmIdsToFeatureIdPath,
                                       std::map<base::GeoObjectId, uint32_t> & osmIdToFeatureId);
bool ParseRoadsFeatureIdToOsmIdMapping(std::string const & osmIdsToFeatureIdPath,
                                       std::map<uint32_t, base::GeoObjectId> & featureIdToOsmId);
}  // namespace routing
