#include "diskinfo.h"
#include <windows.h>
#include <winioctl.h>
#include <QDir>
#include <QFileInfo>
#include <QtDebug>

// MinGW 头文件可能缺少以下定义 (Windows 7+ 的结构体)
#ifndef StorageDeviceSeekPenaltyProperty
#define StorageDeviceSeekPenaltyProperty 7
#endif

#ifndef DEVICE_SEEK_PENALTY_DESCRIPTOR
typedef struct _DEVICE_SEEK_PENALTY_DESCRIPTOR {
    DWORD   Version;
    DWORD   Size;
    BOOLEAN IncursSeekPenalty;
} DEVICE_SEEK_PENALTY_DESCRIPTOR;
#endif

#ifndef StorageAdapterProperty
#define StorageAdapterProperty 8
#endif

// BusTypeNvme = 17 (ntddstor.h)
#ifndef BusTypeNvme
#define BusTypeNvme 17
#endif

namespace DiskDetector {

// ─── 判断 HDD vs SSD: 查询 StorageDeviceSeekPenaltyProperty ───
// TRUE = HDD (有寻道延迟), FALSE = SSD (无寻道延迟)
static bool hasSeekPenalty(const QString& rootPath)
{
    // 打开物理驱动器需要形如 "\\.\C:" 的路径
    QString devicePath = "\\\\.\\" + rootPath;
    devicePath.chop(1); // 去掉末尾的 '\'

    HANDLE hDevice = CreateFileW(
        reinterpret_cast<LPCWSTR>(devicePath.utf16()),
        0,                          // 不需要读写权限
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        0,
        nullptr);

    if (hDevice == INVALID_HANDLE_VALUE) {
        return true; // 打不开 → 保守假设为 HDD
    }

    // 构建查询
    STORAGE_PROPERTY_QUERY query = {};
    query.PropertyId = static_cast<STORAGE_PROPERTY_ID>(StorageDeviceSeekPenaltyProperty);
    query.QueryType  = PropertyStandardQuery;

    DEVICE_SEEK_PENALTY_DESCRIPTOR result = {};
    DWORD bytesReturned = 0;

    BOOL ok = DeviceIoControl(
        hDevice,
        IOCTL_STORAGE_QUERY_PROPERTY,
        &query, sizeof(query),
        &result, sizeof(result),
        &bytesReturned,
        nullptr);

    CloseHandle(hDevice);

    if (!ok) {
        return true; // 查询失败 → 保守假设为 HDD
    }

    // IncursSeekPenalty == TRUE → HDD (有寻道惩罚)
    return result.IncursSeekPenalty != FALSE;
}

// ─── 尝试通过总线类型区分 NVMe ───
static bool isNvmeBus(const QString& rootPath)
{
    QString devicePath = "\\\\.\\" + rootPath;
    devicePath.chop(1);

    HANDLE hDevice = CreateFileW(
        reinterpret_cast<LPCWSTR>(devicePath.utf16()),
        0,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, 0, nullptr);

    if (hDevice == INVALID_HANDLE_VALUE)
        return false;

    STORAGE_PROPERTY_QUERY query = {};
    query.PropertyId = static_cast<STORAGE_PROPERTY_ID>(StorageAdapterProperty);
    query.QueryType  = PropertyStandardQuery;

    STORAGE_ADAPTER_DESCRIPTOR adapter = {};
    DWORD bytesReturned = 0;

    BOOL ok = DeviceIoControl(
        hDevice,
        IOCTL_STORAGE_QUERY_PROPERTY,
        &query, sizeof(query),
        &adapter, sizeof(adapter),
        &bytesReturned, nullptr);

    CloseHandle(hDevice);

    if (!ok) return false;

    // BusTypeNvme == 17 (defined in ntddstor.h, may not be in MinGW headers)
    return adapter.BusType == 17;
}

// ─── 检测单个路径对应的磁盘 ───
DiskInfo detectForPath(const QString& path)
{
    DiskInfo info;

    // 获取盘符根路径
    QString root = path;
    if (root.length() >= 2 && root.at(1) == ':') {
        root = root.left(2) + "\\";
    } else if (root.startsWith("\\\\")) {
        // UNC 网络路径
        info.mountPoint = path;
        info.rootPath   = path;
        info.type       = DiskType::Network;
        info.volumeName = "网络位置";
        return info;
    } else {
        return info; // 无效路径
    }

    info.mountPoint = root.left(2); // "D:"
    info.rootPath   = root;

    // ── 1. 基本类型判断 ──
    UINT driveType = GetDriveTypeW(reinterpret_cast<LPCWSTR>(root.utf16()));
    switch (driveType) {
    case DRIVE_FIXED:     info.type = DiskType::HDD; break; // 后续细分
    case DRIVE_REMOVABLE: info.type = DiskType::Removable; break;
    case DRIVE_REMOTE:    info.type = DiskType::Network; break;
    case DRIVE_CDROM:     info.type = DiskType::Optical; break;
    default:              info.type = DiskType::Unknown; break;
    }

    // ── 2. 对固定磁盘进一步区分 HDD/SSD/NVMe ──
    if (driveType == DRIVE_FIXED) {
        if (isNvmeBus(root))
            info.type = DiskType::NVMe;
        else if (!hasSeekPenalty(root))
            info.type = DiskType::SSD;
        else
            info.type = DiskType::HDD;
    }

    // ── 3. 卷标 ──
    wchar_t volName[MAX_PATH + 1] = {};
    GetVolumeInformationW(
        reinterpret_cast<LPCWSTR>(root.utf16()),
        volName, MAX_PATH + 1,
        nullptr, nullptr, nullptr, nullptr, 0);
    info.volumeName = QString::fromWCharArray(volName);
    if (info.volumeName.isEmpty()) {
        info.volumeName = "本地磁盘";
    }

    // ── 4. 容量 ──
    ULARGE_INTEGER freeBytesAvail, totalBytes, totalFreeBytes;
    if (GetDiskFreeSpaceExW(
            reinterpret_cast<LPCWSTR>(root.utf16()),
            &freeBytesAvail, &totalBytes, &totalFreeBytes)) {
        info.totalBytes = static_cast<qint64>(totalBytes.QuadPart);
        info.freeBytes  = static_cast<qint64>(freeBytesAvail.QuadPart);
    }

    // ── 5. 是否系统盘 ──
    wchar_t sysDir[MAX_PATH];
    if (GetSystemDirectoryW(sysDir, MAX_PATH) > 0) {
        info.isSystemDisk = (sysDir[0] == root[0].unicode());
    }

    return info;
}

// ─── 扫描所有可用磁盘 ───
QVector<DiskInfo> allAvailableDisks()
{
    QVector<DiskInfo> result;
    DWORD drives = GetLogicalDrives();

    for (int i = 0; i < 26; ++i) {
        if (!(drives & (1 << i))) continue;

        char letter = 'A' + i;
        QString root = QString("%1:\\").arg(QLatin1Char(letter));

        DiskInfo info = detectForPath(root);

        // 跳过光驱和无效设备
        if (info.type == DiskType::Optical || info.type == DiskType::Unknown)
            continue;

        result.append(info);
    }

    return result;
}

// ─── 按物理磁盘分组 ───
// 通过比较 DeviceIoControl(IOCTL_STORAGE_GET_DEVICE_NUMBER)
// 获取每个分区的物理磁盘编号
static int getPhysicalDiskNumber(const QString& rootPath)
{
    QString devicePath = "\\\\.\\" + rootPath;
    devicePath.chop(1);

    HANDLE hDevice = CreateFileW(
        reinterpret_cast<LPCWSTR>(devicePath.utf16()),
        0, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, 0, nullptr);

    if (hDevice == INVALID_HANDLE_VALUE)
        return -1;

    STORAGE_DEVICE_NUMBER devNum = {};
    DWORD bytesReturned = 0;

    BOOL ok = DeviceIoControl(
        hDevice,
        IOCTL_STORAGE_GET_DEVICE_NUMBER,
        nullptr, 0,
        &devNum, sizeof(devNum),
        &bytesReturned, nullptr);

    CloseHandle(hDevice);

    if (!ok) return -1;
    return devNum.DeviceNumber;
}

QMap<QString, QStringList> groupByPhysicalDisk(const QStringList& paths)
{
    QMap<QString, QStringList> groups;

    for (const QString& path : paths) {
        QString root = path;
        if (root.length() >= 2 && root.at(1) == ':')
            root = root.left(2) + "\\";
        else
            continue;

        int diskNum = getPhysicalDiskNumber(root);
        QString key = QString("PhysicalDisk%1").arg(diskNum);

        groups[key].append(path);
    }

    return groups;
}

} // namespace DiskDetector
