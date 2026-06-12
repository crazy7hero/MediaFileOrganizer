#include "ntfsscanner.h"
#include <windows.h>
#include <QDir>
#include <QFileInfo>
#include <QSet>
#include <QStack>

// ─── 解析扩展名过滤器 ───
static QSet<QString> parseExtFilters(const QStringList& filters)
{
    QSet<QString> exts;
    for (const QString& f : filters) {
        QString e = f.toLower();
        if (e.startsWith("*."))      exts.insert(e.mid(2));
        else if (e.startsWith("."))  exts.insert(e.mid(1));
        else                         exts.insert(e);
    }
    return exts;
}

// ─── 递归快速扫描 (FindFirstFileEx + FindExInfoBasic + 大缓冲区) ───
// 比 QDirIterator 快 2-3 倍: 跳过 8.3 短文件名, 批量读取
static void fastScanRecurse(const QString& dirPath, const QSet<QString>& wantedExts,
                             QVector<FileEntry>& results)
{
    QString search = dirPath + "\\*";
    if (search.startsWith("\\\\")) search = "\\\\.\\" + dirPath + "\\*";

    WIN32_FIND_DATAW fd;
    HANDLE hFind = FindFirstFileExW(
        reinterpret_cast<LPCWSTR>(search.utf16()),
        FindExInfoBasic,           // 跳过 8.3 短文件名 → 显著加速
        &fd,
        FindExSearchNameMatch,
        nullptr,
        FIND_FIRST_EX_LARGE_FETCH); // 大缓冲区批量读取

    if (hFind == INVALID_HANDLE_VALUE) return;

    do {
        // 跳过 . 和 ..
        if (fd.cFileName[0] == L'.' && (fd.cFileName[1] == 0 ||
            (fd.cFileName[1] == L'.' && fd.cFileName[2] == 0)))
            continue;

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            // 跳过 junction/reparse point (避免循环)
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
            QString subDir = dirPath + "\\" + QString::fromWCharArray(fd.cFileName);
            fastScanRecurse(subDir, wantedExts, results);
        } else {
            // 文件: 检查扩展名
            QString name = QString::fromWCharArray(fd.cFileName);
            int dot = name.lastIndexOf('.');
            if (dot >= 0 && wantedExts.contains(name.mid(dot + 1).toLower())) {
                LARGE_INTEGER sz;
                sz.LowPart  = fd.nFileSizeLow;
                sz.HighPart = fd.nFileSizeHigh;
                results.append(FileEntry(dirPath + "\\" + name, sz.QuadPart));
            }
        }
    } while (FindNextFileW(hFind, &fd));

    FindClose(hFind);
}

// ─── 判断 NTFS ───
bool NTFSScanner::isNTFS(const QString& path)
{
    if (path.length() < 2 || path.at(1) != ':') return false;
    wchar_t fsName[MAX_PATH];
    QString root = path.left(2) + "\\";
    if (!GetVolumeInformationW(reinterpret_cast<LPCWSTR>(root.utf16()),
                               nullptr, 0, nullptr, nullptr, nullptr,
                               fsName, MAX_PATH))
        return false;
    return QString::fromWCharArray(fsName) == "NTFS";
}

// ─── 快速扫描 ───
QVector<FileEntry> NTFSScanner::fastScan(const QString& srcPath,
                                           const QStringList& filters,
                                           QString* /*errorMsg*/)
{
    QVector<FileEntry> results;
    if (filters.isEmpty()) return results;

    QSet<QString> wantedExts = parseExtFilters(filters);
    if (wantedExts.isEmpty()) return results;

    fastScanRecurse(srcPath, wantedExts, results);
    return results;
}
