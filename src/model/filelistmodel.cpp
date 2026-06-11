#include "filelistmodel.h"
#include <QColor>
#include <QFont>
#include <QBrush>
#include <QFileInfo>

FileListModel::FileListModel(QObject* parent) : QAbstractTableModel(parent) {}

int FileListModel::rowCount(const QModelIndex& p) const
    { return p.isValid() ? 0 : m_entries.size(); }
int FileListModel::columnCount(const QModelIndex& p) const
    { return p.isValid() ? 0 : ColCount; }

QVariant FileListModel::data(const QModelIndex& idx, int role) const
{
    if (!idx.isValid() || idx.row() >= m_entries.size()) return QVariant();
    const FileEntry& e = m_entries.at(idx.row());

    // ─── Checkbox ───
    if (idx.column() == ColCheck) {
        if (role == Qt::CheckStateRole)
            return m_checked.contains(e.filePath) ? Qt::Checked : Qt::Unchecked;
        return QVariant();
    }

    // ─── 路径 ───
    if (idx.column() == ColPath) {
        if (role == Qt::DisplayRole) return e.filePath;
        if (role == Qt::ToolTipRole) return e.filePath;
    }

    // ─── 大小 ───
    if (idx.column() == ColSize) {
        if (role == Qt::DisplayRole) {
            qint64 sz = e.fileSize;
            if (sz < 1024)                return QString::number(sz)+" B";
            if (sz < 1048576)             return QString::number(sz/1024.0,'f',1)+" KB";
            if (sz < 1073741824LL)        return QString::number(sz/1048576.0,'f',2)+" MB";
            return QString::number(sz/1073741824.0,'f',2)+" GB";
        }
        if (role == Qt::ToolTipRole)   return QString::number(e.fileSize)+" 字节";
        if (role == Qt::TextAlignmentRole)
            return static_cast<int>(Qt::AlignRight | Qt::AlignVCenter);
        if (role == Qt::UserRole)      return QVariant(e.fileSize);
    }

    // ─── 元数据列 ───
    auto it = m_metadata.find(e.filePath);
    bool hasMeta = (it != m_metadata.end() && it->isValid);

    if (idx.column() == ColRes) {
        if (role == Qt::DisplayRole) {
            if (!hasMeta) return "...";
            const auto& m = *it;
            if (m.width > 0 && m.height > 0)
                return QString("%1×%2").arg(m.width).arg(m.height);
            return "-";
        }
        if (role == Qt::TextAlignmentRole)
            return static_cast<int>(Qt::AlignCenter);
        if (role == Qt::UserRole) {
            if (!hasMeta) return -1;
            return QVariant(it->width * it->height);
        }
    }

    if (idx.column() == ColDuration) {
        if (role == Qt::DisplayRole) {
            if (!hasMeta) return "...";
            const auto& m = *it;
            if (m.durationMs <= 0) return "-";
            qint64 secs = m.durationMs / 1000;
            int h = secs / 3600, mi = (secs % 3600) / 60, s = secs % 60;
            if (h > 0)
                return QString("%1:%2:%3").arg(h).arg(mi,2,10,QChar('0')).arg(s,2,10,QChar('0'));
            return QString("%1:%2").arg(mi).arg(s,2,10,QChar('0'));
        }
        if (role == Qt::TextAlignmentRole)
            return static_cast<int>(Qt::AlignCenter);
        if (role == Qt::UserRole) {
            if (!hasMeta) return -1;
            return QVariant(it->durationMs);
        }
    }

    if (idx.column() == ColCodec) {
        if (role == Qt::DisplayRole) {
            if (!hasMeta) return "...";
            const auto& m = *it;
            if (m.codec.isEmpty()) return "-";
            return m.codec;
        }
        if (role == Qt::ToolTipRole && hasMeta) {
            const auto& m = *it;
            QString tip = m.codec;
            if (!m.audioCodec.isEmpty()) tip += " / " + m.audioCodec;
            if (m.framerate > 0) tip += QString("  %1fps").arg(m.framerate,0,'f',1);
            return tip;
        }
        if (role == Qt::UserRole) {
            if (!hasMeta) return QString();
            return QVariant(it->codec);
        }
    }

    // ─── 操作 ───
    if (idx.column() == ColAction) {
        if (role == Qt::DisplayRole)   return QStringLiteral("🔍 浏览");
        if (role == Qt::ToolTipRole)   return QStringLiteral("在资源管理器中定位该文件");
        if (role == Qt::TextAlignmentRole)
            return static_cast<int>(Qt::AlignCenter);
        if (role == Qt::ForegroundRole) return QBrush(QColor("#1976D2"));
        if (role == Qt::FontRole) { QFont f; f.setUnderline(true); return f; }
    }

    return QVariant();
}

QVariant FileListModel::headerData(int sec, Qt::Orientation o, int role) const
{
    if (o != Qt::Horizontal || role != Qt::DisplayRole) return QVariant();
    switch (sec) {
    case ColCheck:    return QStringLiteral("☑");
    case ColPath:     return QStringLiteral("文件路径");
    case ColSize:     return QStringLiteral("大小");
    case ColRes:      return QStringLiteral("分辨率");
    case ColDuration: return QStringLiteral("时长");
    case ColCodec:    return QStringLiteral("编码");
    case ColAction:   return QStringLiteral("操作");
    }
    return QVariant();
}

Qt::ItemFlags FileListModel::flags(const QModelIndex& idx) const
{
    Qt::ItemFlags f = QAbstractTableModel::flags(idx);
    if (idx.column() == ColCheck) f |= Qt::ItemIsUserCheckable;
    return f;
}

