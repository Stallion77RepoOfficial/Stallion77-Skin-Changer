#pragma once

#include <cstdint>
#include <string_view>
#include <array>

namespace stallion {
    struct Offsets {
        std::uint64_t frame = 0;
        std::uint64_t setup = 0;
        std::uint64_t preload = 0;
        std::uint64_t actor_refresh = 0;
        std::uint64_t character_update = 0;
        std::uint64_t character_prepare = 0;
        std::uint64_t actor_global = 0;
        std::uint64_t refresh = 0;
        std::uint32_t champion_name = 0;
        std::uint32_t skin_id = 0;
        // id = ror(key ^ ror(ror(field, pre_ror) + add, field_ror), value_ror).
        // Arithmetic wraps at 32 bits. All zeros mean a plain field.
        std::uint32_t skin_key = 0;
        std::uint32_t skin_field_ror = 0;
        std::uint32_t skin_value_ror = 0;
        std::uint32_t skin_pre_ror = 0;
        std::uint32_t skin_add = 0;
        std::uint32_t stack = 0;
        std::uint32_t gear = 0;
        std::uint32_t character_record = 0;
        std::uint32_t animation_slot = 0;
        std::uint32_t animation_graph = 0;
        std::uint32_t clip_map = 0;
        std::uint64_t operator_new = 0;
        // [[actor + skin_resource] + skin_properties] + gear_list is the skin's
        // list of forms (mGearSkinUpgrades); its count follows at +8.
        std::uint32_t skin_resource = 0;
        std::uint32_t skin_properties = 0;
        std::uint32_t gear_list = 0;
        std::array<unsigned char, 16> uuid{};
    };

    auto apply_live_skin(std::uint32_t pid, std::uint32_t skin_id,
                         const Offsets& offsets,
                         std::string_view expected_champion = {},
                         int selected_gear = -1) -> int;
    // Re-applies the selected form and animation fixes of the skin the local
    // actor already wears, without switching skins. The form is checked
    // against the skin's own form list in the game. Returns 3 while the skin's
    // animations have not loaded yet, so the caller can try again.
    auto apply_live_form(std::uint32_t pid, std::uint32_t skin_id,
                         const Offsets& offsets,
                         std::string_view expected_champion,
                         int selected_gear = -1) -> int;
    struct LiveSkinState {
        std::uint64_t actor = 0;
        std::uint32_t skin_id = 0;
        std::uint32_t stack_skin = 0;
        std::int8_t gear = -1;
        std::uint64_t character_record = 0;
        std::uint32_t character_record_skin = 0;
        std::uint64_t stack_record = 0;
        std::uint32_t stack_record_skin = 0;
    };
    auto read_live_skin_state(std::uint32_t pid, const Offsets& offsets,
                              std::string_view expected_champion,
                              LiveSkinState& state) -> int;
    void emergency_resume_live_skin() noexcept;
}
