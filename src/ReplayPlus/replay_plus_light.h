#pragma once
#include <array>
#include <cstdint>
#include <string>

// The camera light as a real light of the game's: a spot light made through the game's own light renderer, as the
// game makes the spot lights of its levels (the PbrSpotLight entity's way, replay_plus_light.cpp), on the replay
// camera and pointing where it looks. The game lights the world with it as with its own lights (every surface, its
// shading and shadows, in exported videos as on the screen). (Until 1.3.2 it could be drawn over the picture instead,
// from the game's depth, which could only brighten what the picture showed: that light is gone.)
namespace dingosdk::replay_plus::light {
struct Wanted {
    bool on{};
    float lumens{12000};         // its brightness, in the renderer's units (the game's own lamps: 10 000 to 25 000 000)
    float range{10};             // metres it reaches (its attenuation radius)
    float outer{70}, inner{35};  // its beam and the bright core inside it, degrees across
    std::array<float, 3> colour{1, 1, 1};
    bool shadows{true};          // it casts shadows (behind what it lights, from just above the lens)
    // Where it is and which way it shines: the game's transform rows (right, up, forward, position), the 4th of each
    // row 0 but the position's (1).
    std::array<float, 16> transform{};
};
// Game thread only (the client tick). Makes, moves, changes or takes away the light as `wanted` says. False when the
// game's light renderer is not one this game build has (nothing is made then).
bool update(std::uintptr_t base, const Wanted& wanted) noexcept;
// The light taken away (the replay editor closed, a level going, the DLL stopping). Game thread.
void remove(std::uintptr_t base) noexcept;
// Whether this game build's light renderer is the one known (checked once).
bool usable(std::uintptr_t base) noexcept;
// Whether the light is made now.
bool exists() noexcept;
// What it is doing, for `replayplus status`.
std::string status();
// Research (test builds): the game's own light makes logged from now on.
void watch_makes(std::uintptr_t base) noexcept;
// Research: made again with these B marks and description flag (-1: as it is).
void research(std::uintptr_t base, std::uint32_t marks_b, int flag51) noexcept;
} // namespace dingosdk::replay_plus::light
