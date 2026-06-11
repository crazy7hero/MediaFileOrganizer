#include "ntfsscanner.h"
#include <windows.h>
#include <winioctl.h>
#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QSet>
#include <QDebug>

// ─── 缺失的结构体定义 (MinGW 头文件可能没有) ───

#ifndef FSCTL_GET_NTFS_VOLUME_DATA
#define FSCTL_GET_NTFS_VOLUME_DATA CTL_CODE(FILE_DEVICE_FILE_SYSTEM, 30, METHOD_BUFFERED, FILE_ANY_ACCESS)
#endif

// NTFS_VOLUME_DATA_BUFFER 已在 MinGW winioctl.h 中定义

#ifndef SE_BACKUP_NAME
#define SE_BACKUP_NAME L"SeBackupPrivilege"
#endif

// ─── MFT 记录头 (FILE record header) ───
#pragma pack(push, 1)
struct MftRecordHeader {
    char     magic[4];
    uint16_t usaOffset;
    uint16_t usaCount;
    uint64_t lsn;
    uint16_t seqNumber;
    uint16_t linkCount;
    uint16_t firstAttrOffset;
    uint16_t flags;           // 0x0001=在用, 0x0002=目录
    uint32_t bytesInUse;
    uint32_t bytesAllocated;
    uint64_t baseRecordRef;
    uint16_t nextAttrId;
    // 对齐填充...
};

struct AttrHeader {
    uint32_t type;
    uint32_t length;
    uint8_t  nonResident;
    uint8_t  nameLength;
    uint16_t nameOffset;
    uint16_t flags;
    uint16_t attrId;
    uint32_t bodyLength;
    uint16_t bodyOffset;
    uint8_t  indexed;
    uint8_t  reserved;
};

struct FileNameAttrBody {
    uint64_t parentRef;
    uint64_t createTime;
    uint64_t modifyTime;
    uint64_t mftChangeTime;
    uint64_t accessTime;
    uint64_t allocSize;
    uint64_t realSize;
    uint32_t fileAttr;
    uint32_t reparse;
    uint8_t  nameLength;
    wchar_t  name[1];
};
#pragma pack(pop)

// ─── 目录信息 (用于路径还原) ───
struct DirInfo {
    uint64_t parentRef;
    QString  name;
};

// ─── NTFS 卷参数 ───
struct NtfsVolume {
    HANDLE hVol = INVALID_HANDLE_VALUE;
    DWORD  bytesPerCluster = 4096;
    DWORD  bytesPerRecord  = 1024;
    ULONGLONG mftStartLcn  = 0;
    ULONGLONG totalRecords = 0;
    QString volumeRoot;     // "D:"
    QString rootPath;       // "D:\\"
};

// ─── 启用备份权限 ───
static bool enableBackupPrivilege()
{
    HANDLE hToken;
    if (!OpenProcessToken(GetCurrentProcess(),
                          TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken))
        return false;

    TOKEN_PRIVILEGES tp;
    LookupPrivilegeValueW(nullptr, SE_BACKUP_NAME, &tp.Privileges[0].Luid);
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;

    BOOL ok = AdjustTokenPrivileges(hToken, FALSE, &tp, sizeof(tp), nullptr, nullptr);
    CloseHandle(hToken);
    return ok && GetLastError() == ERROR_SUCCESS;
}

// ─── 打开 NTFS 卷, 读取参数 ───
static bool openVolume(const QString& path, NtfsVolume& vol)
{
    vol.volumeRoot = path.left(2);
    vol.rootPath   = vol.volumeRoot + "\\";

    // 先打开卷设备, 获取 NTFS 参数
    QString devPath = "\\\\.\\" + vol.volumeRoot;
    HANDLE hVol = CreateFileW(reinterpret_cast<LPCWSTR>(devPath.utf16()),
                              GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_EXISTING, 0, nullptr);
    if (hVol == INVALID_HANDLE_VALUE)
        return false;

    NTFS_VOLUME_DATA_BUFFER vb = {};
    DWORD bytesRet = 0;
    if (!DeviceIoControl(hVol, FSCTL_GET_NTFS_VOLUME_DATA,
                         nullptr, 0, &vb, sizeof(vb), &bytesRet, nullptr)) {
        CloseHandle(hVol);
        return false;
    }
    CloseHandle(hVol);

    vol.bytesPerCluster = vb.BytesPerCluster;
    vol.bytesPerRecord  = vb.BytesPerFileRecordSegment;
    vol.mftStartLcn     = vb.MftStartLcn.QuadPart;
    vol.totalRecords    = vb.MftValidDataLength.QuadPart / vol.bytesPerRecord;

    // 直接打开 $Mft 文件 (NTFS 驱动处理碎片, 无需手动定位)
    QString mftPath = "\\\\.\\" + vol.volumeRoot + "\\$Mft";
    vol.hVol = CreateFileW(reinterpret_cast<LPCWSTR>(mftPath.utf16()),
                           GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, 0, nullptr);
    return vol.hVol != INVALID_HANDLE_VALUE;
}

