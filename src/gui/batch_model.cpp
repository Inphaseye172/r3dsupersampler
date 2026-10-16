#include "batch_model.hpp"
#include "qt_paths.hpp"
#include <QApplication>
#include <QPainter>
#include <QStyleOptionProgressBar>
#include "r3dss/codec/codec.hpp"

namespace r3dss::gui {

QString rowStateName(RowState s) {
    switch (s) {
    case RowState::Queued: return "Queued";
    case RowState::Probing: return "Probing";
    case RowState::Running: return "Running";
    case RowState::Encoding: return "Encoding";
    case RowState::Verifying: return "Verifying";
    case RowState::Done: return "Done";
    case RowState::Failed: return "Failed";
    case RowState::Skipped: return "Skipped";
    case RowState::Copied: return "Copied";
    case RowState::Cancelled: return "Cancelled";
    }
    return "?";
}

QString describeResult(const pipeline::ClipResult& res) {
    QString msg = QString::fromStdString(res.message);
    if (res.status == pipeline::ClipStatus::Done) {
        // A bit budget exists only for output codecs with rate control; CinemaDNG output is always lossless.
        const QString coding = res.rateControl ? QString("budget %1 bpp").arg(res.averageBudgetBpp, 0, 'f', 2) : QString("lossless");
        msg = QString("%1x%2 -> %3x%4, %5 frames, %6 s, %7 MiB, %8 bpp (%9, %10)")
                  .arg(res.srcWidth).arg(res.srcHeight).arg(res.outWidth).arg(res.outHeight).arg(res.frames)
                  .arg(res.seconds, 0, 'f', 1).arg(static_cast<double>(res.outputBytes) / (1024.0 * 1024.0), 0, 'f', 1)
                  .arg(res.averageBpp, 0, 'f', 2).arg(coding, QString::fromStdString(res.resampler));
    }
    for (const auto& w : res.warnings) msg += "; warning: " + QString::fromStdString(w);
    for (const auto& e : res.copyErrors) msg += "; archive copy failed: " + QString::fromStdString(e);
    return msg;
}

BatchModel::BatchModel(QObject* parent) : QAbstractTableModel(parent) {}

QVariant BatchModel::headerData(int section, Qt::Orientation o, int role) const {
    if (o != Qt::Horizontal || role != Qt::DisplayRole) return {};
    static const char* names[] = {"File", "Format", "Resolution", "FPS", "Frames", "Output", "Status", "Progress", "Message"};
    return section < ColumnCount ? QString(names[section]) : QString();
}

QVariant BatchModel::data(const QModelIndex& idx, int role) const {
    if (!idx.isValid() || idx.row() >= rowCount()) return {};
    const ClipRow& r = rows_[static_cast<size_t>(idx.row())];
    if (role == Qt::ToolTipRole) return toQString(r.source) + "\n" + r.message;
    if (role == Qt::UserRole) return r.progressPercent;
    if (role == Qt::ForegroundRole) {
        if (r.state == RowState::Failed) return QColor(200, 60, 60);
        if (r.state == RowState::Done || r.state == RowState::Copied) return QColor(60, 150, 80);
        return {};
    }
    if (role != Qt::DisplayRole) return {};
    switch (idx.column()) {
    case File: return toQString(r.source.filename());
    case Format: return r.format;
    case Resolution: return r.resolution;
    case Fps: return r.fps;
    case Frames: return r.frames;
    case Output: return r.output;
    case Status: {
        QString s = rowStateName(r.state);
        if (r.state == RowState::Running && r.currentFps > 0) s += QString(" (%1 fps)").arg(r.currentFps, 0, 'f', 1);
        return s;
    }
    case Progress: return r.progressPercent;
    case Message: return r.message;
    }
    return {};
}

bool BatchModel::addClip(const std::filesystem::path& source, const std::filesystem::path& root) {
    for (const auto& r : rows_)
        if (r.source == source) return true; // already listed
    auto factory = codec::CodecRegistry::instance().probe(source);
    if (!factory) return false;
    ClipRow row;
    row.source = source;
    row.root = root;
    row.sourceFormat = factory->format();
    row.format = QString::fromStdString(codec::formatName(factory->format()));
    auto reader = factory->makeReader();
    auto info = reader->open(source);
    if (info) {
        row.probeOk = true;
        if (!info->sourceLabel.empty()) row.format = QString::fromStdString(info->sourceLabel);
        row.resolution = QString("%1x%2").arg(info->width).arg(info->height);
        row.fps = QString::number(info->fps.toDouble(), 'f', 3);
        row.frames = QString::number(info->frameCount);
        if (!factory->status().usable()) row.message = QString::fromStdString(factory->status().text);
    } else {
        row.message = QString::fromStdString(info.error().toString());
    }
    beginInsertRows({}, rowCount(), rowCount());
    rows_.push_back(std::move(row));
    endInsertRows();
    return true;
}

void BatchModel::removeRows(const QList<int>& rows) {
    QList<int> sorted = rows;
    std::sort(sorted.begin(), sorted.end(), std::greater<int>());
    for (int r : sorted) {
        if (r < 0 || r >= rowCount()) continue;
        beginRemoveRows({}, r, r);
        rows_.erase(rows_.begin() + r);
        endRemoveRows();
    }
}

void BatchModel::clear() {
    beginResetModel();
    rows_.clear();
    endResetModel();
}

void BatchModel::touch(int row) { emit dataChanged(index(row, 0), index(row, ColumnCount - 1)); }

void BatchModel::setState(int row, RowState s, const QString& message) {
    if (row < 0 || row >= rowCount()) return;
    rows_[static_cast<size_t>(row)].state = s;
    if (!message.isEmpty()) rows_[static_cast<size_t>(row)].message = message;
    touch(row);
}

void BatchModel::setProgress(int row, const pipeline::ClipProgress& p) {
    if (row < 0 || row >= rowCount()) return;
    ClipRow& r = rows_[static_cast<size_t>(row)];
    r.progressPercent = p.totalFrames ? static_cast<int>(100 * p.framesDone / p.totalFrames) : 0;
    r.currentFps = p.fps;
    if (p.stage == "verifying") r.state = RowState::Verifying;
    else if (p.stage == "audio") r.state = RowState::Encoding;
    else r.state = RowState::Running;
    touch(row);
}

void BatchModel::setResult(int row, const pipeline::ClipResult& res) {
    if (row < 0 || row >= rowCount()) return;
    ClipRow& r = rows_[static_cast<size_t>(row)];
    switch (res.status) {
    case pipeline::ClipStatus::Done: r.state = RowState::Done; r.progressPercent = 100; break;
    case pipeline::ClipStatus::Failed: r.state = RowState::Failed; break;
    case pipeline::ClipStatus::Skipped: r.state = RowState::Skipped; break;
    case pipeline::ClipStatus::Cancelled: r.state = RowState::Cancelled; break;
    }
    r.output = toQString(res.output.filename());
    r.message = describeResult(res);
    r.currentFps = 0;
    touch(row);
}

void BatchModel::resetForRun() {
    for (auto& r : rows_) {
        r.state = RowState::Queued;
        r.progressPercent = 0;
        r.currentFps = 0;
        if (r.probeOk) r.message.clear();
    }
    if (!rows_.empty()) emit dataChanged(index(0, 0), index(rowCount() - 1, ColumnCount - 1));
}

void ProgressDelegate::paint(QPainter* p, const QStyleOptionViewItem& opt, const QModelIndex& idx) const {
    if (idx.column() != BatchModel::Progress) {
        QStyledItemDelegate::paint(p, opt, idx);
        return;
    }
    QStyleOptionProgressBar bar;
    bar.rect = opt.rect.adjusted(2, 2, -2, -2);
    bar.minimum = 0;
    bar.maximum = 100;
    bar.progress = idx.data(Qt::UserRole).toInt();
    bar.text = QString("%1%").arg(bar.progress);
    bar.textVisible = true;
    QApplication::style()->drawControl(QStyle::CE_ProgressBar, &bar, p);
}

} // namespace r3dss::gui
