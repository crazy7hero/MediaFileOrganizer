#ifndef NTFSScanner_H
#define NTFSScanner_H

#include <QString>
#include <QStringList>
#include <QVector>
#include "filelistmodel.h"

// ─── NTFS MFT 快速扫描 ───
// 直接读取 NTFS $Mft 文件, 绕过文件系统 API 递归遍历
// 扫描速度: 100 万文件 < 10 秒
namespace NTFSScanner {

    // 判断路径是否在 NTFS 卷上
    bool isNTFS(const QString& path);

    // 快速扫描, 返回匹配过滤器的文件列表
    // 失败时返回空列表, errorMsg 包含失败原因
    QVector<FileEntry> fastScan(const QString& rootPath,
                                 const QStringList& filters,
                                 QString* errorMsg = nullptr);

} // namespace NTFSScanner

#endif // NTFSScanner_H
