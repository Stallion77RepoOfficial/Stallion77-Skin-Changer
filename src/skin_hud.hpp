#pragma once

#include <CoreGraphics/CoreGraphics.h>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#ifdef __OBJC__
@class NSWindow;
#else
typedef struct objc_object NSWindow;
#endif

namespace stallion {

// A skin whose own HUD is a plain form picker. The game builds it only for a
// skin chosen before the match, so Stallion redraws it from the game's own
// layout and textures, as CommunityDragon publishes them for the patch.
struct SkinHudSpec {
    const char* layout;         // UI bin (JSON) below CommunityDragon's game/
    const char* menu_scene;     // scene that holds the form buttons
    const char* button_format;  // name of the button for form i (printf)
    const char* toggle;         // button that opens and closes the menu
    int forms;
};

const SkinHudSpec* skin_hud_spec(std::string_view champion, unsigned skin);

// Textures (PNG, below game/) that a layout's drawn elements use.
std::vector<std::string> skin_hud_textures(const std::string& layout_json,
                                           const SkinHudSpec& spec);

// The HUD scale factor for the game's [HUD] GlobalScale setting.
double skin_hud_scale(double global_scale);

class SkinHud {
public:
    SkinHud();
    ~SkinHud();
    SkinHud(const SkinHud&) = delete;
    SkinHud& operator=(const SkinHud&) = delete;

    // Builds the HUD from a layout and its cached textures; false with a reason.
    bool load(const std::string& layout_json, const SkinHudSpec& spec,
              const std::filesystem::path& texture_root, std::string& error);
    bool loaded() const;
    const SkinHudSpec* spec() const;

    // Lays the elements out over the game area (global, top-left origin) the
    // way the game does, moved by offset (layout units). Returns true when the
    // window frame changed.
    bool layout(CGRect game_area, double hud_scale, CGPoint offset);
    CGRect frame() const;                // window frame, global coordinates
    double layout_scale() const;         // points per layout unit
    // Hit regions in global coordinates: form buttons 0..forms-1 while the menu
    // is open (empty rectangles while closed), then the toggle.
    std::vector<CGRect> regions() const;
    int toggle_region() const;

    // Each setter returns whether the drawing changed.
    bool set_form(int form);
    bool set_hover(int region);
    bool set_pressed(int region);
    bool set_menu_open(bool open);
    bool menu_open() const;
    // The game draws its cursor into its own frame, under this window. While the
    // pointer is over a drawn element, draw one at that global point.
    bool set_cursor(bool enabled, CGPoint global);

    NSWindow* window() const;
    void redraw();

    struct State;
private:
    std::unique_ptr<State> state_;
};

}  // namespace stallion
