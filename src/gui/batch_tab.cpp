#include "batch_tab.hpp"
#include "qt_paths.hpp"
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
#include <algorithm>
#include <map>
#include <set>
#include "r3dss/pipeline/folder_scanner.hpp"
#include "r3dss/pipeline/output_claim.hpp"

namespace r3dss::gui {

namespace fs = std::filesystem;

namespace {

bool isDngFrame(const fs::path& p) {
    std::error_code ec;
    if (fs::is_directory(p, ec)) return false;
    return toQString(p.extension()).compare(".dng", Qt::CaseInsensitive) == 0;
}

// The output codecs a batch writes: the chosen container, or each listed clip's own format.
std::vector<codec::Format> outputFormats(const std::optional<codec::Format>& chosen, const std::vector<ClipRow>& rows) {
    std::vector<codec::Format> out;
    if (chosen) out.push_back(*chosen);
    else
        for (const auto& r : rows) out.push_back(r.sourceFormat);
    return out;
}

} // namespace

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
    sizeBox_->setObjectName("sizeBox");
    sizeBox_->addItems({"6K (6048 wide)", "UHD (3840 wide)", "2K DCI (2048 wide)", "Full HD (1920 wide)", "Same resolution"});
    sizeBox_->setItemData(4, "Keeps the source size: a CinemaDNG source is re-encoded losslessly, sample for sample.", Qt::ToolTipRole);
    sizeBox_->setCurrentIndex(1);
    containerBox_ = new QComboBox(this);
    containerBox_->setObjectName("containerBox");
    containerBox_->addItems({"CinemaDNG", "Keep source container (CinemaDNG sources only)"});
    containerBox_->setItemData(1, "REDCODE cannot be written; REDCODE sources must choose an output format.", Qt::ToolTipRole);
    profileBox_ = new QComboBox(this);
    profileBox_->setObjectName("profileBox");
    profileBox_->addItems({"Q1 Maximum quality", "Q3 High", "Q5 Normal", "Q7 Proxy"});
    profileBox_->setCurrentIndex(2);
    kernelBox_ = new QComboBox(this);
    kernelBox_->setObjectName("kernelBox");
    kernelBox_->addItems({"Lanczos-3 (sharp, clip-aware)", "Area average (soft, lowest noise)"});
    outputEdit_ = new QLineEdit(settings_.lastOutputDir, this);
    outputEdit_->setObjectName("outputEdit");
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
    verifyBox_->setObjectName("verifyBox");
    verifyBox_->setChecked(true);
    gpuBox_ = new QCheckBox("Use CUDA GPU when available", this);
    gpuBox_->setObjectName("gpuBox");
    gpuBox_->setChecked(settings_.useGpu);
    overwriteBox_ = new QCheckBox("Overwrite existing outputs", this);
    overwriteBox_->setObjectName("overwriteBox");
    form->addRow(verifyBox_);
    form->addRow(gpuBox_);
    form->addRow(overwriteBox_);

    auto* folderBox = new QGroupBox("Folder mode", this);
    auto* fl = new QVBoxLayout(folderBox);
    recursiveBox_ = new QCheckBox("Search sub-folders", this);
    recursiveBox_->setObjectName("recursiveBox");
    recursiveBox_->setChecked(true);
    recursiveBox_->setToolTip("Applies to folders added from now on; the table lists the clips that will be converted.");
    preserveBox_ = new QCheckBox("Preserve folder structure in the output", this);
    preserveBox_->setObjectName("preserveBox");
    preserveBox_->setChecked(true);
    preserveBox_->setToolTip("Mirror the added folder's sub-folders in the output folder. With several added folders, each "
                             "one's outputs go into a sub-folder named after it.\nUnticked: every output goes straight into "
                             "the output folder.");
    archiveBox_ = new QCheckBox("Archive mode: copy non-raw files unchanged and verify by hash", this);
    archiveBox_->setObjectName("archiveBox");
    fl->addWidget(recursiveBox_);
    fl->addWidget(preserveBox_);
    fl->addWidget(archiveBox_);

    addFilesBtn_ = new QPushButton("Add files...", this);
    addFolderBtn_ = new QPushButton("Add folder...", this);
    removeBtn_ = new QPushButton("Remove", this);
    startBtn_ = new QPushButton("Start", this);
    startBtn_->setObjectName("startButton");
    pauseBtn_ = new QPushButton("Pause", this);
    cancelBtn_ = new QPushButton("Cancel", this);
    summaryLabel_ = new QLabel(this);
    summaryLabel_->setObjectName("summaryLabel");
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
    // The output codec decides whether Quality means anything (CinemaDNG is always lossless).
    connect(containerBox_, &QComboBox::currentIndexChanged, this, [this] { updateButtons(); });
    connect(archiveBox_, &QCheckBox::toggled, this, [this] {
        refreshSummary();
        updateButtons();
    });
    updateButtons();
}

