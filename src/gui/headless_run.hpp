// `r3dss-gui --run-job JOB.json`: runs a saved batch job without a window, through the same
// EngineBridge / Scheduler path the batch tabs use. JOB.json is the format `r3dss-cli scan ... --write-job`
// writes; the job's own options apply (the GUI settings are not read or changed).
#pragma once
#include <QString>
#include <ostream>

namespace r3dss::gui {

// Prints one summary line per clip to `out` and returns the process exit code (the same as r3dss-cli,
// pipeline::batchExitCode): 0 = every clip done, or skipped only because its complete output already
// exists; 1 = a clip failed or was cancelled, an archive copy failed, the batch was interrupted, or the
// job file cannot be read; 3 = nothing was written for a clip skipped for another reason (e.g. a size
// larger than the source). Needs a QCoreApplication. With `handleInterrupts`, Ctrl-C / SIGTERM cancel the
// batch through the normal path (unfinished outputs are removed, exit code 1).
int runJobHeadless(const QString& jobPath, std::ostream& out, std::ostream& err, bool handleInterrupts = true);

// Windows: r3dss-gui is a GUI-subsystem program without a console of its own. When stdout / stderr are
// not redirected, attach to the console of the shell that started it so --run-job output is visible.
// Does nothing elsewhere.
void attachParentConsole();

} // namespace r3dss::gui
