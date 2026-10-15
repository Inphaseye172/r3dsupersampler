#include "main_window.hpp"
#include <QAction>
#include <QCloseEvent>
#include <QDockWidget>
#include <QLabel>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QStatusBar>
#include <QTabWidget>
#include <QToolBar>
#include "r3dss/core/logger.hpp"
#include "r3dss/pipeline/clip_converter.hpp"
#include "settings_dialog.hpp"

namespace r3dss::gui {

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent), settings_(AppSettings::load()) {
    setWindowTitle("r3dss (preview)");
    resize(1280, 760);
    tabs_ = new QTabWidget(this);
    tabs_->setTabsClosable(true);
    tabs_->setMovable(true);
    setCentralWidget(tabs_);
    connect(tabs_, &QTabWidget::tabCloseRequested, this, &MainWindow::onTabClose);

    auto* tb = addToolBar("Main");
    tb->setMovable(false);
    tb->addAction("New batch", this, [this] { newBatch(); });
    tb->addAction("Settings...", this, &MainWindow::onSettings);
    tb->addAction("Resolve note", this, &MainWindow::onResolveNote);

    activeLabel_ = new QLabel("Idle", this);
    queueLabel_ = new QLabel(this);
    gpuLabel_ = new QLabel(this);
    statusBar()->addWidget(activeLabel_, 1);
    statusBar()->addPermanentWidget(queueLabel_);
    statusBar()->addPermanentWidget(gpuLabel_);

    log_ = new QPlainTextEdit(this);
    log_->setReadOnly(true);
    log_->setMaximumBlockCount(5000);
    auto* dock = new QDockWidget("Log", this);
    dock->setWidget(log_);
    addDockWidget(Qt::BottomDockWidgetArea, dock);
    dock->resize(dock->width(), 140);

    Logger::instance().setCallback([this](const LogRecord& r) {
        const QString line = QString::fromStdString(r.timestamp + " [" + logLevelName(r.level) + "] " + r.category + ": " + r.message);
        QMetaObject::invokeMethod(this, [this, line] { appendLog(line); }, Qt::QueuedConnection);
    });
    applyLogLevel();
    newBatch();
    updateStatus();
}

BatchTab* MainWindow::newBatch() {
    auto* tab = new BatchTab(settings_, this);
    tabs_->addTab(tab, tab->title());
    tabs_->setCurrentWidget(tab);
    connect(tab, &BatchTab::requestStart, this, &MainWindow::onRequestStart);
    connect(tab, &BatchTab::finished, this, &MainWindow::onBatchFinished);
    connect(tab, &BatchTab::statusText, this, [this](const QString& t) { activeLabel_->setText(t); });
    connect(tab, &BatchTab::stateChanged, this, [this](BatchTab* t) {
        const int i = tabs_->indexOf(t);
        if (i >= 0) tabs_->setTabText(i, t->title() + (t->state() == BatchState::Queued ? " (queued)" : t->state() == BatchState::Running ? " (running)" : ""));
        updateStatus();
    });
    return tab;
}

void MainWindow::onRequestStart(BatchTab* tab) {
    tab->applySettings(settings_);
    if (running_) {
        queue_.push_back(tab);
        tab->markQueued();
    } else {
        running_ = tab;
        tab->startNow();
    }
    updateStatus();
}

void MainWindow::onBatchFinished(BatchTab* tab) {
    if (running_ == tab) {
        running_ = nullptr;
        startNextQueued();
    } else {
        queue_.erase(std::remove(queue_.begin(), queue_.end(), tab), queue_.end());
    }
    updateStatus();
}

void MainWindow::startNextQueued() {
    while (!queue_.empty() && !running_) {
        BatchTab* next = queue_.front();
        queue_.pop_front();
        if (next->state() != BatchState::Queued) continue;
        running_ = next;
        next->startNow();
    }
}

void MainWindow::onTabClose(int index) {
    auto* tab = qobject_cast<BatchTab*>(tabs_->widget(index));
    if (!tab) return;
    if (tab->state() == BatchState::Running || tab->state() == BatchState::Paused) {
        QMessageBox::information(this, "Batch running", "Cancel the batch before closing its tab.");
        return;
    }
    queue_.erase(std::remove(queue_.begin(), queue_.end(), tab), queue_.end());
    tabs_->removeTab(index);
    tab->deleteLater();
    if (tabs_->count() == 0) newBatch();
    updateStatus();
}

void MainWindow::onSettings() {
    SettingsDialog dlg(settings_, this);
    if (dlg.exec() == QDialog::Accepted) {
        settings_ = dlg.settings();
        settings_.save();
        applyLogLevel();
        for (int i = 0; i < tabs_->count(); ++i)
            if (auto* t = qobject_cast<BatchTab*>(tabs_->widget(i))) t->applySettings(settings_);
        updateStatus();
    }
}

void MainWindow::onResolveNote() {
    QMessageBox::information(this, "DaVinci Resolve: half-res decode ringing",
                             "When Resolve decodes R3D NE at half resolution, clipped highlights can show ringing "
                             "(dark halos and overshoot at hard edges).\n\n"
                             "Fix: Project Settings > Camera RAW > R3D > Decode quality = Full Res Premium, or set it "
                             "per clip in the Camera RAW palette. Playback resolution does not change the decode "
                             "quality.\n\nThe supersampled files from this app resample each colour plane with a "
                             "clip-aware filter, so the mosaic itself carries no such halos.");
}

void MainWindow::appendLog(const QString& line) { log_->appendPlainText(line); }

void MainWindow::applyLogLevel() {
    auto& l = Logger::instance();
    const QString lv = settings_.logLevel;
    l.setLevel(lv == "trace" ? LogLevel::Trace : lv == "debug" ? LogLevel::Debug : lv == "warn" ? LogLevel::Warn
                                                : lv == "error" ? LogLevel::Error : LogLevel::Info);
}

void MainWindow::updateStatus() {
    queueLabel_->setText(QString("Queued batches: %1").arg(queue_.size()));
    const auto rc = pipeline::chooseResampler(settings_.useGpu);
    gpuLabel_->setText(rc.cuda ? "GPU: CUDA" : "CPU fallback");
    if (!running_) activeLabel_->setText("Idle");
}

void MainWindow::closeEvent(QCloseEvent* e) {
    if (running_) {
        const auto r = QMessageBox::question(this, "Batch running", "A batch is still running. Cancel it and quit?");
        if (r != QMessageBox::Yes) {
            e->ignore();
            return;
        }
        running_->cancel();
    }
    settings_.save();
    Logger::instance().setCallback(nullptr);
    e->accept();
}

} // namespace r3dss::gui