QString BatchTab::title() const {
    if (model_->rowCount() == 0) return "New batch";
    return toQString(model_->rows().front().source.filename()) +
           (model_->rowCount() > 1 ? QString(" +%1").arg(model_->rowCount() - 1) : QString());
}

bool BatchTab::hasWork() const {
    // A folder without clips can still have files to archive.
    return model_->rowCount() > 0 || (archiveBox_->isChecked() && !folders_.empty());
}

void BatchTab::setState(BatchState s) {
    state_ = s;
    updateButtons();
    emit stateChanged(this);
}

std::optional<codec::Format> BatchTab::chosenOutputFormat() const {
    if (containerBox_->currentIndex() == 0) return codec::Format::CinemaDNG;
    return std::nullopt; // keep each source's container
}

bool BatchTab::qualityApplies() const {
    for (const codec::Format f : outputFormats(chosenOutputFormat(), model_->rows())) {
        const auto factory = codec::CodecRegistry::instance().byFormat(f);
        if (factory && factory->caps().write && factory->caps().rateControl) return true;
    }
    return false;
}

QString BatchTab::qualityDisabledReason() const {
    std::set<std::string> lossless; // output codecs of this batch that ignore the profile
    for (const codec::Format f : outputFormats(chosenOutputFormat(), model_->rows())) {
        const auto factory = codec::CodecRegistry::instance().byFormat(f);
        if (factory && factory->caps().write && !factory->caps().rateControl) lossless.insert(codec::formatName(f));
    }
    if (lossless.empty()) return "The quality profile applies only to output codecs with rate control.";
    QStringList names;
    for (const auto& n : lossless) names << QString::fromStdString(n);
    return names.join(" and ") + " output is always lossless";
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
    for (QWidget* w : std::vector<QWidget*>{sizeBox_, containerBox_, kernelBox_, outputEdit_, verifyBox_, gpuBox_, overwriteBox_,
                                            recursiveBox_, preserveBox_, archiveBox_})
        w->setEnabled(idle);
    const bool quality = qualityApplies();
    profileBox_->setEnabled(idle && quality);
    profileBox_->setToolTip(quality ? QString() : qualityDisabledReason());
}

fs::path BatchTab::outputRoot() const {
    const QString text = outputEdit_->text();
    return text.isEmpty() ? fs::path() : pipeline::normaliseSourcePath(toPath(text));
}

std::vector<fs::path> BatchTab::folderOutputDirs(const fs::path& outRoot) const {
    std::vector<fs::path> dirs(folders_.size(), outRoot);
    if (folders_.size() < 2 || !preserveBox_->isChecked()) return dirs;
    // Several folders keep their own structure side by side: <output>/<folder name>/...
    std::set<QString> used; // case-insensitive, as on Windows and macOS
    for (size_t i = 0; i < folders_.size(); ++i) {
        QString base = toQString(folders_[i].root.filename());
        if (base.isEmpty()) base = "folder"; // a drive or file-system root
        QString name = base;
        for (int n = 2; !used.insert(name.toLower()).second; ++n) name = QString("%1_%2").arg(base).arg(n);
        dirs[i] = outRoot / toPath(name);
    }
    return dirs;
}

void BatchTab::onAddFiles() {
    const QStringList files = QFileDialog::getOpenFileNames(this, "Add raw clips", settings_.lastInputDir,
                                                            "Raw clips (*.R3D *.r3d *.NEV *.nev *.dng *.DNG);;All files (*)");
    if (files.isEmpty()) return;
    settings_.lastInputDir = QFileInfo(files.front()).absolutePath();
    const QStringList rejected = addFiles(files);
    if (!rejected.isEmpty())
        QMessageBox::warning(this, "Not recognised", "These files are not recognised raw clips:\n" + rejected.join("\n"));
}

QStringList BatchTab::addFiles(const QStringList& files) {
    QStringList rejected;
    fs::path firstClip;
    for (const QString& f : files) {
        fs::path p = pipeline::normaliseSourcePath(toPath(f));
        if (isDngFrame(p)) p = p.parent_path(); // a CinemaDNG frame stands for its clip folder
        if (!model_->addClip(p)) {
            rejected << f;
            continue;
        }
        if (firstClip.empty()) firstClip = p;
    }
    // Default output next to the clip, never inside it (a CinemaDNG clip is a folder).
    if (outputEdit_->text().isEmpty() && !firstClip.empty())
        outputEdit_->setText(toQString(fs::absolute(firstClip).parent_path() / "supersampled"));
    refreshSummary();
    updateButtons();
    emit stateChanged(this);
    return rejected;
}

