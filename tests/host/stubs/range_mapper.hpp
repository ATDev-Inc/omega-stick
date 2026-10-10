#pragma once

// Host-test stand-in for espp/math range_mapper: only the Config struct is
// needed, since omega_calibration builds these and hands them to Joystick.

namespace espp {

template <typename T> class RangeMapper {
public:
  struct Config {
    T center;
    T center_deadband = 0;
    T minimum;
    T maximum;
    T range_deadband = 0;
    T output_center = 0;
    T output_range = 1;
    bool invert_output = false;
  };
};

typedef RangeMapper<float> FloatRangeMapper;
typedef RangeMapper<int> IntRangeMapper;

} // namespace espp
