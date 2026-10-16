// One batch: source table on the left, options on the right, run controls at the bottom.
#pragma once
#include <QStringList>
#include <QWidget>
#include <filesystem>
#include <memory>
#include <optional>
#include <vector>
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
    bool hasWork() const;
    // Builds the job from what the panel shows now. Destinations are never kept from the moment a clip or
    // folder was added: every added folder is scanned again with the current output folder, "Preserve
    // folder structure" and archive mode, so clips, sidecars and archive copies all land under the output
    // folder that is set at start time. With several added folders and a preserved structure, each
    // folder's outputs go into a sub-folder named after it. Anything that prevents the job from running
    // as shown (a folder that can no longer be scanned) is appended to `problems`.
    pipeline::BatchJob buildJob(QStringList* problems = nullptr) const;
    void applySettings(const AppSettings& s) { settings_ = s; }

    // Lists the clips of a folder (and of its sub-folders when "Search sub-folders" is ticked).
    void addFolder(const std::filesystem::path& root);
    // Adds clip files or CinemaDNG folders (a .dng frame stands for its clip folder). Returns the
    // entries that are not recognised raw clips.
    QStringList addFiles(const QStringList& files);

    // The output codec for every clip: the chosen container, or the clip's own when keeping it.
    std::optional<codec::Format> chosenOutputFormat() const;
    // True when the quality profile has an effect: some output codec of this batch has rate control.
    // CinemaDNG output is always lossless, so the Quality control is disabled for it.
    bool qualityApplies() const;

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
    struct AddedFolder {
        std::filesystem::path root;
        bool recursive = true; // "Search sub-folders" when the folder was added: decides which clips it lists
    };

    void setState(BatchState s);
    void updateButtons();
    void refreshSummary();
    QString qualityDisabledReason() const;
    std::filesystem::path outputRoot() const;
    // Output folder of each added folder (the output root itself unless several are added and the
    // folder structure is preserved).
    std::vector<std::filesystem::path> folderOutputDirs(const std::filesystem::path& outRoot) const;

    AppSettings settings_;
    BatchModel* model_;
    QTableView* table_;
    QComboBox *sizeBox_, *containerBox_, *profileBox_, *kernelBox_;
    QLineEdit* outputEdit_;
    QCheckBox *recursiveBox_, *preserveBox_, *archiveBox_, *verifyBox_, *gpuBox_, *overwriteBox_;
    QPushButton *addFilesBtn_, *addFolderBtn_, *removeBtn_, *startBtn_, *pauseBtn_, *cancelBtn_;
    QLabel* summaryLabel_;
    BatchState state_ = BatchState::Idle;
    std::unique_ptr<EngineBridge> engine_;
    std::vector<AddedFolder> folders_;
    size_t plannedCopies_ = 0;
    int copiesDone_ = 0, copiesFailed_ = 0;
};

} // namespace r3dss::gui
