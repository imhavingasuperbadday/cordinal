// cordinal's own icon on Waybar, the top bar of Omarchy and other Hyprland or Sway desktops (waybar.cpp).
// Omarchy folds tray icons behind an expander, so the tray icon alone is out of sight; this module sits
// on the bar itself, shows what plays in its tooltip, and is there only while cordinal runs.
#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace lx::waybar {

// `cordinal --waybar`: the module's process. Waybar starts it (the config's "exec") and reads one JSON
// line per change; it follows cordinal over the session bus and never exits on its own.
int RunModule();

// The user's Waybar config file (as Waybar looks for it), or empty when there is none.
std::string ConfigPath();
bool Shown();                // the config has the module
bool SetShown(bool shown);   // adds or removes it, then asks Waybar to reload; false when it could not

// The config edits, on text, for the checks: the module's entry goes in "modules-right" (else center,
// else left) with its definition just before that key, between comment markers that Remove looks for.
// `command` is the shell command that runs cordinal. Add returns nothing when there is no modules list.
std::optional<std::string> Add(std::string_view config, std::string_view command);
std::string Remove(std::string_view config);
bool Has(std::string_view config);

}  // namespace lx::waybar
