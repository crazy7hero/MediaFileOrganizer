#include "filecopierengine.h"
#include <windows.h>
#include <QDir>
#include <QFileInfo>
#include <QMetaObject>
#include <QDateTime>

// ═══════════════════════════════════════════════════════════════
// CopyFileEx 进度回调 — 实现可中断的大文件复制
// ═══════════════════════════════════════════════════════════════
//
// 设计要点:
//   1. 总是串行复制 (一次一个文件) — 避免 HDD 磁头争抢
//   2. CopyFileEx 回调中检查取消标志 → 大文件也能中途停止
//   3. 回调在工作线程中, 只读取消标志, 不操作 UI
//
static DWORD CALLBACK copyProgressCallback(
    LARGE_INTEGER /*totalSize*/,
    LARGE_INTEGER /*transferred*/,
    LARGE_INTEGER /*streamSize*/,
    LARGE_INTEGER /*streamTransferred*/,
    DWORD          /*streamNumber*/,
    DWORD          /*callbackReason*/,
    HANDLE         /*srcHandle*/,
    HANDLE         /*dstHandle*/,
    LPVOID         lpData)
{
    const FileCopierEngine* engine = static_cast<const FileCopierEngine*>(lpData);
    if (engine->isCancelled()) {
        return PROGRESS_CANCEL;       // ← 中断 CopyFileEx
    }
    return PROGRESS_CONTINUE;
}

// ═══════════════════════════════════════════════════════════════

FileCopierEngine::FileCopierEngine(QObject* parent)
    : QObject(parent)
    , m_cancelled(0)
{
}

