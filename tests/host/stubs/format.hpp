#pragma once

// Host-test stand-in for espp/format. The only fmt use the headers under test
// make is espp's hid-rp gamepad report befriending fmt::formatter, which just
// needs the primary template declared. Nothing here formats through fmt.

namespace fmt {
template <typename T, typename Char = char, typename Enable = void> struct formatter;
} // namespace fmt
