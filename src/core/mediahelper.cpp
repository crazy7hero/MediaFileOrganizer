#include "mediahelper.h"
#include "MediaInfoDLL.h"
#include <QFileInfo>

using namespace MediaInfoDLL;

// ─── 单个文件提取 (ParseSpeed=0 仅读头, 不扫全文件) ───
// 返回的 MediaMetadata.isValid=false 表示无法解析
static MediaMetadata extractOne(const QString& path)
{
    MediaMetadata m;
    m.filePath = path;
    m.fileSize = QFileInfo(path).size();

    // 跳过 >2GB 的大文件 (通常未压缩视频, MediaInfo 解析极慢甚至卡死)
    if (m.fileSize > 2LL * 1024 * 1024 * 1024)
        return m;

    // 跳过零字节文件
    if (m.fileSize == 0)
        return m;

    MediaInfo MI;
    MI.Option(__T("ParseSpeed"), __T("0"));
    MI.Option(__T("ReadByHuman"), __T("0"));

    std::wstring wpath = path.toStdWString();
    if (MI.Open(wpath) == 0)
        return m;

    int videoCount = MI.Count_Get(Stream_Video);
    int audioCount = MI.Count_Get(Stream_Audio);
    int imageCount = MI.Count_Get(Stream_Image);

    m.isVideo = (videoCount > 0);
    m.isAudio = (audioCount > 0 && videoCount == 0);
    m.isImage = (imageCount > 0 && videoCount == 0);

    if (videoCount > 0) {
        String w = MI.Get(Stream_Video, 0, __T("Width"),  Info_Text, Info_Name);
        String h = MI.Get(Stream_Video, 0, __T("Height"), Info_Text, Info_Name);
        if (!w.empty()) m.width  = std::stoi(w);
        if (!h.empty()) m.height = std::stoi(h);
        String dur = MI.Get(Stream_Video, 0, __T("Duration"), Info_Text, Info_Name);
        if (!dur.empty()) m.durationMs = std::stoll(dur);
        String cdc = MI.Get(Stream_Video, 0, __T("CodecID/Hint"), Info_Text, Info_Name);
        if (cdc.empty()) cdc = MI.Get(Stream_Video, 0, __T("Format"), Info_Text, Info_Name);
        m.codec = QString::fromStdWString(cdc);
        String fps = MI.Get(Stream_Video, 0, __T("FrameRate"), Info_Text, Info_Name);
        if (!fps.empty()) m.framerate = std::stod(fps);
        m.isValid = true;
    }

    if (imageCount > 0 && videoCount == 0) {
        String w = MI.Get(Stream_Image, 0, __T("Width"),  Info_Text, Info_Name);
        String h = MI.Get(Stream_Image, 0, __T("Height"), Info_Text, Info_Name);
        if (!w.empty()) m.width  = std::stoi(w);
        if (!h.empty()) m.height = std::stoi(h);
        String fmt = MI.Get(Stream_Image, 0, __T("Format"), Info_Text, Info_Name);
        m.codec = QString::fromStdWString(fmt);
        m.isValid = true;
    }

    if (audioCount > 0) {
        String ac = MI.Get(Stream_Audio, 0, __T("CodecID/Hint"), Info_Text, Info_Name);
        if (ac.empty()) ac = MI.Get(Stream_Audio, 0, __T("Format"), Info_Text, Info_Name);
        m.audioCodec = QString::fromStdWString(ac);
        String ch = MI.Get(Stream_Audio, 0, __T("Channel(s)"), Info_Text, Info_Name);
        if (!ch.empty()) m.audioChannels = std::stoi(ch);
        String sr = MI.Get(Stream_Audio, 0, __T("SamplingRate"), Info_Text, Info_Name);
        if (!sr.empty()) m.audioSampleRate = std::stoi(sr);
        if (m.isAudio && !m.isValid) {
            String dur = MI.Get(Stream_Audio, 0, __T("Duration"), Info_Text, Info_Name);
            if (!dur.empty()) m.durationMs = std::stoll(dur);
            if (m.codec.isEmpty()) m.codec = m.audioCodec;
            m.isValid = true;
        }
    }

    MI.Close();
    return m;
}

// ─── 批量提取 ───
// 每个文件必定返回结果 (成功或失败), 不采用超时/异步
QVector<MediaMetadata> extractMetadata(const QStringList& filePaths)
{
    QVector<MediaMetadata> results;
    results.reserve(filePaths.size());
    for (const QString& path : filePaths) {
        results.append(extractOne(path));
    }
    return results;
}
