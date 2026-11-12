#pragma once

#include <cstdint>
#include <string_view>

namespace stallion {

struct SpecialForms {
    const char* const* names;
    int count;
};

// Any skin with a form list (mGearSkinUpgrades) gets form buttons; the game
// reports how many forms the loaded skin has. These are the names of known
// lists, in the game's order, and the skins whose first form is their normal
// look, so a switch preselects it. Gun Goddess MF and God-King Garen have a
// live test on 16.21.
constexpr int kMaxForms = 16;

inline SpecialForms special_forms(std::string_view champion, std::uint32_t skin) {
    static const char* const mf[] = {"Scarlet Fair", "Zero Hour", "Royal Arms", "Starswarm"};
    static const char* const kat[] = {"Dagger 1", "Dagger 2", "Dagger 3", "Dagger 4", "Dagger 5", "Dagger 6"};
    static const char* const renekton[] = {"Head off", "Head on", "Fins", "Ultimate"};
    static const char* const ezreal[] = {"Level 1", "Level 2", "Level 3"};
    // Elementalist Lux shows every element's parts until a form is equipped.
    static const char* const lux[] = {"Light", "Air", "Dark", "Fire", "Ice",
                                      "Magma", "Mystic", "Nature", "Storm", "Water"};
    if ((champion == "Miss Fortune" || champion == "MissFortune") && skin == 16) return {mf, 4};
    if (champion == "Katarina" && skin >= 29 && skin <= 36) return {kat, 6};
    if (champion == "Renekton" && skin >= 26 && skin <= 32) return {renekton, 4};
    if (champion == "Ezreal" && skin == 5) return {ezreal, 3};
    if (champion == "Lux" && skin == 7) return {lux, 10};
    return {nullptr, 0};
}

// Names for form lists the switch does not preselect, because the game shows
// that form only during play (Jax 14 starts with its wings hidden).
inline SpecialForms form_names(std::string_view champion, std::uint32_t skin) {
    static const char* const jax[] = {"Wings"};
    if (champion == "Jax" && skin == 14) return {jax, 1};
    return special_forms(champion, skin);
}

inline bool valid_gear(std::string_view champion, std::uint32_t skin, int gear) {
    if (gear == -1) return true;
    const auto forms = special_forms(champion, skin);
    return gear >= 0 && gear < forms.count;
}

struct ClipAlias {
    const char* missing;
    const char* replacement;
};

struct ClipAliases {
    const ClipAlias* items;
    int count;
};

// The server chooses animation names for the skin it assigned at match start.
// After a live switch it still sends the default skin's names, and the client
// falls back to the run/idle clip when the new skin's graph lacks one.
// A 16.21 survey of all 1650 champion skin graphs against their default graph
// found these gameplay clips missing with a clear replacement:
// - God-King Garen (13, 44): E spin, including the second sword, is
//   "Spell3_Med"/"Spell3_Fast" instead of "Spell3".
// - Leona 8: W is "Spell2_open" (the skin's copy of leona_spell2.anm).
// - Jax 14: the E spin is "Spell3_Idle" (jax_skin14_spell3_spin.anm).
// The other gaps (DJ Sona, Sejuani 3, Akali 61 R, Anivia 5 Q, Kayn 20 E, Miss
// Fortune 16 W, Jinx 60 Q, Master Yi 116) have no clip or no single equivalent
// and are left alone.
inline ClipAliases clip_aliases(std::string_view champion, std::uint32_t skin) {
    static const ClipAlias god_king[] = {{"Spell3", "Spell3_Med"}};
    static const ClipAlias leona[] = {{"Spell2", "Spell2_open"}};
    static const ClipAlias jax[] = {{"Spell3", "Spell3_Idle"}};
    if (champion == "Garen" && (skin == 13 || skin == 44)) return {god_king, 1};
    if (champion == "Leona" && skin == 8) return {leona, 1};
    if (champion == "Jax" && skin == 14) return {jax, 1};
    return {nullptr, 0};
}

} // namespace stallion
