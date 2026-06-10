#ifndef MEDIAHELPER_H
#define MEDIAHELPER_H

#include <QString>
#include <QVector>

// ─── 提取的媒体元数据 ───
struct MediaMetadata {
    QString filePath;
    bool    isValid   = false;

    // 通用
    bool    isVideo   = false;
    bool    isImage   = false;
    bool    isAudio   = false;

    // 视频 / 图像
    int     width     = 0;
    int     height    = 0;

    // 视频 / 音频
    qint64  durationMs = 0;    // 毫秒
    QString codec;             // "H.264", "HEVC", "AAC"...
    double  framerate = 0.0;

    // 音频
    QString audioCodec;
    int     audioChannels   = 0;
    int     audioSampleRate = 0;

    qint64  fileSize    = 0;   // 字节 (冗余, 方便比对)
};

// ─── 批量元数据提取器 ───
// 从文件路径列表中提取元数据, 在工作线程中调用
QVector<MediaMetadata> extractMetadata(const QStringList& filePaths);

#endif // MEDIAHELPER_H
