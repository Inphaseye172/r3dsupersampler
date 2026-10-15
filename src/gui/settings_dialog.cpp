#include "settings_dialog.hpp"
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QSpinBox>
#include <QTabWidget>
#include <QTableWidget>
#include <QHeaderView>
#include <QVBoxLayout>
#include "r3dss/codec/codec.hpp"
#include "r3dss/core/hardware.hpp"
#include "r3dss/pipeline/clip_converter.hpp"

namespace r3dss::gui {

SettingsDialog::SettingsDialog(const AppSettings& s, QWidget* parent) : QDialog(parent), base_(s) {
    setWindowTitle("Settings");
    resize(640, 420);
    auto* tabs = new QTabWidget(this);

    auto* engine = new QWidget(this);
    auto* form = new QFormLayout(engine);
    const HardwareInfo hw = probeHardware();
    const auto rc = pipeline::chooseResampler(true);
    form->addRow("Hardware", new QLabel(QString("%1 logical cores, %2 GiB RAM, resampler: %3")
                                            .arg(hw.logicalCores).arg(hw.totalRamBytes >> 30)
                                            .arg(QString::fromStdString(rc.name)), this));
    gpu_ = new QCheckBox("Use CUDA GPU when available", this);
    gpu_->setChecked(s.useGpu);
    parallel_ = new QSpinBox(this);
    parallel_->setRange(0, 16);
    parallel_->setSpecialValueText("automatic (cores / 8, 1..4)");
    parallel_->setValue(static_cast<int>(s.maxParallelClips));
    threads_ = new QSpinBox(this);
    threads_->setRange(0, 256);
    threads_->setSpecialValueText("all cores");
    threads_->setValue(static_cast<int>(s.maxThreads));
    naming_ = new QLineEdit(s.namingTemplate, this);
    naming_->setToolTip("{stem} {size} {profile} {format} {kernel}");
    logLevel_ = new QComboBox(this);
    logLevel_->addItems({"trace", "debug", "info", "warn", "error"});
    logLevel_->setCurrentText(s.logLevel);
    form->addRow(gpu_);
    form->addRow("Parallel clips", parallel_);
    form->addRow("Thread limit", threads_);
    form->addRow("Output name template", naming_);
    form->addRow("Log level", logLevel_);
    tabs->addTab(engine, "Engine");

    auto* codecs = new QTableWidget(this);
    codecs->setColumnCount(4);
    codecs->setHorizontalHeaderLabels({"Module", "Read", "Write", "Status"});
    const auto all = codec::CodecRegistry::instance().all();
    codecs->setRowCount(static_cast<int>(all.size()));
    int row = 0;
    for (const auto& f : all) {
        codecs->setItem(row, 0, new QTableWidgetItem(QString::fromStdString(f->displayName())));
        codecs->setItem(row, 1, new QTableWidgetItem(f->caps().read ? "yes" : "no"));
        codecs->setItem(row, 2, new QTableWidgetItem(f->caps().write ? "yes" : "no"));
        codecs->setItem(row, 3, new QTableWidgetItem(QString::fromStdString(f->status().text)));
        ++row;
    }
    codecs->horizontalHeader()->setStretchLastSection(true);
    codecs->setEditTriggers(QAbstractItemView::NoEditTriggers);
    codecs->setWordWrap(true);
    codecs->resizeRowsToContents();
    tabs->addTab(codecs, "Codec modules");

    auto* about = new QLabel(
        "<h3>r3dss 0.2 (preview, product name not chosen)</h3>"
        "<p>Goal: genuine camera RAW in, genuine camera RAW out at lower resolution and smaller size, resampled "
        "in the Bayer domain without debayering.</p>"
        "<p>This preview converts CinemaDNG mosaics to smaller CinemaDNG mosaics. REDCODE RAW (RED KOMODO), Nikon "
        "R3D NE and NEV clips are recognised; RAW-to-RAW conversion of those formats is not available (see the "
        "Codec modules tab and docs/FEASIBILITY.md for the exact blockers).</p>"
        "<p><b>DaVinci Resolve note:</b> half-resolution R3D NE decode can ring near clipped highlights. "
        "Set the decode quality to <i>Full Res Premium</i> in the project or clip Camera RAW settings.</p>",
        this);
    about->setWordWrap(true);
    about->setTextInteractionFlags(Qt::TextSelectableByMouse);
    tabs->addTab(about, "About");

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    auto* layout = new QVBoxLayout(this);
    layout->addWidget(tabs);
    layout->addWidget(buttons);
}

AppSettings SettingsDialog::settings() const {
    AppSettings s = base_;
    s.useGpu = gpu_->isChecked();
    s.maxParallelClips = static_cast<unsigned>(parallel_->value());
    s.maxThreads = static_cast<unsigned>(threads_->value());
    s.namingTemplate = naming_->text();
    s.logLevel = logLevel_->currentText();
    return s;
}

} // namespace r3dss::gui
