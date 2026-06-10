#ifndef FILECOPIERENGINE_H
#define FILECOPIERENGINE_H

#include <QObject>
#include <QStringList>
#include <QMap>
#include <QAtomicInt>
#include <QElapsedTimer>

// ─── 文件复制引擎 ───
// 在 DynamicThreadPool 的工作线程中执行复制
// 通过 QMetaObject::invokeMethod 安全地回传进度到 UI 线程
class FileCopierEngine : public QObject
{
    Q_OBJECT

public:
    explicit FileCopierEngine(QObject* parent = nullptr);

    // 在工作线程中调用
    void start(const QStringList& sourceFiles,
               const QMap<QString, qint64>& sizeMap,
               const QString& destinationDir);

    void cancel();
    bool isCancelled() const { return m_cancelled.loadRelaxed() != 0; }
    void reset() { m_cancelled.storeRelaxed(0); }

    // 设置是否跳过目标中已存在的同名同大小文件
    void setSkipExisting(bool skip) { m_skipExisting = skip; }

    // 设置是否保留源文件的时间戳
    void setPreserveTimestamp(bool preserve) { m_preserveTimestamp = preserve; }

signals:
    // 进度更新 (percent, copied, failed, total, bytesCopied)
    void progressUpdated(int percent, int copied, int failed, int total,
                         qint64 bytesCopied);

    // 复制完成
    void copyFinished(int totalFiles, int successCount, int failedCount,
                      qint64 totalBytes);

    // 单个文件错误
    void fileError(const QString& filePath, const QString& errorMessage);

    // 日志消息
    void logMessage(const QString& message);

private:
    void postProgress(int percent, int copied, int failed, int total,
                      qint64 bytes);
    void postFinished(int total, int success, int failed, qint64 bytes);
    void postFileError(const QString& path, const QString& err);
    void postLog(const QString& msg);

    QAtomicInt m_cancelled;
    bool       m_skipExisting       = false;
    bool       m_preserveTimestamp  = true;   // 默认保留时间戳
};

#endif // FILECOPIERENGINE_H
