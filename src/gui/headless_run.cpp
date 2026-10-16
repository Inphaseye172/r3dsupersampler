#include "headless_run.hpp"
#include "batch_model.hpp"
#include "engine_bridge.hpp"
#include "interrupt.hpp"
#include "qt_paths.hpp"
#include <QEventLoop>
#include <iomanip>
#include <optional>
#include "r3dss/pipeline/job.hpp"
#include "r3dss/pipeline/scheduler.hpp"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstdio>
#include <iostream>
#endif

namespace r3dss::gui {

int runJobHeadless(const QString& jobPath, std::ostream& out, std::ostream& err, bool handleInterrupts) {
    auto loaded = pipeline::loadBatchJob(toPath(jobPath));
    if (!loaded) {
        err << "r3dss-gui --run-job: " << loaded.error().toString() << "\n";
        return 1;
    }
    const pipeline::BatchJob job = std::move(*loaded);
    const size_t clipCount = job.clips.size();
    out << "job '" << job.name << "': " << clipCount << " clips, " << job.copies.size() << " files to copy, output "
        << toUtf8(job.outputRoot) << "\n"
        << std::flush;

    QEventLoop loop;
    pipeline::BatchResult result;
    bool finished = false;
    EngineBridge bridge(job);
    EngineWorker* worker = bridge.worker();
    QObject::connect(worker, &EngineWorker::clipDone, &loop, [&](int i, pipeline::ClipResult r) {
        const auto idx = static_cast<size_t>(i);
        out << "clip " << idx + 1 << "/" << clipCount << " " << pipeline::clipStatusName(r.status) << ": "
            << (idx < clipCount ? toUtf8(job.clips[idx].source) : std::string("?"));
        if (!r.output.empty()) out << " -> " << toUtf8(r.output);
        const QString detail = describeResult(r);
        if (!detail.isEmpty()) out << " | " << detail.toStdString();
        out << "\n" << std::flush;
    }, Qt::QueuedConnection);
    QObject::connect(worker, &EngineWorker::copyDone, &loop, [&](int, bool ok, QString text) {
        if (!ok) out << "copy failed: " << text.toStdString() << "\n" << std::flush;
    }, Qt::QueuedConnection);
    QObject::connect(worker, &EngineWorker::finished, &loop, [&](pipeline::BatchResult r) {
        result = std::move(r);
        finished = true;
        loop.quit();
    }, Qt::QueuedConnection);

    {
        // The first Ctrl-C / SIGTERM cancels the batch like the Cancel button (unfinished outputs are
        // removed); the guard calls back from its own thread, and Scheduler::cancel() is thread-safe.
        std::optional<cli::InterruptGuard> interrupt;
        if (handleInterrupts) interrupt.emplace([&bridge] { bridge.cancel(); });
        bridge.start();
        if (!finished) loop.exec();
    }

    out << "batch '" << job.name << "': " << result.done << " done, " << result.failed << " failed, " << result.skipped
        << " skipped, " << result.cancelled << " cancelled, " << result.copies.size() << " copied, " << std::fixed
        << std::setprecision(1) << result.seconds << " s\n";
    if (!result.copyErrors.empty()) out << result.copyErrors.size() << " archive copies failed\n";
    if (!result.manifest.empty()) out << "manifest: " << toUtf8(result.manifest) << "\n";
    const int code = pipeline::batchExitCode(result);
    if (result.aborted) out << "batch cancelled: unfinished outputs were removed\n";
    if (code == 3) {
        if (result.done == 0) out << "no output written: every clip was skipped (see the messages above)\n";
        else
            out << "no output written for " << result.skipped - result.skippedExisting << " of " << result.clips.size()
                << " clips: they were skipped (see above)\n";
    }
    out << std::flush;
    return code;
}

void attachParentConsole() {
#ifdef _WIN32
    auto usable = [](DWORD which) {
        const HANDLE h = GetStdHandle(which);
        return h != nullptr && h != INVALID_HANDLE_VALUE;
    };
    const bool outOk = usable(STD_OUTPUT_HANDLE), errOk = usable(STD_ERROR_HANDLE);
    if (outOk && errOk) return; // redirected to a file or pipe: keep it
    if (!AttachConsole(ATTACH_PARENT_PROCESS)) return; // started without a console (e.g. from Explorer)
    FILE* f = nullptr;
    if (!outOk && freopen_s(&f, "CONOUT$", "w", stdout) == 0) std::cout.clear();
    if (!errOk && freopen_s(&f, "CONOUT$", "w", stderr) == 0) std::cerr.clear();
#endif
}

} // namespace r3dss::gui
