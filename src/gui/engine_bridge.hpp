// Runs a Scheduler on a worker thread and forwards its callbacks as queued Qt signals.
#pragma once
#include <QObject>
#include <QThread>
#include <memory>
#include "r3dss/pipeline/scheduler.hpp"

namespace r3dss::gui {

class EngineWorker final : public QObject {
    Q_OBJECT
public:
    explicit EngineWorker(pipeline::BatchJob job) : job_(std::move(job)) {}
    pipeline::Scheduler& scheduler() { return sched_; }
public slots:
    void run();
signals:
    void clipStarted(int clip);
    void clipProgress(int clip, qint64 done, qint64 total, double fps, QString stage);
    void clipDone(int clip, r3dss::pipeline::ClipResult result);
    void copyDone(int copy, bool ok, QString text);
    void finished(r3dss::pipeline::BatchResult result);

private:
    pipeline::BatchJob job_;
    pipeline::Scheduler sched_;
};

// Owns the thread; one per running batch.
class EngineBridge final : public QObject {
    Q_OBJECT
public:
    explicit EngineBridge(pipeline::BatchJob job, QObject* parent = nullptr);
    ~EngineBridge() override;
    void start();
    void cancel();
    void pause();
    void resume();
    bool paused() const;
    EngineWorker* worker() const { return worker_; }

private:
    QThread thread_;
    EngineWorker* worker_;
};

} // namespace r3dss::gui

Q_DECLARE_METATYPE(r3dss::pipeline::ClipResult)
Q_DECLARE_METATYPE(r3dss::pipeline::BatchResult)