// ─── 遍历属性链查找指定类型 ───
static const AttrHeader* findAttr(const char* record, uint16_t firstOffset,
                                   uint32_t type)
{
    const AttrHeader* attr = reinterpret_cast<const AttrHeader*>(record + firstOffset);
    while (attr->type != 0xFFFFFFFF && attr->type != 0) {
        if (attr->type == type)
            return attr;
        if (attr->length == 0) break;
        attr = reinterpret_cast<const AttrHeader*>(
            reinterpret_cast<const char*>(attr) + attr->length);
    }
    return nullptr;
}

// ─── 展开 NTFS 文件名 (清理非法字符) ───
static QString cleanName(const wchar_t* name, uint8_t nameLen)
{
    QString s = QString::fromWCharArray(name, nameLen);
    // 过滤 NTFS 内部使用的字符
    s.remove(QChar(L'\\'));
    s.remove(QChar(L'/'));
    s.remove(QChar(L':'));
    return s;
}

// ─── 解析扩展名过滤器 (用于快速匹配) ───
static QSet<QString> parseExtFilters(const QStringList& filters)
{
    QSet<QString> exts;
    for (const QString& f : filters) {
        QString e = f.toLower();
        if (e.startsWith("*.")) {
            exts.insert(e.mid(2));
        } else if (e.startsWith(".")) {
            exts.insert(e.mid(1));
        } else {
            exts.insert(e); // 不含点的扩展名
        }
    }
    return exts;
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
                                           QString* errorMsg)
{
    QVector<FileEntry> results;
    if (filters.isEmpty()) return results;

    // 先尝试直接打开卷 (不需要特殊权限)
    NtfsVolume vol;
    if (!openVolume(srcPath, vol)) {
        // 直接打开失败 → 尝试获取备份权限再试
        enableBackupPrivilege();
        if (!openVolume(srcPath, vol)) {
            if (errorMsg) *errorMsg = "NTFS快速扫描不可用 (无卷访问权限), 已回退普通扫描";
            return results;
        }
    }

    if (errorMsg) *errorMsg = QString("bpr=%1 bpc=%2 mftLcn=%3 totalRec=%4")
                                  .arg(vol.bytesPerRecord).arg(vol.bytesPerCluster)
                                  .arg(vol.mftStartLcn).arg(vol.totalRecords);
    if (vol.totalRecords == 0 || vol.bytesPerRecord == 0) return results;

    // 限制扫描范围: 只处理 srcPath 下的文件
    QString scanDir = QDir::cleanPath(srcPath);
    if (!scanDir.endsWith('/') && !scanDir.endsWith('\\'))
        scanDir += '\\';
    QString scanRoot = scanDir.left(2); // "D:"
    bool wholeVolume = (scanDir.length() <= 3);

    // 解析扩展名
    QSet<QString> wantedExts = parseExtFilters(filters);
    if (wantedExts.isEmpty()) return results;

    // ─── 第一遍: 遍历所有 MFT 记录, 收集目录和文件 ───
    QHash<uint64_t, DirInfo> dirMap;    // MFT ref → DirInfo
    QVector<FileEntry> fileEntries;     // 匹配的文件

    DWORD bpr  = vol.bytesPerRecord;
    std::vector<char> mftChunk;
    mftChunk.resize(65536);

    // 直接顺序读 $Mft 文件 (NTFS 驱动已处理碎片)
    ULONGLONG mftOffset = 0;
    for (ULONGLONG i = 0; i < vol.totalRecords; ++i) {
        if (i % 64 == 0) {
            LARGE_INTEGER li; li.QuadPart = mftOffset;
            SetFilePointerEx(vol.hVol, li, nullptr, FILE_BEGIN);
            DWORD toRead = (i + 64 <= vol.totalRecords) ? 64 * bpr
                          : (vol.totalRecords - i) * bpr;
            DWORD read = 0;
            ReadFile(vol.hVol, mftChunk.data(), toRead, &read, nullptr);
            mftOffset += read;
        }

        const char* record = mftChunk.data() + (i % 64) * bpr;
        const MftRecordHeader* hdr = reinterpret_cast<const MftRecordHeader*>(record);

        // 校验魔术字
        if (memcmp(hdr->magic, "FILE", 4) != 0) continue;
        // 检查是否在用
        if (!(hdr->flags & 0x0001)) continue;

        bool isDir = (hdr->flags & 0x0002) != 0;

        // 找 $FILE_NAME 属性
        const AttrHeader* fnAttr = findAttr(record, hdr->firstAttrOffset, 0x30);
        if (!fnAttr) continue;

        const FileNameAttrBody* fnBody = reinterpret_cast<const FileNameAttrBody*>(
            record + fnAttr->bodyOffset);

        uint64_t parentRef = fnBody->parentRef & 0x0000FFFFFFFFFFFFULL;
        QString name = cleanName(fnBody->name, fnBody->nameLength);

        if (isDir) {
            // 记录目录信息用于后续路径还原
            DirInfo di;
            di.parentRef = parentRef;
            di.name = name;
            dirMap[i] = di;

            // 根目录: MFT ref 5
            if (i == 5) {
                di.name = vol.volumeRoot;
                dirMap[i] = di;
            }
            continue;
        }

        // ─── 文件: 检查扩展名 ───
        int dotPos = name.lastIndexOf('.');
        if (dotPos < 0) continue;
        QString ext = name.mid(dotPos + 1).toLower();
        if (!wantedExts.contains(ext)) continue;

        // ─── 获取文件大小 ───
        qint64 fileSize = 0;
        const AttrHeader* siAttr = findAttr(record, hdr->firstAttrOffset, 0x10);
        if (siAttr && !siAttr->nonResident) {
            // 常驻 $STANDARD_INFORMATION: 大小在 $FILE_NAME 里
            fileSize = static_cast<qint64>(fnBody->realSize);
        } else {
            // 非常驻: 用 $FILE_NAME 的大小
            fileSize = static_cast<qint64>(fnBody->realSize);
        }

        fileEntries.append(FileEntry(QString(), fileSize));
        // 暂存父引用, 路径稍后还原
        FileEntry& fe = fileEntries.last();
        fe.filePath = QString::number(parentRef) + "|" + name;
    }

    CloseHandle(vol.hVol);

    // ─── 路径还原 ───
    // 递归 + 缓存构建目录完整路径
    QHash<uint64_t, QString> pathCache;
    std::function<QString(uint64_t)> resolvePath = [&](uint64_t ref) -> QString {
        if (pathCache.contains(ref)) return pathCache[ref];
        if (ref == 5) {
            pathCache[ref] = vol.rootPath;
            return vol.rootPath;
        }
        if (!dirMap.contains(ref)) {
            pathCache[ref] = vol.rootPath + "(unknown)";
            return pathCache[ref];
        }
        const DirInfo& di = dirMap[ref];
        QString parent = resolvePath(di.parentRef);
        pathCache[ref] = parent + di.name + "\\";
        return pathCache[ref];
    };

    // 还原每个文件的完整路径, 并筛选在扫描范围内的
    QVector<FileEntry> filtered;
    filtered.reserve(fileEntries.size());
    for (auto& fe : fileEntries) {
        // 从临时格式 "parentRef|name" 中拆分
        int sep = fe.filePath.indexOf('|');
        uint64_t parentRef = fe.filePath.left(sep).toULongLong();
        QString name = fe.filePath.mid(sep + 1);

        QString dirPath = resolvePath(parentRef);
        QString fullPath = dirPath + name;

        // 检查是否在扫描目录范围内
        if (!wholeVolume) {
            if (!fullPath.startsWith(scanDir, Qt::CaseInsensitive))
                continue;
        }

        fe.filePath = fullPath;
        filtered.append(fe);
    }

    return filtered;
}
