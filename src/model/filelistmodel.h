#ifndef FILELISTMODEL_H
#define FILELISTMODEL_H

#include <QAbstractTableModel>
#include <QString>
#include <QVector>
#include <QSet>
#include <QMap>
#include "mediahelper.h"

struct FileEntry {
    QString filePath;
    qint64  fileSize;
    FileEntry() : fileSize(0) {}
    FileEntry(const QString& p, qint64 s) : filePath(p), fileSize(s) {}
};

enum class FileCategory { Video, Image, Audio, Other };

// ─── 列定义 ───
// 0=☑  1=文件路径  2=大小  3=分辨率  4=时长  5=编码  6=操作
class FileListModel : public QAbstractTableModel
{
    Q_OBJECT

public:
    enum Column {
        ColCheck    = 0,
        ColPath     = 1,
        ColSize     = 2,
        ColRes      = 3,
        ColDuration = 4,
        ColCodec    = 5,
        ColAction   = 6,
        ColCount    = 7
    };

    explicit FileListModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QVariant headerData(int section, Qt::Orientation, int role) const override;
    Qt::ItemFlags flags(const QModelIndex& index) const override;
    bool setData(const QModelIndex& index, const QVariant& value, int role) override;

    // 数据操作
    void addFiles(const QVector<FileEntry>& entries);
    void clear();
    FileEntry entryAt(int row) const;
    QString  filePathAt(int row) const;
    qint64   fileSizeAt(int row) const;

    // 元数据
    void setMetadata(const QVector<MediaMetadata>& batch);
    bool hasMetadata(const QString& path) const;
    MediaMetadata metadataFor(const QString& path) const;

    // Checkbox
    void setAllChecked(bool checked);
    void invertChecked();
    QStringList checkedFiles() const;
    int checkedCount() const { return m_checked.size(); }

    // 汇总
    int    fileCount() const  { return m_entries.size(); }
    qint64 totalSize() const  { return m_totalSize; }
    int    videoCount() const { return m_videoCount; }
    int    imageCount() const { return m_imageCount; }
    int    audioCount() const { return m_audioCount; }
    qint64 videoSize() const  { return m_videoSize; }
    qint64 imageSize() const  { return m_imageSize; }
    qint64 audioSize() const  { return m_audioSize; }

signals:
    void countChanged(int count, qint64 totalSize);
    void checkedCountChanged(int count);

private:
    static FileCategory categorize(const QString& path);

    QVector<FileEntry>  m_entries;
    QHash<QString, int> m_pathToRow;  // 路径→行号 O(1) 查找
    QSet<QString>       m_checked;
    qint64              m_totalSize = 0;

    // 元数据缓存
    QMap<QString, MediaMetadata> m_metadata;

    // 分类统计
    int m_videoCount=0, m_imageCount=0, m_audioCount=0;
    qint64 m_videoSize=0, m_imageSize=0, m_audioSize=0;
};

#endif
