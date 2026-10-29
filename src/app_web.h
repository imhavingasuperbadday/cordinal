// Native controls and lifecycle of the optional browser music server.
#pragma once
#include <windows.h>

#include <cstdint>

namespace app {
constexpr UINT_PTR kWebTimer = 147;
void ShowWebServer();
void OpenWebPlayer();  // explicit --web activation; does not opt into startup
bool WebDialogMessage(MSG* message);
void InitWebServer();
void StopWebServer();
void WebTick();
void InvalidateWebLibrary();
// "Start the web server with cordinal" (the library.web_start tweak). Setting it only stores it;
// ApplyWebStart starts the server now when it is turned on.
bool WebStartsWithCordinal();
void SetWebStartsWithCordinal(bool on);
void ApplyWebStart();
// 0 keeps the next start on a loopback-only ephemeral port, so checks never open a network port.
void SetWebPortForTest(uint16_t port);
}  // namespace app
