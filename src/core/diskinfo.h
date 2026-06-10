#ifndef DISKINFO_H
#define DISKINFO_H

#include <QString>
#include <QVector>

// ─── 磁盘类型 ───
enum class DiskType {
    Unknown   = 0,
    HDD       = 1,  // 机械硬盘 — 建议串行复制
    SSD       = 2,  // 固态硬盘 — 可少量并行
    NVMe      = 3,  // NVMe 高速盘 — 可更多并行
    Removable = 4,  // U盘/SD卡 — 串行（总线瓶颈）
    Network   = 5,  // 网络驱动器 — 单线程（带宽瓶颈）
    Optical   = 6,  // 光驱 — 跳过
};

// ─── 磁盘信息 ───
struct DiskInfo {
    QString  mountPoint;   // "D:"
    QString  volumeName;   // "My Passport"
    QString  rootPath;     // "D:\\"
    DiskType type    = DiskType::Unknown;
    qint64   totalBytes = 0;
    qint64   freeBytes  = 0;
    bool     isSystemDisk = false;

    // 建议的并行复制线程数
    int recommendedConcurrency() const {
        switch (type) {
        case DiskType::NVMe:      return 3;
        case DiskType::SSD:       return 2;
        case DiskType::HDD:       return 1;
        case DiskType::Removable: return 1;
        case DiskType::Network:   return 1;
        default:                  return 1;
        }
    }

    QString typeLabel() const {
        switch (type) {
        case DiskType::HDD:       return "HDD 机械盘";
        case DiskType::SSD:       return "SSD 固态盘";
        case DiskType::NVMe:      return "NVMe 高速盘";
        case DiskType::Removable: return "U盘/移动设备";
        case DiskType::Network:   return "网络驱动器";
        case DiskType::Optical:   return "光驱";
        default:                  return "未知类型";
        }
    }

    // 是否适合作为复制目标
    bool isValidTarget() const {
        return type != DiskType::Unknown &&
               type != DiskType::Optical &&
               !mountPoint.isEmpty();
    }
};

// ─── 磁盘检测工具 ───
namespace DiskDetector {

    // 检测单个路径对应的磁盘信息
    DiskInfo detectForPath(const QString& path);

    // 扫描所有可用磁盘 (过滤掉光驱/系统保留等)
    QVector<DiskInfo> allAvailableDisks();

    // 对一组源路径进行智能分组 (同物理盘 vs 异物理盘)
    // 返回: { 物理磁盘标识 → 属于该盘的路径列表 }
    QMap<QString, QStringList> groupByPhysicalDisk(const QStringList& paths);

} // namespace DiskDetector

#endif // DISKINFO_H
