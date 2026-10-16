#include <QApplication>
#include <QCoreApplication>
#include <QTimer>
#include <cstring>
#include <iostream>
#include "headless_run.hpp"
#include "main_window.hpp"
#include "r3dss/codec/codec.hpp"
#include "r3dss/core/logger.hpp"

namespace {

bool isRunJobArg(const char* a) { return std::strcmp(a, "--run-job") == 0 || std::strncmp(a, "--run-job=", 10) == 0; }

// r3dss-gui --run-job JOB.json: the batch runs without a window (no display needed).
int runHeadless(int argc, char** argv) {
    r3dss::gui::attachParentConsole();
    QCoreApplication app(argc, argv);
    r3dss::codec::registerBuiltinCodecs();
    auto& logger = r3dss::Logger::instance();
    logger.enableStderr(false);
    logger.openFile(r3dss::Logger::defaultLogDir() / "r3dss-gui.log");
    // QCoreApplication::arguments() is Unicode on every platform (argv is ANSI on Windows).
    const QStringList args = QCoreApplication::arguments();
    QString jobPath;
    for (int i = 1; i < args.size(); ++i) {
        if (args[i] == "--run-job" && i + 1 < args.size()) jobPath = args[i + 1];
        else if (args[i].startsWith("--run-job=")) jobPath = args[i].mid(10);
    }
    if (jobPath.isEmpty()) {
        std::cerr << "usage: r3dss-gui --run-job JOB.json   (a job written by r3dss-cli scan ... --write-job)\n";
        return 2;
    }
    return r3dss::gui::runJobHeadless(jobPath, std::cout, std::cerr);
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication::setOrganizationName("Anchorpoint");
    QCoreApplication::setApplicationName("r3dss");
    for (int i = 1; i < argc; ++i)
        if (isRunJobArg(argv[i])) return runHeadless(argc, argv);

    QApplication app(argc, argv);
    r3dss::codec::registerBuiltinCodecs();
    auto& logger = r3dss::Logger::instance();
    logger.enableStderr(false);
    logger.openFile(r3dss::Logger::defaultLogDir() / "r3dss-gui.log");
    r3dss::gui::MainWindow w;
    w.show();
    // --smoke: show the window, process events, exit (CI / headless check)
    for (int i = 1; i < argc; ++i)
        if (QString(argv[i]) == "--smoke") {
            QTimer::singleShot(500, &app, &QCoreApplication::quit);
            break;
        }
    return app.exec();
}