void BatchTab::onAddFolder() {
    const QString dir = QFileDialog::getExistingDirectory(this, "Add folder", settings_.lastInputDir);
    if (dir.isEmpty()) return;
    settings_.lastInputDir = dir;
    if (outputEdit_->text().isEmpty()) outputEdit_->setText(dir + "_supersampled");
    addFolder(toPath(dir));
}

void BatchTab::addFolder(const fs::path& rootIn) {
    const fs::path root = pipeline::normaliseSourcePath(rootIn);
    pipeline::ScanOptions so;
    so.recursive = recursiveBox_->isChecked();
    // Only the clip list is taken from this scan. Output folders, sidecars and archive copies depend on
    // the output folder, the layout and archive mode, which may still change: buildJob() scans again.
    so.preserveStructure = true;
    so.archiveMode = false;
    auto res = pipeline::scanFolder(root, outputRoot(), so);
    if (!res) {
        QMessageBox::warning(this, "Scan failed", QString::fromStdString(res.error().toString()));
        return;
    }
    const std::string key = pipeline::pathKey(root);
    auto it = std::find_if(folders_.begin(), folders_.end(), [&](const AddedFolder& f) { return pipeline::pathKey(f.root) == key; });
    fs::path listedRoot = root;
    if (it == folders_.end()) {
        folders_.push_back({root, so.recursive});
    } else {
        it->recursive = so.recursive; // added again: the latest choice decides
        listedRoot = it->root;
    }
    for (const auto& c : res->clips) model_->addClip(c.source, listedRoot);
    refreshSummary();
    updateButtons();
    emit stateChanged(this);
}

