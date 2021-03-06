project(geometry)

set(
  SRC
  algorithm.cpp
  algorithm.hpp
  angles.cpp
  angles.hpp
  any_rect2d.hpp
  bounding_box.cpp
  bounding_box.hpp
  calipers_box.cpp
  calipers_box.hpp
  cellid.hpp
  convex_hull.cpp
  convex_hull.hpp
  covering_utils.hpp
  diamond_box.cpp
  diamond_box.hpp
  distance_on_sphere.cpp
  distance_on_sphere.hpp
  latlon.cpp
  latlon.hpp
  line2d.cpp
  line2d.hpp
  mercator.cpp
  mercator.hpp
  meter.hpp
  parametrized_segment.hpp
  point2d.hpp
  polygon.hpp
  rect2d.hpp
  rect_intersect.hpp
  region2d.hpp
  robust_orientation.cpp
  robust_orientation.hpp
  screenbase.cpp
  screenbase.hpp
  segment2d.cpp
  segment2d.hpp
  simplification.hpp
  transformations.hpp
  tree4d.hpp
  triangle2d.cpp
  triangle2d.hpp

  region2d/binary_operators.cpp
  region2d/binary_operators.hpp
  region2d/boost_concept.hpp
)

geocore_add_library(${PROJECT_NAME} ${SRC})

geocore_add_test_subdirectory(geometry_tests)