void FileCopierEngine::start(const QStringList& sourceFiles,
                              const QMap<QString, qint64>& sizeMap,
                              const QString& destinationDir)
{
    if (sourceFiles.isEmpty() || destinationDir.isEmpty()) {
        postFinished(0, 0, 0, 0);
        return;
    }

    // 确保目标目录存在
    QDir dir(destinationDir);
    if (!dir.exists()) {
        dir.mkpath(".");
    }

    const int total   = sourceFiles.size();
    int copied        = 0;
    int failed        = 0;
    int skipped       = 0;
    qint64 totalBytes = 0;

    QElapsedTimer progressTimer;
    progressTimer.start();

    postLog(QString("开始复制 %1 个文件 → %2 (串行模式, 避免磁盘争抢)")
                .arg(total).arg(destinationDir));

    // ─── 串行复制: 一次只复制一个文件 ───
    // 这样避免 HDD 磁头在多文件之间反复寻道,
    // 实际吞吐量远高于多线程并发复制
    for (int i = 0; i < total && !isCancelled(); ++i) {
        const QString& srcPath  = sourceFiles.at(i);
        const QFileInfo srcInfo(srcPath);
        const QString  fileName = srcInfo.fileName();
        QString        dstPath  = destinationDir + "/" + fileName;

        // ─── 去重 ───
        if (QFile::exists(dstPath)) {
            QString base    = srcInfo.baseName();
            QString suffix  = srcInfo.completeSuffix();
            int     counter = 1;

            do {
                if (suffix.isEmpty())
                    dstPath = destinationDir + "/" + base + "_" + QString::number(counter);
                else
                    dstPath = destinationDir + "/" + base + "_" + QString::number(counter) + "." + suffix;
                ++counter;
            } while (QFile::exists(dstPath) && counter < 10000);
        }

        // ─── 跳过已存在文件 (同名 + 同大小) ───
        if (m_skipExisting) {
            QFileInfo existCheck(dstPath);
            if (existCheck.exists() && existCheck.size() == sizeMap.value(srcPath, 0)) {
                ++skipped;
                continue;
            }
        }

        // ─── CopyFileEx: 支持回调 + 可中断 ───
        BOOL ret = CopyFileExW(
            reinterpret_cast<LPCWSTR>(srcPath.utf16()),
            reinterpret_cast<LPCWSTR>(dstPath.utf16()),
            copyProgressCallback,          // 进度/取消回调
            const_cast<FileCopierEngine*>(this), // 传 this 给回调
            nullptr,                       // 不需要 pbCancel (用回调返回值)
            0);                            // flags

        if (ret) {
            ++copied;
            qint64 sz = sizeMap.value(srcPath, 0);
            totalBytes += sz;

            // ─── 保留原始时间戳 ───
            if (m_preserveTimestamp) {
                HANDLE hDst = CreateFileW(
                    reinterpret_cast<LPCWSTR>(dstPath.utf16()),
                    FILE_WRITE_ATTRIBUTES,
                    FILE_SHARE_READ | FILE_SHARE_WRITE,
                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (hDst != INVALID_HANDLE_VALUE) {
                    FILETIME ftCreate, ftAccess, ftWrite;
                    HANDLE hSrc = CreateFileW(
                        reinterpret_cast<LPCWSTR>(srcPath.utf16()),
                        GENERIC_READ,
                        FILE_SHARE_READ,
                        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
                    if (hSrc != INVALID_HANDLE_VALUE) {
                        GetFileTime(hSrc, &ftCreate, &ftAccess, &ftWrite);
                        SetFileTime(hDst, &ftCreate, &ftAccess, &ftWrite);
                        CloseHandle(hSrc);
                    }
                    CloseHandle(hDst);
                }
            }
        } else {
            ++failed;
            DWORD err = GetLastError();
            QString errMsg;

            // 如果是因为取消导致的失败, 不记录错误
            if (isCancelled()) break;

            switch (err) {
            case ERROR_FILE_NOT_FOUND:   errMsg = "源文件未找到"; break;
            case ERROR_PATH_NOT_FOUND:   errMsg = "路径未找到"; break;
            case ERROR_ACCESS_DENIED:    errMsg = "访问被拒绝"; break;
            case ERROR_DISK_FULL:        errMsg = "磁盘已满"; break;
            case ERROR_FILE_EXISTS:      errMsg = "目标已存在"; break;
            case ERROR_WRITE_PROTECT:    errMsg = "写保护"; break;
            case ERROR_NOT_READY:        errMsg = "设备未就绪"; break;
            case ERROR_HANDLE_DISK_FULL: errMsg = "磁盘空间不足"; break;
            case ERROR_REQUEST_ABORTED:  errMsg = "操作被取消"; break;
            default: errMsg = QString("错误码: %1").arg(err); break;
            }
            postFileError(srcPath, errMsg);
        }

        // ─── 限流进度报告 ───
        bool isMilestone  = ((i + 1) % 50 == 0);
        bool timeToReport = (progressTimer.elapsed() >= 300);
        bool isLast       = (i == total - 1);

        if (isMilestone || timeToReport || isLast) {
            int pct = static_cast<int>((i + 1) * 100LL / total);
            postProgress(pct, copied, failed, total, totalBytes);
            progressTimer.restart();
        }
    }

    if (isCancelled()) {
        postLog("⚠ 操作已被用户取消");
    }

    if (skipped > 0) {
        postLog(QString("  已跳过 %1 个目标中已存在的文件").arg(skipped));
    }

    postFinished(total, copied, failed, totalBytes);
}

void FileCopierEngine::cancel()
{
    m_cancelled.storeRelaxed(1);
}

// ─── 安全投递到 UI 线程 ───

void FileCopierEngine::postProgress(int percent, int copied, int failed,
                                     int total, qint64 bytes)
{
    QMetaObject::invokeMethod(this, [this, percent, copied, failed, total, bytes]() {
        emit progressUpdated(percent, copied, failed, total, bytes);
    }, Qt::QueuedConnection);
}

void FileCopierEngine::postFinished(int total, int success, int failed,
                                     qint64 bytes)
{
    QMetaObject::invokeMethod(this, [this, total, success, failed, bytes]() {
        emit copyFinished(total, success, failed, bytes);
    }, Qt::QueuedConnection);
}

void FileCopierEngine::postFileError(const QString& path, const QString& err)
{
    QMetaObject::invokeMethod(this, [this, path, err]() {
        emit fileError(path, err);
    }, Qt::QueuedConnection);
}

void FileCopierEngine::postLog(const QString& msg)
{
    QMetaObject::invokeMethod(this, [this, msg]() {
        emit logMessage(msg);
    }, Qt::QueuedConnection);
}
