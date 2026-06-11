#ifndef NTFSPIPESERVER_H
#define NTFSPIPESERVER_H

#include <QString>
#include <QStringList>
#include <QVector>
#include "filelistmodel.h"

// ─── 命名管道协议 ───
// 管道名: \\.\pipe\MediaFileOrganizer
// 客户端发送: SCAN:rootPath|ext1|ext2|...\n
// 服务端返回: COUNT:N\n + 每条 FILE:size|path\n + DONE\n

#define MFO_PIPE_NAME L"\\\\.\\pipe\\MediaFileOrganizer"

namespace NTFSPipeServer {

    // 启动管道服务器 (阻塞, 在服务线程中调用)
    void run();

    // 停止服务器
    void stop();

} // namespace NTFSPipeServer

namespace NTFSPipeClient {

    // 通过管道连接服务端进行快速扫描
    // 成功返回文件列表, 失败返回空 (服务未运行/权限不足)
    QVector<FileEntry> scanViaPipe(const QString& rootPath,
                                    const QStringList& filters);

    // 检测服务是否在运行
    bool isServiceRunning();

} // namespace NTFSPipeClient

// ─── Windows 服务管理 ───
namespace ServiceInstaller {

    bool install(const QString& exePath);
    bool uninstall();
    bool isInstalled();

} // namespace ServiceInstaller

#endif // NTFSPIPESERVER_H
