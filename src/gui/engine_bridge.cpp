#include "engine_bridge.hpp"

namespace r3dss::gui {

void EngineWorker::run() {
    pipeline::BatchCallbacks cb;
    cb.onClipStart = [this](size_t i) { emit clipStarted(static_cast<int>(i)); };
    cb.onClipProgress = [this](size_t i, const pipeline::ClipProgress& p) {
        emit clipProgress(static_cast<int>(i), p.framesDone, p.totalFrames, p.fps, QString::fromStdString(p.stage));
    };
    cb.onClipDone = [this](size_t i, const pipeline::ClipResult& r) { emit clipDone(static_cast<int>(i), r); };
    cb.onCopyDone = [this](size_t i, const Expected<pipeline::CopyResult>& r) {
        emit copyDone(static_cast<int>(i), r.hasValue(),
                      r ? QString::fromStdString(r->dest.string()) : QString::fromStdString(r.error().toString()));
    };
    emit finished(sched_.run(job_, cb));
}

EngineBridge::EngineBridge(pipeline::BatchJob job, QObject* parent) : QObject(parent), worker_(new EngineWorker(std::move(job))) {
    static bool registered = false;
    if (!registered) {
        qRegisterMetaType<pipeline::ClipResult>("r3dss::pipeline::ClipResult");
        qRegisterMetaType<pipeline::BatchResult>("r3dss::pipeline::BatchResult");
        registered = true;
    }
    worker_->moveToThread(&thread_);
    connect(&thread_, &QThread::started, worker_, &EngineWorker::run);
    connect(worker_, &EngineWorker::finished, &thread_, &QThread::quit);
}

EngineBridge::~EngineBridge() {
    worker_->scheduler().cancel();
    thread_.quit();
    thread_.wait();
    delete worker_;
}

void EngineBridge::start() { thread_.start(); }
void EngineBridge::cancel() { worker_->scheduler().cancel(); }
void EngineBridge::pause() { worker_->scheduler().pause(); }
void EngineBridge::resume() { worker_->scheduler().resume(); }
bool EngineBridge::paused() const { return worker_->scheduler().paused(); }

} // namespace r3dss::gui
