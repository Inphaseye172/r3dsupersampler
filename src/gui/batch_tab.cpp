#include "batch_tab.hpp"
#include <QCheckBox>
#include <QComboBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QTableView>
#include <QVBoxLayout>
#include "r3dss/pipeline/folder_scanner.hpp"

namespace r3dss::gui {

namespace fs = std::filesystem;

BatchTab::BatchTab(const AppSettings& settings, QWidget* parent) : QWidget(parent), settings_(settings) {
    model_ = new BatchModel(this);
    table_ = new QTableView(this);
    table_->setModel(model_);
    table_->setItemDelegate(new ProgressDelegate(table_));
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    table_->horizontalHeader()->setStretchLastSection(true);
    table_->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    table_->setColumnWidth(BatchModel::File, 220);
    table_->setColumnWidth(BatchModel::Progress, 120);
    table_->verticalHeader()->setVisible(false);
    table_->setAlternatingRowColors(true);

    // Options panel
    auto* options = new QGroupBox("Output", this);
    auto* form = new QFormLayout(options);
    sizeBox_ = new QComboBox(this);
    sizeBox_->addItems({"6K (6048 x 3402)", "UHD (3840 x 2160)", "Full HD (1920 x 1080)", "Same resolution (VBR only)"});
    sizeBox_->setCurrentIndex(1);
    containerBox_ = new QComboBox(this);
    containerBox_->addItems({"CinemaDNG", "Keep source container (CinemaDNG sources only)"});
    containerBox_->setItemData(1, "REDCODE cannot be written; REDCODE sources must choose an output format.", Qt::ToolTipRole);
    profileBox_ = new QComboBox(this);
    profileBox_->addItems({"Q1 Maximum quality", "Q3 High", "Q5 Normal", "Q7 Proxy"});
    profileBox_->setCurrentIndex(2);
    kernelBox_ = new QComboBox(this);
    kernelBox_->addItems({"Lanczos-3 (sharp, clip-aware)", "Area average (soft, lowest noise)"});
    outputEdit_ = new QLineEdit(settings_.lastOutputDir, this);
    auto* browse = new QPushButton("Browse...", this);
    auto* outRow = new QHBoxLayout;
    outRow->addWidget(outputEdit_);
    outRow->addWidget(browse);
    form->addRow("Size", sizeBox_);
    form->addRow("Container", containerBox_);
    form->addRow("Quality", profileBox_);
    form->addRow("Filter", kernelBox_);
    form->addRow("Output folder", outRow);
    verifyBox_ = new QCheckBox("Verify every output (reopen, frame count, resolution, audio, timecode)", this);
    verifyBox_->setChecked(true);
    gpuBox_ = new QCheckBox("Use CUDA GPU when available", this);
    gpuBox_->setChecked(settings_.useGpu);
    overwriteBox_ = new QCheckBox("Overwrite existing outputs", this);
    form->addRow(verifyBox_);
    form->addRow(gpuBox_);
    form->addRow(overwriteBox_);

    auto* folderBox = new QGroupBox("Folder mode", this);
    auto* fl = new QVBoxLayout(folderBox);
    recursiveBox_ = new QCheckBox("Search sub-folders", this);
    recursiveBox_->setChecked(true);
    preserveBox_ = new QCheckBox("Preserve folder structure in the output", this);
    preserveBox_->setChecked(true);
    archiveBox_ = new QCheckBox("Archive mode: copy non-raw files unchanged and verify by hash", this);
    fl->addWidget(recursiveBox_);
    fl->addWidget(preserveBox_);
    fl->addWidget(archiveBox_);

    addFilesBtn_ = new QPushButton("Add files...", this);
    addFolderBtn_ = new QPushButton("Add folder...", this);
    removeBtn_ = new QPushButton("Remove", this);
    startBtn_ = new QPushButton("Start", this);
    pauseBtn_ = new QPushButton("Pause", this);
    cancelBtn_ = new QPushButton("Cancel", this);
    summaryLabel_ = new QLabel(this);
    summaryLabel_->setWordWrap(true);

    auto* right = new QVBoxLayout;
    right->addWidget(options);
    right->addWidget(folderBox);
    right->addStretch(1);
    right->addWidget(summaryLabel_);
    auto* rightW = new QWidget(this);
    rightW->setLayout(right);
    rightW->setMinimumWidth(360);
    rightW->setMaximumWidth(460);

    auto* buttons = new QHBoxLayout;
    buttons->addWidget(addFilesBtn_);
    buttons->addWidget(addFolderBtn_);
    buttons->addWidget(removeBtn_);
    buttons->addStretch(1);
    buttons->addWidget(startBtn_);
    buttons->addWidget(pauseBtn_);
    buttons->addWidget(cancelBtn_);

    auto* left = new QVBoxLayout;
    left->addWidget(table_, 1);
    left->addLayout(buttons);
    auto* leftW = new QWidget(this);
    leftW->setLayout(left);

    auto* root = new QHBoxLayout(this);
    root->addWidget(leftW, 1);
    root->addWidget(rightW);

    connect(addFilesBtn_, &QPushButton::clicked, this, &BatchTab::onAddFiles);
    connect(addFolderBtn_, &QPushButton::clicked, this, &BatchTab::onAddFolder);
    connect(removeBtn_, &QPushButton::clicked, this, &BatchTab::onRemove);
    connect(browse, &QPushButton::clicked, this, &BatchTab::onBrowseOutput);
    connect(startBtn_, &QPushButton::clicked, this, &BatchTab::onStartClicked);
    connect(pauseBtn_, &QPushButton::clicked, this, &BatchTab::pauseResume);
    connect(cancelBtn_, &QPushButton::clicked, this, &BatchTab::cancel);
    connect(outputEdit_, &QLineEdit::textChanged, this, [this](const QString& t) { model_->setOutputDir(fs::path(t.toStdString())); });
    updateButtons();
}

QString BatchTab::title() const {
    if (model_->rowCount() == 0) return "New batch";
    return QString::fromStdString(model_->rows().front().source.filename().string()) +
           (model_->rowCount() > 1 ? QString(" +%1").arg(model_->rowCount() - 1) : QString());
}

void BatchTab::setState(BatchState s) {
    state_ = s;
    updateButtons();
    emit stateChanged(this);
}

void BatchTab::updateButtons() {
    const bool idle = state_ == BatchState::Idle || state_ == BatchState::Finished;
    addFilesBtn_->setEnabled(idle);
    addFolderBtn_->setEnabled(idle);
    removeBtn_->setEnabled(idle);
    startBtn_->setEnabled(idle && hasWork());
    startBtn_->setText(state_ == BatchState::Finished ? "Run again" : "Start");
    pauseBtn_->setEnabled(state_ == BatchState::Running || state_ == BatchState::Paused);
    pauseBtn_->setText(state_ == BatchState::Paused ? "Resume" : "Pause");
    cancelBtn_->setEnabled(state_ == BatchState::Running || state_ == BatchState::Paused || state_ == BatchState::Queued);
    for (QWidget* w : std::vector<QWidget*>{sizeBox_, containerBox_, profileBox_, kernelBox_, outputEdit_, verifyBox_, gpuBox_,
                                            overwriteBox_, recursiveBox_, preserveBox_, archiveBox_})
        w->setEnabled(idle);
}

void BatchTab::onAddFiles() {
    const QStringList files = QFileDialog::getOpenFileNames(this, "Add raw clips", settings_.lastInputDir,
                                                            "Raw clips (*.R3D *.r3d *.NEV *.nev *.dng);;All files (*)");
    if (files.isEmpty()) return;
    settings_.lastInputDir = QFileInfo(files.front()).absolutePath();
    QStringList rejected;
    for (const QString& f : files) {
        fs::path p(f.toStdString());
        if (p.extension() == ".dng" || p.extension() == ".DNG") p = p.parent_path();
        if (!model_->addClip(p, fs::path(outputEdit_->text().toStdString()))) rejected << f;
    }
    if (!rejected.isEmpty())
        QMessageBox::warning(this, "Not recognised", "These files are not recognised raw clips:\n" + rejected.join("\n"));
    if (outputEdit_->text().isEmpty() && !files.isEmpty())
        outputEdit_->setText(QFileInfo(files.front()).absolutePath() + "/supersampled");
    updateButtons();
    emit stateChanged(this);
}

void BatchTab::onAddFolder() {
    const QString dir = QFileDialog::getExistingDirectory(this, "Add folder", settings_.lastInputDir);
    if (dir.isEmpty()) return;
    settings_.lastInputDir = dir;
    if (outputEdit_->text().isEmpty()) outputEdit_->setText(dir + "_supersampled");
    addFolder(fs::path(dir.toStdString()));
}

void BatchTab::addFolder(const fs::path& root) {
    pipeline::ScanOptions so;
    so.recursive = recursiveBox_->isChecked();
    so.preserveStructure = preserveBox_->isChecked();
    so.archiveMode = archiveBox_->isChecked();
    auto res = pipeline::scanFolder(root, fs::path(outputEdit_->text().toStdString()), so);
    if (!res) {
        QMessageBox::warning(this, "Scan failed", QString::fromStdString(res.error().toString()));
        return;
    }
    for (const auto& c : res->clips) model_->addClip(c.source, c.outputDir, c.relativeDir);
    copies_ = res->copies;
    summaryLabel_->setText(QString("%1 clips, %2 non-raw files to copy").arg(model_->rowCount()).arg(copies_.size()));
    updateButtons();
    emit stateChanged(this);
}

void BatchTab::onRemove() {
    QList<int> rows;
    for (const QModelIndex& i : table_->selectionModel()->selectedRows()) rows << i.row();
    model_->removeRows(rows);
    updateButtons();
    emit stateChanged(this);
}

void BatchTab::onBrowseOutput() {
    const QString dir = QFileDialog::getExistingDirectory(this, "Output folder", outputEdit_->text());
    if (!dir.isEmpty()) {
        outputEdit_->setText(dir);
        settings_.lastOutputDir = dir;
    }
}

pipeline::BatchJob BatchTab::buildJob() const {
    pipeline::BatchJob job;
    job.name = title().toStdString();
    job.outputRoot = fs::path(outputEdit_->text().toStdString());
    job.archiveMode = archiveBox_->isChecked();
    settings_.applyTo(job.options);
    static const SizePreset sizes[] = {SizePreset::SixK, SizePreset::UHD, SizePreset::FHD, SizePreset::Same};
    job.options.size = sizes[std::clamp(sizeBox_->currentIndex(), 0, 3)];
    if (containerBox_->currentIndex() == 0) job.options.outputFormat = codec::Format::CinemaDNG;
    else job.options.outputFormat.reset();
    static const QualityProfile profiles[] = {QualityProfile::Q1_Maximum, QualityProfile::Q3_High, QualityProfile::Q5_Normal,
                                              QualityProfile::Q7_Proxy};
    job.options.profile = profiles[std::clamp(profileBox_->currentIndex(), 0, 3)];
    job.options.kernel = kernelBox_->currentIndex() == 1 ? ResampleKernel::Area : ResampleKernel::Lanczos3;
    job.options.verify = verifyBox_->isChecked();
    job.options.useGpu = gpuBox_->isChecked();
    job.options.overwrite = overwriteBox_->isChecked();
    for (const auto& r : model_->rows()) job.clips.push_back(r.job);
    job.copies = copies_;
    return job;
}

void BatchTab::onStartClicked() {
    if (outputEdit_->text().isEmpty()) {
        QMessageBox::warning(this, "Output folder", "Choose an output folder first.");
        return;
    }
    settings_.lastOutputDir = outputEdit_->text();
    emit requestStart(this);
}

void BatchTab::markQueued() {
    setState(BatchState::Queued);
    summaryLabel_->setText("Queued: starts when the running batch finishes.");
}

void BatchTab::startNow() {
    model_->resetForRun();
    copiesDone_ = copiesFailed_ = 0;
    engine_ = std::make_unique<EngineBridge>(buildJob());
    auto* w = engine_->worker();
    connect(w, &EngineWorker::clipStarted, this, [this](int i) { model_->setState(i, RowState::Probing); }, Qt::QueuedConnection);
    connect(w, &EngineWorker::clipProgress, this, [this](int i, qint64 done, qint64 total, double fps, QString stage) {
        model_->setProgress(i, pipeline::ClipProgress{done, total, fps, stage.toStdString()});
        emit statusText(QString("%1: frame %2/%3 at %4 fps").arg(title()).arg(done).arg(total).arg(fps, 0, 'f', 1));
    }, Qt::QueuedConnection);
    connect(w, &EngineWorker::clipDone, this, [this](int i, pipeline::ClipResult r) { model_->setResult(i, r); }, Qt::QueuedConnection);
    connect(w, &EngineWorker::copyDone, this, [this](int, bool ok, QString) {
        ok ? ++copiesDone_ : ++copiesFailed_;
        summaryLabel_->setText(QString("Copied %1 of %2 files (%3 failed)").arg(copiesDone_).arg(copies_.size()).arg(copiesFailed_));
    }, Qt::QueuedConnection);
    connect(w, &EngineWorker::finished, this, [this](pipeline::BatchResult res) {
        summaryLabel_->setText(QString("Finished: %1 done, %2 failed, %3 skipped, %4 cancelled, %5 copied in %6 s%7")
                                   .arg(res.done).arg(res.failed).arg(res.skipped).arg(res.cancelled).arg(res.copies.size())
                                   .arg(res.seconds, 0, 'f', 1)
                                   .arg(res.manifest.empty() ? QString() : "\nManifest: " + QString::fromStdString(res.manifest.string())));
        setState(BatchState::Finished);
        emit statusText(QString("%1 finished").arg(title()));
        emit finished(this);
    }, Qt::QueuedConnection);
    setState(BatchState::Running);
    summaryLabel_->setText("Running...");
    engine_->start();
}

void BatchTab::cancel() {
    if (state_ == BatchState::Queued) {
        setState(BatchState::Idle);
        summaryLabel_->setText("Removed from the queue.");
        emit finished(this);
        return;
    }
    if (engine_) {
        engine_->resume();
        engine_->cancel();
        summaryLabel_->setText("Cancelling...");
    }
}

void BatchTab::pauseResume() {
    if (!engine_) return;
    if (engine_->paused()) {
        engine_->resume();
        setState(BatchState::Running);
    } else {
        engine_->pause();
        setState(BatchState::Paused);
        summaryLabel_->setText("Paused after the frames in flight.");
    }
}

} // namespace r3dss::gui
