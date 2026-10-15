#include <QApplication>
#include <QTimer>
#include "main_window.hpp"
#include "r3dss/codec/codec.hpp"
#include "r3dss/core/logger.hpp"

int main(int argc, char** argv) {
    QCoreApplication::setOrganizationName("Anchorpoint");
    QCoreApplication::setApplicationName("R3DSupersampler");
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
