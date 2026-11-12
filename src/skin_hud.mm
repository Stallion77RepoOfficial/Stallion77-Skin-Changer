#include "skin_hud.hpp"

#include <Cocoa/Cocoa.h>
#include <rapidjson/document.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <map>

namespace stallion {

const SkinHudSpec* skin_hud_spec(std::string_view champion, unsigned skin) {
    // Gun Goddess Miss Fortune: four weapon buttons in a frame above the
    // portrait, opened and closed by a round button on the portrait.
    static const SkinHudSpec gun_goddess = {
        "clientstates/gameplay/ux/lol/skins/missfortuneskin16/uibase.cdtb.bin.json",
        "MissFortuneGear_Menu", "MissFortuneGear%d_Button", "GearSelectionToggle", 4};
    if ((champion == "Miss Fortune" || champion == "MissFortune") && skin == 16) return &gun_goddess;
    return nullptr;
}

// The game's HUD scale setting maps 0..1 to a factor of 0.66..1 (the same
// conversion as the game's own settings code at 0x100c1df54 in 16.21).
double skin_hud_scale(double global_scale) {
    const double setting = std::clamp(global_scale, 0.0, 1.0);
    return setting + (1.0 - setting) * 0.66;
}

namespace {

struct Element {
    std::string scene;
    CGRect rect = CGRectZero;  // layout units of its source resolution
    double source_w = 1920, source_h = 1080, anchor_x = 0, anchor_y = 0;
    int layer = 0;
    int order = 0;             // position in the bin, for equal layers
    std::string texture;       // PNG below game/; empty for hit regions
    CGRect uv = CGRectZero;    // in a texture of uv_w x uv_h
    double uv_w = 0, uv_h = 0;
    NSImage* image = nil;
    CGRect global = CGRectZero;
};

struct Button {
    std::vector<std::string> normal, hover, clicked, current;
    std::string hit;
};

std::string leaf(const std::string& path) {
    const size_t slash = path.rfind('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

// "assets/.../atlas.tex" as CommunityDragon serves it: lowercase PNG.
std::string png_path(std::string texture) {
    std::transform(texture.begin(), texture.end(), texture.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const size_t dot = texture.rfind('.');
    return (dot == std::string::npos ? texture : texture.substr(0, dot)) + ".png";
}

bool number_pair(const rapidjson::Value& object, const char* key, double& a, double& b) {
    if (!object.IsObject() || !object.HasMember(key)) return false;
    const auto& list = object[key];
    if (!list.IsArray() || list.Size() != 2 || !list[0].IsNumber() || !list[1].IsNumber()) return false;
    a = list[0].GetDouble();
    b = list[1].GetDouble();
    return true;
}

double number(const rapidjson::Value& object, const char* key, double fallback) {
    return object.IsObject() && object.HasMember(key) && object[key].IsNumber()
        ? object[key].GetDouble() : fallback;
}

std::string text(const rapidjson::Value& object, const char* key) {
    return object.IsObject() && object.HasMember(key) && object[key].IsString()
        ? object[key].GetString() : std::string();
}

std::vector<std::string> display_list(const rapidjson::Value& button, const char* state) {
    std::vector<std::string> names;
    if (!button.HasMember(state) || !button[state].IsObject()) return names;
    const auto& value = button[state];
    if (!value.HasMember("DisplayElementList") || !value["DisplayElementList"].IsArray()) return names;
    for (const auto& name : value["DisplayElementList"].GetArray())
        if (name.IsString()) names.emplace_back(name.GetString());
    return names;
}

// Icons and hit regions with a position; animations are not redrawn.
bool parse_element(const rapidjson::Value& value, int order, Element& element) {
    const std::string type = text(value, "__type");
    if (type != "UiElementIconData" && type != "UiElementEffectDesaturateData" &&
        type != "UiElementRegionData") return false;
    if (!value.HasMember("Position") || !value["Position"].IsObject()) return false;
    const auto& position = value["Position"];
    if (!position.HasMember("UIRect") || !position["UIRect"].IsObject()) return false;
    const auto& rect = position["UIRect"];
    double x = 0, y = 0, w = 0, h = 0;
    if (!number_pair(rect, "Position", x, y) || !number_pair(rect, "Size", w, h) || w <= 0 || h <= 0)
        return false;
    element.scene = text(value, "Scene");
    element.rect = CGRectMake(x, y, w, h);
    element.source_w = number(rect, "SourceResolutionWidth", 1920);
    element.source_h = number(rect, "SourceResolutionHeight", 1080);
    if (position.HasMember("Anchors") && position["Anchors"].IsObject())
        number_pair(position["Anchors"], "Anchor", element.anchor_x, element.anchor_y);
    element.layer = static_cast<int>(number(value, "Layer", 0));
    element.order = order;
    if (type == "UiElementRegionData") return true;
    if (!value.HasMember("TextureData") || !value["TextureData"].IsObject()) return false;
    const auto& texture = value["TextureData"];
    const std::string name = text(texture, "mTextureName");
    if (name.empty() || !texture.HasMember("mTextureUV") || !texture["mTextureUV"].IsArray() ||
        texture["mTextureUV"].Size() != 4) return false;
    const auto& uv = texture["mTextureUV"];
    for (unsigned i = 0; i < 4; ++i) if (!uv[i].IsNumber()) return false;
    element.texture = png_path(name);
    element.uv = CGRectMake(uv[0].GetDouble(), uv[1].GetDouble(),
                            uv[2].GetDouble() - uv[0].GetDouble(), uv[3].GetDouble() - uv[1].GetDouble());
    element.uv_w = number(texture, "mTextureSourceResolutionWidth", 0);
    element.uv_h = number(texture, "mTextureSourceResolutionHeight", 0);
    return element.uv.size.width > 0 && element.uv.size.height > 0 &&
           element.uv_w > 0 && element.uv_h > 0;
}

}  // namespace

struct SkinHud::State {
    const SkinHudSpec* spec = nullptr;
    std::map<std::string, Element> elements;
    std::vector<std::string> menu_icons;  // drawn while the menu is open
    std::vector<Button> forms;
    Button toggle;
    bool has_toggle = false;
    CGRect frame = CGRectZero;
    double scale = 1;
    int form = -1, hover = -1, pressed = -1;
    bool menu_open = true;
    bool cursor = false;
    CGPoint cursor_at = CGPointZero;
    NSWindow* window = nil;
    NSView* view = nil;

    const Element* find(const std::string& name) const {
        auto it = elements.find(name);
        return it == elements.end() ? nullptr : &it->second;
    }
    void add_drawn(const std::vector<std::string>& names, std::vector<const Element*>& out) const {
        for (const auto& name : names)
            if (const Element* element = find(name); element && element->image) out.push_back(element);
    }
    const std::vector<std::string>& button_state(const Button& button, int region, bool current) const {
        if (current && !button.current.empty()) return button.current;
        if (pressed == region && !button.clicked.empty()) return button.clicked;
        if ((hover == region || pressed == region) && !button.hover.empty()) return button.hover;
        return button.normal;
    }
    std::vector<const Element*> drawn() const {
        std::vector<const Element*> out;
        if (menu_open) {
            add_drawn(menu_icons, out);
            for (size_t i = 0; i < forms.size(); ++i)
                add_drawn(button_state(forms[i], static_cast<int>(i), form == static_cast<int>(i)), out);
        }
        if (has_toggle) add_drawn(button_state(toggle, static_cast<int>(forms.size()), false), out);
        std::stable_sort(out.begin(), out.end(), [](const Element* a, const Element* b) {
            return a->layer != b->layer ? a->layer < b->layer : a->order < b->order;
        });
        return out;
    }
};

}  // namespace stallion

@interface StallionHudView : NSView
@property (nonatomic) stallion::SkinHud::State* hud;
@end

@implementation StallionHudView
- (BOOL)isFlipped { return YES; }
- (BOOL)isOpaque { return NO; }
- (void)drawRect:(NSRect)dirty {
    (void)dirty;
    [[NSColor clearColor] set];
    NSRectFill(self.bounds);
    if (!_hud) return;
    // The game scales these sprites up from a 512 px atlas; keep them smooth.
    NSDictionary* hints = @{NSImageHintInterpolation: @(NSImageInterpolationHigh)};
    const CGRect frame = _hud->frame;
    for (const stallion::Element* element : _hud->drawn()) {
        const NSRect rect = NSMakeRect(element->global.origin.x - frame.origin.x,
                                       element->global.origin.y - frame.origin.y,
                                       element->global.size.width, element->global.size.height);
        [element->image drawInRect:rect fromRect:NSZeroRect
                         operation:NSCompositingOperationSourceOver fraction:1.0
                    respectFlipped:YES hints:hints];
    }
    if (_hud->cursor) {
        NSCursor* arrow = [NSCursor arrowCursor];
        const NSSize size = arrow.image.size;
        const NSRect rect = NSMakeRect(_hud->cursor_at.x - frame.origin.x - arrow.hotSpot.x,
                                       _hud->cursor_at.y - frame.origin.y - arrow.hotSpot.y,
                                       size.width, size.height);
        [arrow.image drawInRect:rect fromRect:NSZeroRect
                      operation:NSCompositingOperationSourceOver fraction:1.0
                 respectFlipped:YES hints:nil];
    }
}
@end

namespace stallion {

std::vector<std::string> skin_hud_textures(const std::string& layout_json, const SkinHudSpec& spec) {
    (void)spec;
    rapidjson::Document doc;
    doc.Parse(layout_json.c_str());
    std::vector<std::string> textures;
    if (doc.HasParseError() || !doc.IsObject()) return textures;
    int order = 0;
    for (auto it = doc.MemberBegin(); it != doc.MemberEnd(); ++it) {
        Element element;
        if (parse_element(it->value, order++, element) && !element.texture.empty() &&
            std::find(textures.begin(), textures.end(), element.texture) == textures.end())
            textures.push_back(element.texture);
    }
    return textures;
}

SkinHud::SkinHud() : state_(std::make_unique<State>()) {}

SkinHud::~SkinHud() {
    for (auto& [name, element] : state_->elements) [element.image release];
    if (state_->window) {
        [state_->window orderOut:nil];
        [state_->window release];
    }
    [state_->view release];
}

bool SkinHud::load(const std::string& layout_json, const SkinHudSpec& spec,
                   const std::filesystem::path& texture_root, std::string& error) {
    rapidjson::Document doc;
    doc.Parse(layout_json.c_str());
    if (doc.HasParseError() || !doc.IsObject()) { error = "HUD layout is not valid JSON"; return false; }
    State& s = *state_;
    std::string menu_scene;
    int order = 0;
    for (auto it = doc.MemberBegin(); it != doc.MemberEnd(); ++it) {
        const std::string name = it->name.GetString();
        Element element;
        if (parse_element(it->value, order++, element)) s.elements[name] = element;
        if (text(it->value, "__type") == "UISceneData" && leaf(name) == spec.menu_scene) menu_scene = name;
    }
    if (menu_scene.empty()) { error = "HUD layout has no form menu"; return false; }
    // Buttons name their elements per state; the game shows the button of the
    // equipped form in its inactive state.
    s.forms.assign(spec.forms, Button{});
    std::vector<bool> found(spec.forms, false);
    std::vector<std::string> in_buttons;
    for (auto it = doc.MemberBegin(); it != doc.MemberEnd(); ++it) {
        if (text(it->value, "__type") != "UiElementGroupButtonData") continue;
        const auto& value = it->value;
        Button button{display_list(value, "DefaultStateElements"), display_list(value, "HoverStateElements"),
                      display_list(value, "ClickedStateElements"), display_list(value, "InactiveStateElements"),
                      text(value, "HitRegionElement")};
        for (const auto* list : {&button.normal, &button.hover, &button.clicked, &button.current})
            in_buttons.insert(in_buttons.end(), list->begin(), list->end());
        const std::string name = leaf(it->name.GetString());
        if (name == spec.toggle) {
            button.current.clear();
            s.toggle = button;
            s.has_toggle = true;
            continue;
        }
        if (text(value, "Scene") != menu_scene) continue;
        for (int i = 0; i < spec.forms; ++i) {
            char expected[96];
            std::snprintf(expected, sizeof expected, spec.button_format, i);
            if (name == expected) { s.forms[i] = button; found[i] = true; }
        }
    }
    for (int i = 0; i < spec.forms; ++i)
        if (!found[i] || !s.find(s.forms[i].hit)) { error = "HUD layout lacks a form button"; return false; }
    if (s.has_toggle && !s.find(s.toggle.hit)) s.has_toggle = false;
    for (const auto& [name, element] : s.elements)
        if (element.scene == menu_scene && !element.texture.empty() &&
            std::find(in_buttons.begin(), in_buttons.end(), name) == in_buttons.end())
            s.menu_icons.push_back(name);
    // Cut every drawn element out of its atlas.
    std::map<std::string, CGImageRef> atlases;
    bool ok = true;
    for (auto& [name, element] : s.elements) {
        if (element.texture.empty()) continue;
        CGImageRef& atlas = atlases[element.texture];
        if (!atlas) {
            const std::string path = (texture_root / element.texture).string();
            CGDataProviderRef provider = CGDataProviderCreateWithFilename(path.c_str());
            if (provider) {
                atlas = CGImageCreateWithPNGDataProvider(provider, nullptr, true, kCGRenderingIntentDefault);
                CGDataProviderRelease(provider);
            }
            if (!atlas) { error = "HUD texture missing: " + element.texture; ok = false; break; }
        }
        const double sx = CGImageGetWidth(atlas) / element.uv_w;
        const double sy = CGImageGetHeight(atlas) / element.uv_h;
        const CGRect cut = CGRectIntegral(CGRectMake(element.uv.origin.x * sx, element.uv.origin.y * sy,
                                                     element.uv.size.width * sx, element.uv.size.height * sy));
        if (CGImageRef part = CGImageCreateWithImageInRect(atlas, cut)) {
            element.image = [[NSImage alloc] initWithCGImage:part size:NSZeroSize];
            CGImageRelease(part);
        }
    }
    for (auto& [path, atlas] : atlases) if (atlas) CGImageRelease(atlas);
    if (!ok) return false;
    s.spec = &spec;

    NSWindow* window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 16, 16)
                                                   styleMask:NSWindowStyleMaskBorderless
                                                     backing:NSBackingStoreBuffered
                                                       defer:NO];
    [window setReleasedWhenClosed:NO];
    // Same layer and behavior as the overlay. Clicks reach the event tap, never
    // this window, so the game keeps focus.
    [window setLevel:2147483630];
    [window setHidesOnDeactivate:NO];
    [window setCollectionBehavior:NSWindowCollectionBehaviorCanJoinAllSpaces |
        NSWindowCollectionBehaviorFullScreenAuxiliary | NSWindowCollectionBehaviorStationary];
    [window setOpaque:NO];
    [window setBackgroundColor:[NSColor clearColor]];
    [window setHasShadow:NO];
    [window setIgnoresMouseEvents:YES];
    [window setAnimationBehavior:NSWindowAnimationBehaviorNone];
    StallionHudView* view = [[StallionHudView alloc] initWithFrame:NSMakeRect(0, 0, 16, 16)];
    view.hud = state_.get();
    [window setContentView:view];
    s.window = window;
    s.view = view;
    return true;
}

bool SkinHud::loaded() const { return state_->spec != nullptr; }
const SkinHudSpec* SkinHud::spec() const { return state_->spec; }

// Each element keeps its anchor on the game area and scales with the area's
// height relative to its source resolution, times the HUD scale.
bool SkinHud::layout(CGRect area, double hud_scale, CGPoint offset) {
    State& s = *state_;
    CGRect bounds = CGRectNull;
    for (auto& [name, element] : s.elements) {
        const double scale = area.size.height / element.source_h * hud_scale;
        s.scale = scale;
        const double x = area.origin.x + element.anchor_x * area.size.width +
            (element.rect.origin.x + offset.x - element.anchor_x * element.source_w) * scale;
        const double y = area.origin.y + element.anchor_y * area.size.height +
            (element.rect.origin.y + offset.y - element.anchor_y * element.source_h) * scale;
        element.global = CGRectMake(x, y, element.rect.size.width * scale, element.rect.size.height * scale);
        bounds = CGRectUnion(bounds, element.global);
    }
    const CGRect frame = CGRectIsNull(bounds) ? CGRectZero : CGRectIntegral(bounds);
    const bool changed = !CGRectEqualToRect(frame, s.frame);
    s.frame = frame;
    if (changed) {
        [s.window setContentSize:NSMakeSize(frame.size.width, frame.size.height)];
        [s.view setFrame:NSMakeRect(0, 0, frame.size.width, frame.size.height)];
    }
    return changed;
}

CGRect SkinHud::frame() const { return state_->frame; }
double SkinHud::layout_scale() const { return state_->scale; }

std::vector<CGRect> SkinHud::regions() const {
    const State& s = *state_;
    std::vector<CGRect> out;
    for (const Button& button : s.forms) {
        const Element* hit = s.find(button.hit);
        out.push_back(s.menu_open && hit ? hit->global : CGRectZero);
    }
    const Element* toggle = s.has_toggle ? s.find(s.toggle.hit) : nullptr;
    out.push_back(toggle ? toggle->global : CGRectZero);
    return out;
}

int SkinHud::toggle_region() const { return static_cast<int>(state_->forms.size()); }

static bool assign(int& field, int value) {
    if (field == value) return false;
    field = value;
    return true;
}
bool SkinHud::set_form(int form) { return assign(state_->form, form); }
bool SkinHud::set_hover(int region) { return assign(state_->hover, region); }
bool SkinHud::set_pressed(int region) { return assign(state_->pressed, region); }
bool SkinHud::set_menu_open(bool open) {
    if (state_->menu_open == open) return false;
    state_->menu_open = open;
    return true;
}
bool SkinHud::menu_open() const { return state_->menu_open; }

bool SkinHud::set_cursor(bool enabled, CGPoint global) {
    State& s = *state_;
    bool over = false;
    if (enabled)
        for (const Element* element : s.drawn())
            if (CGRectContainsPoint(element->global, global)) { over = true; break; }
    if (over == s.cursor && (!over || CGPointEqualToPoint(global, s.cursor_at))) return false;
    s.cursor = over;
    s.cursor_at = global;
    return true;
}
NSWindow* SkinHud::window() const { return state_->window; }

void SkinHud::redraw() {
    if (state_->view) [state_->view display];
}

}  // namespace stallion