bool FileListModel::setData(const QModelIndex& idx, const QVariant& v, int role)
{
    if (!idx.isValid() || idx.row() >= m_entries.size()) return false;
    if (idx.column() == ColCheck && role == Qt::CheckStateRole) {
        const QString& p = m_entries.at(idx.row()).filePath;
        if (v.toInt() == Qt::Checked) m_checked.insert(p);
        else                          m_checked.remove(p);
        emit checkedCountChanged(m_checked.size());
        emit dataChanged(idx, idx, {role});
        return true;
    }
    return false;
}

// ─── 数据操作 ───

void FileListModel::addFiles(const QVector<FileEntry>& entries)
{
    if (entries.isEmpty()) return;

    // 先去重, 算实际新增数
    QVector<FileEntry> uniq;
    uniq.reserve(entries.size());
    for (const auto& e : entries) {
        if (!m_pathToRow.contains(e.filePath))
            uniq.append(e);
    }
    if (uniq.isEmpty()) return;

    int old = m_entries.size();
    beginInsertRows(QModelIndex(), old, old + uniq.size() - 1);
    m_entries.reserve(old + uniq.size());
    m_pathToRow.reserve(old + uniq.size());
    for (const auto& e : uniq) {
        m_pathToRow[e.filePath] = m_entries.size();
        m_entries.append(e);
        m_totalSize += e.fileSize;
        switch (categorize(e.filePath)) {
        case FileCategory::Video: m_videoCount++; m_videoSize+=e.fileSize; break;
        case FileCategory::Image: m_imageCount++; m_imageSize+=e.fileSize; break;
        case FileCategory::Audio: m_audioCount++; m_audioSize+=e.fileSize; break;
        default: break;
        }
    }
    endInsertRows();
    emit countChanged(m_entries.size(), m_totalSize);
}

void FileListModel::clear()
{
    if (m_entries.isEmpty()) return;
    beginRemoveRows(QModelIndex(), 0, m_entries.size()-1);
    m_entries.clear(); m_pathToRow.clear(); m_checked.clear(); m_metadata.clear();
    m_totalSize=0; m_videoCount=m_imageCount=m_audioCount=0;
    m_videoSize=m_imageSize=m_audioSize=0;
    endRemoveRows();
    emit countChanged(0,0); emit checkedCountChanged(0);
}

FileEntry FileListModel::entryAt(int r) const
    { return (r>=0 && r<m_entries.size()) ? m_entries.at(r) : FileEntry(); }
QString FileListModel::filePathAt(int r) const
    { return (r>=0 && r<m_entries.size()) ? m_entries.at(r).filePath : QString(); }
qint64 FileListModel::fileSizeAt(int r) const
    { return (r>=0 && r<m_entries.size()) ? m_entries.at(r).fileSize : 0; }

// ─── 元数据 ───

void FileListModel::setMetadata(const QVector<MediaMetadata>& batch)
{
    int minRow = m_entries.size(), maxRow = -1;
    for (const auto& md : batch) {
        if (!md.isValid) continue;
        m_metadata[md.filePath] = md;
        auto it = m_pathToRow.find(md.filePath);
        if (it != m_pathToRow.end()) {
            int r = it.value();
            if (r < minRow) minRow = r;
            if (r > maxRow) maxRow = r;
        }
    }
    if (maxRow >= minRow) {
        emit dataChanged(index(minRow, ColRes), index(maxRow, ColCodec));
    }
}

bool FileListModel::hasMetadata(const QString& p) const
    { return m_metadata.contains(p) && m_metadata[p].isValid; }
MediaMetadata FileListModel::metadataFor(const QString& p) const
    { return m_metadata.value(p); }

// ─── Checkbox ───

void FileListModel::setAllChecked(bool ck)
{
    if (ck) for (const auto& e : m_entries) m_checked.insert(e.filePath);
    else    m_checked.clear();
    emit checkedCountChanged(m_checked.size());
    emit dataChanged(index(0,ColCheck), index(m_entries.size()-1,ColCheck), {Qt::CheckStateRole});
}

void FileListModel::invertChecked()
{
    QSet<QString> n;
    for (const auto& e : m_entries)
        if (!m_checked.contains(e.filePath)) n.insert(e.filePath);
    m_checked = n;
    emit checkedCountChanged(m_checked.size());
    emit dataChanged(index(0,ColCheck), index(m_entries.size()-1,ColCheck), {Qt::CheckStateRole});
}

QStringList FileListModel::checkedFiles() const
{
    QStringList r; r.reserve(m_checked.size());
    for (const auto& e : m_entries)
        if (m_checked.contains(e.filePath)) r << e.filePath;
    return r;
}

// ─── 分类 ───

FileCategory FileListModel::categorize(const QString& path)
{
    QString e = QFileInfo(path).suffix().toLower();
    if (e=="mp4"||e=="avi"||e=="mkv"||e=="mov"||e=="wmv"||e=="flv"||e=="webm"||
        e=="mts"||e=="m2ts"||e=="ts"||e=="3gp") return FileCategory::Video;
    if (e=="jpg"||e=="jpeg"||e=="png"||e=="gif"||e=="bmp"||e=="tiff"||e=="tif"||
        e=="webp"||e=="heic"||e=="heif"||e=="raw"||e=="cr2"||e=="nef"||e=="arw")
        return FileCategory::Image;
    if (e=="mp3"||e=="wav"||e=="flac"||e=="aac"||e=="ogg"||e=="wma"||e=="m4a"||
        e=="opus") return FileCategory::Audio;
    return FileCategory::Other;
}
