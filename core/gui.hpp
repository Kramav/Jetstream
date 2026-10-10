#pragma once
// The game's GUI layout files (.gui), CLAUDE.md §10 M3 "Triggers": entries added to one of its lists (the merchant's
// first menu: talk topics). RE4R's layout only (GUI version 34), as REE-Lib's GuiFile.cs (MIT) reads it; nothing
// copied from it. Nothing of the game's file moves: what's new is appended after it.
#include <string>
#include <vector>

namespace remod {

// The merchant's first menu (Buy, Sell, Upgrade, Trade) under the natives root, and the entry the spares copy.
inline constexpr const char* kMerchantMenuGui = "_chainsaw/ui/ui3500/gui/cs_ui3510.gui.540034";
inline constexpr const char* kMerchantMenuLast = "si_menu_3";
// Spare entries remod adds there (si_menu_4..7); the runtime shows the ones talk topics use and hides the rest.
inline constexpr int kMerchantSpareEntries = 4;

// One entry of a GUI list, as the layout file has it.
struct GuiEntry {
    std::string name;
    float x = 0, y = 0, z = 0;  // its Position attribute
    int priority = -1;          // its Priority attribute; -1 if none
};

// The entries of the list holding the element named `entry`, in order. Throws std::runtime_error if the file isn't an
// RE4R GUI layout or has no such element.
std::vector<GuiEntry> gui_list(const std::string& gui, const std::string& entry);

// `gui` with an element per name in `names` added to the list after `copy`, each a copy of it (sharing its children,
// as the game's own entries do): a new ID (made from its name, so the same names give the same file), its name, a
// Position moved on by the step between `copy` and the entry before it, Priority one more each. The game's bytes stay
// as they are; the copies and a new list are appended, and the list's container points at the new list. Throws
// std::runtime_error as gui_list, and if a name is already in the list.
std::string add_gui_entries(const std::string& gui, const std::string& copy, const std::vector<std::string>& names);

}  // namespace remod
