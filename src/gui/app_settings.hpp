// Persistent user settings (QSettings) mapped onto JobOptions defaults.
#pragma once
#include <QSettings>
#include <QString>
#include "r3dss/pipeline/job.hpp"

namespace r3dss::gui {

struct AppSettings {
    bool useGpu = true;
    unsigned maxParallelClips = 0;
    unsigned maxThreads = 0;
    QString namingTemplate = "{stem}_{size}_{profile}";
    QString logLevel = "info";
    QString lastOutputDir;
    QString lastInputDir;

    static AppSettings load() {
        QSettings s;
        AppSettings a;
        a.useGpu = s.value("engine/useGpu", true).toBool();
        a.maxParallelClips = s.value("engine/maxParallelClips", 0).toUInt();
        a.maxThreads = s.value("engine/maxThreads", 0).toUInt();
        a.namingTemplate = s.value("output/namingTemplate", a.namingTemplate).toString();
        a.logLevel = s.value("log/level", "info").toString();
        a.lastOutputDir = s.value("paths/lastOutputDir").toString();
        a.lastInputDir = s.value("paths/lastInputDir").toString();
        return a;
    }
    void save() const {
        QSettings s;
        s.setValue("engine/useGpu", useGpu);
        s.setValue("engine/maxParallelClips", maxParallelClips);
        s.setValue("engine/maxThreads", maxThreads);
        s.setValue("output/namingTemplate", namingTemplate);
        s.setValue("log/level", logLevel);
        s.setValue("paths/lastOutputDir", lastOutputDir);
        s.setValue("paths/lastInputDir", lastInputDir);
    }
    void applyTo(pipeline::JobOptions& o) const {
        o.useGpu = useGpu;
        o.maxParallelClips = maxParallelClips;
        o.maxThreads = maxThreads;
        o.namingTemplate = namingTemplate.toStdString();
    }
};

} // namespace r3dss::gui
