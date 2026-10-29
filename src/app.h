// Umbrella header every app_*.cpp includes: pulls in the shared state (app_state.h)
// and each feature module's declarations. Declarations live with the feature that
// implements them, so a module only needs this one include. All shared state is
// owned by the UI thread unless a field's comment says otherwise.
#pragma once
#include "app_state.h"
#include "app_ui.h"
#include "app_settings.h"
#include "app_playlists.h"
#include "app_playback.h"
#include "app_panels.h"
#include "app_tools.h"
#include "app_platform.h"
#include "app_jobs.h"
#include "app_window.h"
#include "app_foobar.h"
#include "app_apollo.h"
#include "app_web.h"
#include "app_tweaks.h"
#include "app_packs.h"
#include "app_helpers.h"
#include "app_radio.h"
