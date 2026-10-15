// One batch: source table on the left, options on the right, run controls at the bottom.
#pragma once
#include <QWidget>
#include <memory>
#include "app_settings.hpp"
#include "batch_model.hpp"
#include "engine_bridge.hpp"

class QTableView;
class QComboBox;
class QLineEdit;
class QCheckBox;
class QPushButton;
class QGroupBox;
class QLabel;

namespace r3dss::gui {

enum class BatchState { Idle, Queued, Running, Paused, Finished };

class BatchTab final : public QWidget {
    Q_OBJECT
public:
    explicit BatchTab(const AppSettings& settings, QWidget* parent = nullptr);
    BatchState state() const { return state_; }
    QString title() const;
    QString customTitle() const { return customTitle_; }
    void setCustomTitle(const QString& t) { customTitle_ = t; }
    bool hasWork() const { return model_->rowCount() > 0 || !copies_.empty(); }
    pipeline::BatchJob buildJob() const;
    void applySettings(const AppSettings& s) { settings_ = s; }

    // Called by the main window's queue.
    void startNow();
    void cancel();
    void pauseResume();
    void markQueued();

signals:
    void requestStart(BatchTab* tab);
    void finished(BatchTab* tab);
    void statusText(const QString& text);
    void stateChanged(BatchTab* tab);

private slots:
    void onAddFiles();
    void onAddFolder();
    void onRemove();
    void onBrowseOutput();
    void onStartClicked();

private:
    void setState(BatchState s);
    void updateButtons();
    void addFolder(const std::filesystem::path& root);

    AppSettings settings_;
    BatchModel* model_;
    QTableView* table_;
    QComboBox *sizeBox_, *containerBox_, *profileBox_, *kernelBox_;
    QLineEdit* outputEdit_;
    QCheckBox *recursiveBox_, *preserveBox_, *archiveBox_, *verifyBox_, *gpuBox_, *overwriteBox_;
    QPushButton *addFilesBtn_, *addFolderBtn_, *removeBtn_, *startBtn_, *pauseBtn_, *cancelBtn_;
    QLabel* summaryLabel_;
    BatchState state_ = BatchState::Idle;
    QString customTitle_; // user-chosen tab name; empty = derived from the first source
    std::unique_ptr<EngineBridge> engine_;
    std::vector<pipeline::CopyJob> copies_;
    int copiesDone_ = 0, copiesFailed_ = 0;
};

} // namespace r3dss::gui
