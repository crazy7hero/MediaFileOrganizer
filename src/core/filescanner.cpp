#include "filescanner.h"
#include <QDirIterator>
#include <QDir>
#include <QFileInfo>
#include <QMetaObject>
#include <QtDebug>

FileScanner::FileScanner(QObject* parent)
    : QObject(parent)
    , m_cancelled(0)
{
    // 默认过滤器: 常见媒体文件
    m_filters << "*.mp4"  << "*.avi" << "*.mkv"  << "*.mov" << "*.wmv"
              << "*.flv"  << "*.webm"
              << "*.mp3"  << "*.wav" << "*.flac" << "*.aac" << "*.ogg"
              << "*.jpg"  << "*.jpeg" << "*.png" << "*.gif" << "*.bmp"
              << "*.tiff" << "*.webp" << "*.heic";
}

void FileScanner::setFilters(const QStringList& filters)
{
    m_filters = filters;
    if (m_filters.isEmpty()) {
        // 防御: 如果清空了过滤器, 恢复默认
        m_filters << "*.mp4" << "*.jpg" << "*.png";
    }
}

void FileScanner::scan(const QString& rootPath)
{
    if (rootPath.isEmpty() || !QDir(rootPath).exists()) {
        postFinished(0, 0);
        return;
    }

    QDirIterator it(rootPath, m_filters, QDir::Files | QDir::Readable,
                    QDirIterator::Subdirectories | QDirIterator::FollowSymlinks);

    QVector<FileEntry> batch;
    batch.reserve(200);
    int totalFiles    = 0;
    qint64 totalSize  = 0;

    const int BATCH_SIZE = 200;
    const int PROGRESS_INTERVAL = 100; // 每 100 个文件报告一次进度

    while (it.hasNext() && !isCancelled()) {
        const QString path = it.next();
        const QFileInfo fi = it.fileInfo();
        const qint64 size  = fi.size();

        batch.append(FileEntry(path, size));
        totalFiles++;
        totalSize += size;

        // 批次满 → 发送
        if (batch.size() >= BATCH_SIZE) {
            postBatch(batch);
            batch.clear();
            batch.reserve(BATCH_SIZE);
        }

        // 定期报告进度计数
        if (totalFiles % PROGRESS_INTERVAL == 0) {
            postProgress(totalFiles);
        }
    }

    // 发送剩余批次
    if (!batch.isEmpty() && !isCancelled()) {
        postBatch(batch);
    }

    // 最终进度
    postProgress(totalFiles);
    postFinished(totalFiles, totalSize);
}

void FileScanner::cancel()
{
    m_cancelled.store(1);
}

// ─── 安全投递信号到 UI 线程 ───

void FileScanner::postBatch(const QVector<FileEntry>& batch)
{
    QMetaObject::invokeMethod(this, [this, batch]() {
        if (!isCancelled())
            emit batchFound(batch);
    }, Qt::QueuedConnection);
}

void FileScanner::postProgress(int count)
{
    QMetaObject::invokeMethod(this, [this, count]() {
        emit progressUpdated(count);
    }, Qt::QueuedConnection);
}

void FileScanner::postFinished(int total, qint64 totalSize)
{
    QMetaObject::invokeMethod(this, [this, total, totalSize]() {
        emit scanFinished(total, totalSize);
    }, Qt::QueuedConnection);
}
