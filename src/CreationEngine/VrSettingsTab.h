#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

class ModToggle;
class ModSlider;
class ModCombo;

// (fix186) The mod's settings on a "VR" tab of the game's own Settings menu (the VR
// options in the game's menu without the unneeded player options; chosen: "A: in game Settings").
//
// Starfield 1.16.236 (two helpers' research of the exe and of the menu's ActionScript; Starfield-Luma, a mod for the same version,
// adds its own rows to the Display tab the same way): the Settings panel (the pause menu's and the main menu's) is drawn by the menu
// from two lists the game's SettingsDataModel (one object, [0x1461F7228]) keeps: its tabs ("SettingsCategoriesData", rebuilt with
// the game's 7 tabs, ids 0-6, each time Settings opens: 0x14151AE50) and its rows ("SettingsData", rebuilt for a tab each time it is
// asked for: 0x14151BFB0 first takes out that tab's rows, then calls the tab's own builder; above 6 there is none). The menu draws any
// tab it is given (only 4, Control Mappings, is a page of its own; any other id an options page) and shows the rows of the open tab;
// it asks for a tab's rows once per opening of Settings (Display each time: ours are asked for as our tab is added). Each change the
// player makes is sent to the game as an event (a checkbox, a slider, a stepper; Defaults; Back); the game's handlers find the setting
// by the low byte of its id (none of theirs above 0x5A) and ignore the others.
//
// Here: a tab with id 7 is added after the game's 7 (a hook right after the game pushes its last tab, inside the game's lock on the
// list), and this tab's rows are pushed when the game rebuilds it (right after the game took out its old rows, inside the lock), built
// as the game builds its own (its row constructor, its push, its row destructor). Our rows' ids are 0x7C0 and up (low byte 0xC0-0xFF:
// every game switch passes them by). The game's checkbox, slider, stepper and Defaults handlers (slot 1 of their tables) are ours: our
// ids are handled here (the setting changed as in the F11 window, the mod's settings saved), the others go to the game's own. Nothing
// is written into the game's or the menu's files.
namespace VrSettingsTab
{
    enum class Kind : uint32_t
    {
        Slider   = 0,
        Stepper  = 1,
        Checkbox = 3,
        Header   = 5,
    };

    struct Row
    {
        Kind        kind{ Kind::Header };
        std::string text;        // the row's name (plain words: shown as written)
        std::string description; // shown in the menu's box beside the list for the row picked
        ModToggle*  toggle{ nullptr };
        ModSlider*  slider{ nullptr };
        ModCombo*   combo{ nullptr };
        std::function<std::string(float)> show;          // a slider: its value as shown (the menu's bar moves in hundredths)
        std::vector<std::string>          options;       // a stepper: its choices' names
        std::vector<float>                values;        // a stepper over a slider's setting (its steps coarser than the menu's
                                                         // sliders' hundredths): each choice's value
        std::function<bool()>             enabled;       // empty: always usable
        std::function<void()>             apply;         // after a change: the mod's settings set from the value
        bool                              rebuild{ false }; // other rows are usable or not with it: the tab built again after a change
    };

    // (The mod's start, before Install) The rows of the tab, in order; their ids follow (0x7C0 + their place).
    void SetRows(std::vector<Row> rows);
    // Checks the game's code and hooks the tabs' and rows' builds and the change handlers.
    void Install();
    std::string Status();
}
