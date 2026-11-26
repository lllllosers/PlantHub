#pragma once

#include <array>
#include <cstdint>

namespace planthub::board {

inline constexpr int i2cSdaGpio = 8;
inline constexpr int i2cSclGpio = 9;
inline constexpr std::uint32_t i2cFrequencyHz = 100'000;
inline constexpr std::uint8_t ads1115Address = 0x48;
inline constexpr std::uint8_t sht31CandidateAddress = 0x44;
inline constexpr std::array<int, 2> soilAdcChannels{0, 1};
inline constexpr std::array<int, 2> pumpGpios{4, 5};

inline constexpr std::array<int, 2> reservedSoilAdcChannels{2, 3};
inline constexpr std::array<int, 2> reservedPumpGpios{10, 11};
inline constexpr int reservedLowWaterGpio = 6;
inline constexpr int reservedLeakGpio = 7;
inline constexpr std::uint8_t reservedDs3231Address = 0x68;

}  // namespace planthub::board
