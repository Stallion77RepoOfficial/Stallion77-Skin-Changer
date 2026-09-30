#pragma once

#include <cstdint>
#include <string_view>
#include <array>

namespace stallion {
    struct Offsets {
        std::uint64_t frame = 0;
        std::uint64_t setup = 0;
        std::uint64_t actor_global = 0;
        std::uint32_t champion_name = 0;
        std::uint32_t skin_id = 0;
        std::array<unsigned char, 16> uuid{};
    };

    auto apply_live_skin(std::uint32_t pid, std::uint32_t skin_id,
                         const Offsets& offsets,
                         std::string_view expected_champion = {}) -> int;
    struct LiveSkinState {
        std::uint64_t actor = 0;
        std::uint32_t skin_id = 0;
    };
    auto read_live_skin_state(std::uint32_t pid, const Offsets& offsets,
                              std::string_view expected_champion,
                              LiveSkinState& state) -> int;
    void emergency_resume_live_skin() noexcept;
}
