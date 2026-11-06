// Running the user's own upload tool on a cover image. cordinal holds the picture but Discord can only render an
// address it can fetch, so a command configured in settings.ini ([Scrobbling] DiscordCoverUpload=) is handed the
// image's path and the first https address it prints becomes that cover's address for the Discord status. It is
// empty by default: no process runs unless the user configured one.
#pragma once
#include <string>

namespace coverupload {

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
