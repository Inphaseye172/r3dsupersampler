// Table model for the clips of one batch, plus a delegate that paints the progress column.
#pragma once
#include <QAbstractTableModel>
#include <QStyledItemDelegate>
#include <filesystem>
#include <vector>
#include "r3dss/pipeline/clip_converter.hpp"

namespace r3dss::gui {

enum class RowState { Queued, Probing, Running, Encoding, Verifying, Done, Failed, Skipped, Copied, Cancelled };
QString rowStateName(RowState s);

// One line describing a finished clip (table message column and `r3dss-gui --run-job` output).
QString describeResult(const pipeline::ClipResult& r);

struct ClipRow {
    std::filesystem::path source;
    // The folder "Add folder" found this clip in (empty for clips added one by one). Output locations
    // are not stored per row: BatchTab::buildJob() derives them from the panel's state at start time.
    std::filesystem::path root;
    codec::Format sourceFormat = codec::Format::Unknown;
    QString format, resolution, fps, frames, output, message;
    RowState state = RowState::Queued;
    int progressPercent = 0;
    double currentFps = 0.0;
    bool probeOk = false;
};

class BatchModel final : public QAbstractTableModel {
    Q_OBJECT
public:
    enum Column { File = 0, Format, Resolution, Fps, Frames, Output, Status, Progress, Message, ColumnCount };
    explicit BatchModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& = {}) const override { return static_cast<int>(rows_.size()); }
    int columnCount(const QModelIndex& = {}) const override { return ColumnCount; }
    QVariant data(const QModelIndex& idx, int role) const override;
    QVariant headerData(int section, Qt::Orientation o, int role) const override;

    // Probes the clip (format, geometry) and appends it. Returns false if not a raw clip; a clip that is
    // already listed is not added again (returns true). `root`: the added folder it was found in.
    bool addClip(const std::filesystem::path& source, const std::filesystem::path& root = {});
    void removeRows(const QList<int>& rows);
    void clear();
    const std::vector<ClipRow>& rows() const { return rows_; }
    void setState(int row, RowState s, const QString& message = {});
    void setProgress(int row, const pipeline::ClipProgress& p);
    void setResult(int row, const pipeline::ClipResult& r);
    void resetForRun();

private:
    void touch(int row);
    std::vector<ClipRow> rows_;
};

class ProgressDelegate final : public QStyledItemDelegate {
    Q_OBJECT
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    void paint(QPainter* p, const QStyleOptionViewItem& opt, const QModelIndex& idx) const override;
};

} // namespace r3dss::gui
