#include "filescanner.h"
#include "ntfsscanner.h"
#include "ntfspipeserver.h"
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

    // ─── NTFS 快速路径: 管道服务 > 直接 MFT ───
    if (NTFSScanner::isNTFS(rootPath)) {
        // 1) 优先走管道服务 (无需管理员)
        QString diag;
        QVector<FileEntry> entries = NTFSPipeClient::scanViaPipe(rootPath, m_filters, &diag);
        if (!entries.isEmpty()) {
            QMetaObject::invokeMethod(this, [this]() {
                emit scanError("扫描模式: NTFS 快速扫描 (后台服务)");
            }, Qt::QueuedConnection);
            const int BATCH_SIZE = 500;
            qint64 totalSize = 0;
            QVector<FileEntry> batch; batch.reserve(BATCH_SIZE);
            for (int i = 0; i < entries.size() && !isCancelled(); ++i) {
                batch.append(entries[i]); totalSize += entries[i].fileSize;
                if (batch.size() >= BATCH_SIZE) { postBatch(batch); batch.clear(); batch.reserve(BATCH_SIZE); }
                if (i % 500 == 0) postProgress(i + 1);
            }
            if (!batch.isEmpty() && !isCancelled()) postBatch(batch);
            postProgress(entries.size());
            postFinished(entries.size(), totalSize);
            return;
        }

        // 2) 管道不通 → 尝试直接读 MFT (当前进程)
        if (!diag.isEmpty())
            QMetaObject::invokeMethod(this, [this, diag]() {
                emit scanError("管道诊断: " + diag);
            }, Qt::QueuedConnection);
        QString errMsg;
        entries = NTFSScanner::fastScan(rootPath, m_filters, &errMsg);
        if (!entries.isEmpty()) {
            QMetaObject::invokeMethod(this, [this]() {
                emit scanError("扫描模式: NTFS 快速扫描 (直接读 MFT)");
            }, Qt::QueuedConnection);
            const int BATCH_SIZE = 500;
            qint64 totalSize = 0;
            QVector<FileEntry> batch;
            batch.reserve(BATCH_SIZE);

            for (int i = 0; i < entries.size() && !isCancelled(); ++i) {
                batch.append(entries[i]);
                totalSize += entries[i].fileSize;
                if (batch.size() >= BATCH_SIZE) {
                    postBatch(batch);
                    batch.clear(); batch.reserve(BATCH_SIZE);
                }
                if (i % 500 == 0) postProgress(i + 1);
            }
            if (!batch.isEmpty() && !isCancelled())
                postBatch(batch);

            postProgress(entries.size());
            postFinished(entries.size(), totalSize);
            return;
        }
        // MFT 扫描失败 → 打印原因, 回退到慢速扫描
        if (!errMsg.isEmpty())
            QMetaObject::invokeMethod(this, [this, errMsg]() {
                emit scanError(errMsg);
            }, Qt::QueuedConnection);
    }

    // ─── 慢速路径: QDirIterator 遍历 ───
    QMetaObject::invokeMethod(this, [this]() {
        emit scanError("扫描模式: 普通扫描 (QDirIterator)");
    }, Qt::QueuedConnection);
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
