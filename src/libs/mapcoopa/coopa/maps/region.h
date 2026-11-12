/**
 * @file region.h
 * @brief Convenience include for the political geography types.
 *
 * `MapRegion` and `MapCountry` are declared in `map_data.h` alongside
 * `MapTown` and the `MapGraph` that owns them all -- they are map data, and
 * splitting them out would mean `map_data.h` including this header while this
 * header needed its id types. This header exists so a consumer working only
 * with political geography has an obvious thing to include.
 */

#ifndef COOPA_MAPS_REGION_H
#define COOPA_MAPS_REGION_H

#include <coopa/maps/map_data.h>

#endif // COOPA_MAPS_REGION_H
