// Declarations shared by app_ui.cpp and the files split out of it. The rest of the app keeps
// including app_ui.h, which stays the public surface for everything declared here.
#pragma once
#include "app.h"

namespace app {

// The theme font the dark menu bar's items are drawn in (app_ui.cpp); none before CreateFonts().
HFONT MenuFont();

}  // namespace app
