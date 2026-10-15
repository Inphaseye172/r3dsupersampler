#pragma once
#include <QMainWindow>
#include <deque>
#include "app_settings.hpp"
#include "batch_tab.hpp"

class QTabWidget;
class QLabel;
class QPlainTextEdit;

namespace r3dss::gui {

class MainWindow final : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    BatchTab* newBatch();

protected:
    void closeEvent(QCloseEvent* e) override;

private slots:
    void onRequestStart(BatchTab* tab);
    void onBatchFinished(BatchTab* tab);
    void onTabClose(int index);
    void onSettings();
    void onResolveNote();
    void onRenameTab(int index);
    void appendLog(const QString& line);

private:
    void updateStatus();
    void refreshTabText(BatchTab* tab);
    void startNextQueued();
    void applyLogLevel();

    AppSettings settings_;
    QTabWidget* tabs_;
    QLabel *activeLabel_, *queueLabel_, *gpuLabel_;
    QPlainTextEdit* log_;
    BatchTab* running_ = nullptr;
    std::deque<BatchTab*> queue_;
};

} // namespace r3dss::gui
