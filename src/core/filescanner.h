#ifndef FILESCANNER_H
#define FILESCANNER_H

#include <QObject>
#include <QStringList>
#include <QVector>
#include <QAtomicInt>
#include "filelistmodel.h"

// ─── 文件扫描器 ───
// 在 DynamicThreadPool 的工作线程中执行扫描
// 通过 QMetaObject::invokeMethod 安全地回传数据到 UI 线程
class FileScanner : public QObject
{
    Q_OBJECT

public:
    explicit FileScanner(QObject* parent = nullptr);

    // 设置扫描的扩展名过滤器, 如 {"*.mp4","*.jpg","*.png"}
    void setFilters(const QStringList& filters);
    QStringList filters() const { return m_filters; }

    // 在调用线程中执行扫描 (由线程池任务调用)
    // 注意: 此方法直接在工作线程中运行, 不要从 UI 线程调用
    void scan(const QString& rootPath);

    // 取消正在进行的扫描
    void cancel();
    bool isCancelled() const { return m_cancelled.loadRelaxed() != 0; }

    // 重置取消标记
    void reset() { m_cancelled.storeRelaxed(0); }

signals:
    // 批量发现文件 (在 UI 线程中发射, 安全连接)
    void batchFound(const QVector<FileEntry>& batch);

    // 扫描进度 (实时文件计数)
    void progressUpdated(int filesFound);

    // 扫描完成
    void scanFinished(int totalFiles, qint64 totalSize);

    // 扫描错误/警告
    void scanError(const QString& message);

private:
    // 从工作线程投递信号到 UI 线程
    void postBatch(const QVector<FileEntry>& batch);
    void postProgress(int count);
    void postFinished(int total, qint64 totalSize);

    QStringList m_filters;
    QAtomicInt  m_cancelled;
};

#endif // FILESCANNER_H