void BatchTab::onRemove() {
    QList<int> rows;
    for (const QModelIndex& i : table_->selectionModel()->selectedRows()) rows << i.row();
    std::set<fs::path> touched;
    for (int r : rows)
        if (r >= 0 && r < model_->rowCount()) touched.insert(model_->rows()[static_cast<size_t>(r)].root);
    model_->removeRows(rows);
    // A folder whose last clip was removed leaves the batch, archive copies included.
    for (const fs::path& root : touched) {
        if (root.empty()) continue;
        const bool stillListed =
            std::any_of(model_->rows().begin(), model_->rows().end(), [&](const ClipRow& c) { return c.root == root; });
        if (!stillListed)
            folders_.erase(std::remove_if(folders_.begin(), folders_.end(), [&](const AddedFolder& f) { return f.root == root; }),
                           folders_.end());
    }
    refreshSummary();
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

pipeline::BatchJob BatchTab::buildJob(QStringList* problems) const {
    pipeline::BatchJob job;
    job.name = title().toStdString();
    const fs::path outRoot = outputRoot();
    job.outputRoot = outRoot;
    job.archiveMode = archiveBox_->isChecked();
    settings_.applyTo(job.options);
    static const SizePreset sizes[] = {SizePreset::SixK, SizePreset::UHD, SizePreset::TwoK, SizePreset::FHD, SizePreset::Same};
    job.options.size = sizes[std::clamp(sizeBox_->currentIndex(), 0, 4)];
    job.options.outputFormat = chosenOutputFormat();
    static const QualityProfile profiles[] = {QualityProfile::Q1_Maximum, QualityProfile::Q3_High, QualityProfile::Q5_Normal,
                                              QualityProfile::Q7_Proxy};
    job.options.profile = profiles[std::clamp(profileBox_->currentIndex(), 0, 3)];
    job.options.kernel = kernelBox_->currentIndex() == 1 ? ResampleKernel::Area : ResampleKernel::Lanczos3;
    job.options.verify = verifyBox_->isChecked();
    job.options.useGpu = gpuBox_->isChecked();
    job.options.overwrite = overwriteBox_->isChecked();

    // Every added folder is scanned again now, so all destinations follow the current panel.
    const bool preserve = preserveBox_->isChecked();
    const std::vector<fs::path> outDirs = folderOutputDirs(outRoot);
    std::vector<std::map<fs::path, pipeline::ClipJob>> found(folders_.size());
    std::set<fs::path> copySources;
    for (size_t i = 0; i < folders_.size(); ++i) {
        pipeline::ScanOptions so;
        so.recursive = folders_[i].recursive;
        so.preserveStructure = preserve;
        so.archiveMode = job.archiveMode;
        auto res = pipeline::scanFolder(folders_[i].root, outDirs[i], so);
        if (!res) {
            if (problems) *problems << QString("%1: %2").arg(toQString(folders_[i].root), QString::fromStdString(res.error().message));
            continue;
        }
        for (auto& c : res->clips) {
            const fs::path key = c.source.lexically_normal();
            found[i].emplace(key, std::move(c));
        }
        // Folders may overlap (a folder and one of its sub-folders): each file is archived once.
        for (auto& cp : res->copies)
            if (copySources.insert(cp.source.lexically_normal()).second) job.copies.push_back(std::move(cp));
    }

    for (const ClipRow& row : model_->rows()) {
        pipeline::ClipJob cj;
        cj.source = row.source;
        cj.outputDir = outRoot;
        const auto f = std::find_if(folders_.begin(), folders_.end(), [&](const AddedFolder& a) { return a.root == row.root; });
        if (!row.root.empty() && f != folders_.end()) {
            const size_t i = static_cast<size_t>(f - folders_.begin());
            if (auto it = found[i].find(row.source.lexically_normal()); it != found[i].end()) {
                cj = it->second; // output folder, relative folder and sidecars as the scanner lays them out
            } else {
                // Listed when the folder was added but not found by this scan (e.g. sub-folder search was
                // switched off when the folder was added again): same layout rule as the scanner.
                fs::path rel;
                if (preserve) {
                    std::error_code ec;
                    rel = fs::relative(row.source.parent_path(), f->root, ec);
                    if (ec || rel == "." || (!rel.empty() && *rel.begin() == "..")) rel.clear();
                }
                cj.outputDir = rel.empty() ? outDirs[i] : outDirs[i] / rel;
                cj.relativeDir = rel;
            }
        }
        job.clips.push_back(std::move(cj));
    }
    return job;
}

void BatchTab::refreshSummary() {
    if (state_ != BatchState::Idle && state_ != BatchState::Finished) return;
    if (model_->rowCount() == 0 && folders_.empty()) {
        summaryLabel_->clear();
        return;
    }
    QStringList problems;
    const pipeline::BatchJob job = buildJob(&problems);
    QString text = QString("%1 clips").arg(job.clips.size());
    if (job.archiveMode) {
        size_t files = job.copies.size();
        for (const auto& c : job.clips) files += c.sidecars.size();
        text += QString(", %1 non-raw files to copy").arg(files);
    }
    if (!problems.isEmpty()) text += "\n" + problems.join("\n");
    summaryLabel_->setText(text);
}

void BatchTab::onStartClicked() {
    if (outputEdit_->text().isEmpty()) {
        QMessageBox::warning(this, "Output folder", "Choose an output folder first.");
        return;
    }
    QStringList problems;
    buildJob(&problems);
    if (!problems.isEmpty()) {
        QMessageBox::warning(this, "Cannot start", problems.join("\n"));
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
    // Built at the moment the batch really starts (it may have waited in the queue).
    QStringList problems;
    pipeline::BatchJob job = buildJob(&problems);
    if (!problems.isEmpty()) {
        summaryLabel_->setText("Not started:\n" + problems.join("\n"));
        setState(BatchState::Idle);
        emit statusText(QString("%1 not started").arg(title()));
        emit finished(this);
        return;
    }
    plannedCopies_ = job.copies.size();
    engine_ = std::make_unique<EngineBridge>(std::move(job));
    auto* w = engine_->worker();
    connect(w, &EngineWorker::clipStarted, this, [this](int i) { model_->setState(i, RowState::Probing); }, Qt::QueuedConnection);
    connect(w, &EngineWorker::clipProgress, this, [this](int i, qint64 done, qint64 total, double fps, QString stage) {
        model_->setProgress(i, pipeline::ClipProgress{done, total, fps, stage.toStdString()});
        emit statusText(QString("%1: frame %2/%3 at %4 fps").arg(title()).arg(done).arg(total).arg(fps, 0, 'f', 1));
    }, Qt::QueuedConnection);
    connect(w, &EngineWorker::clipDone, this, [this](int i, pipeline::ClipResult r) { model_->setResult(i, r); }, Qt::QueuedConnection);
    connect(w, &EngineWorker::copyDone, this, [this](int, bool ok, QString) {
        ok ? ++copiesDone_ : ++copiesFailed_;
        summaryLabel_->setText(QString("Copied %1 of %2 files (%3 failed)").arg(copiesDone_).arg(plannedCopies_).arg(copiesFailed_));
    }, Qt::QueuedConnection);
    connect(w, &EngineWorker::finished, this, [this](pipeline::BatchResult res) {
        QString text = QString("Finished: %1 done, %2 failed, %3 skipped, %4 cancelled, %5 copied")
                           .arg(res.done).arg(res.failed).arg(res.skipped).arg(res.cancelled).arg(res.copies.size());
        if (!res.copyErrors.empty()) text += QString(", %1 copies failed").arg(res.copyErrors.size());
        text += QString(" in %1 s").arg(res.seconds, 0, 'f', 1);
        if (res.skipped > res.skippedExisting)
            text += QString("\nNo output written for %1 skipped clips (see the Message column).").arg(res.skipped - res.skippedExisting);
        if (!res.manifest.empty()) text += "\nManifest: " + toQString(res.manifest);
        summaryLabel_->setText(text);
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
