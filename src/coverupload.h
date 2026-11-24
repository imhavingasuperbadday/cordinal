// Giving a cover an address Discord can fetch. cordinal holds the picture but Discord can only render an address,
// so either the built-in host (Preferences > Scrobbling > Upload covers for Discord, off by default;
// online::HostCover does the upload) or the user's own command takes it. A command configured in settings.ini
// ([Scrobbling] DiscordCoverUpload=) is handed the image's path and the first https address it prints becomes that
// cover's address. Both are off by default: nothing is sent and no process runs unless the user turned one on.
#pragma once
#include <string>

namespace coverupload {

// The built-in host, for covers that have no address of their own: the one the player shows, embedded, beside the
// track or chosen from a file. Any thread.
bool HostEnabled();
void SetHostEnabled(bool on);

// False when no command is configured, in which case nothing ever runs.
bool Configured();
// The command line, run with the image's path appended as one quoted argument.
void SetCommand(const std::wstring& command);
std::wstring Command();
// Blocking: call it on a worker. "" when nothing was configured, the command failed or ran past the timeout, or
// it printed no https address. The image is passed last so the command keeps its own arguments.
std::string Run(const std::wstring& imagePath);
// Stops a run in progress within a moment, so shutting down never waits for a stuck tool. The next Run starts
// afresh.
void CancelAll();

}  // namespace coverupload
