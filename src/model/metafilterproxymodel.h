#ifndef METAFILTERPROXYMODEL_H
#define METAFILTERPROXYMODEL_H

#include <QSortFilterProxyModel>
#include <QString>
#include "filelistmodel.h"

// ─── 筛选条件 ───
struct FilterCriteria {
    int     minWidth     = 0;
    int     minHeight    = 0;
    qint64  minDurationMs = 0;
    QString codec;

    bool matchAll = true;  // true=全部满足, false=任一满足

    bool isEmpty() const {
        return minWidth == 0 && minHeight == 0 &&
               minDurationMs == 0 && codec.isEmpty();
    }
};

// ─── 元数据筛选代理模型 ───
// 在 FileListModel 之上按分辨率/时长/编码过滤
class MetaFilterProxyModel : public QSortFilterProxyModel
{
    Q_OBJECT

public:
    explicit MetaFilterProxyModel(QObject* parent = nullptr)
        : QSortFilterProxyModel(parent) {}

    void setCriteria(const FilterCriteria& c) {
        m_criteria = c;
        invalidateFilter();
    }
    void clearCriteria() {
        m_criteria = FilterCriteria();
        invalidateFilter();
    }
    FilterCriteria criteria() const { return m_criteria; }

protected:
    bool filterAcceptsRow(int row, const QModelIndex& parent) const override {
        if (m_criteria.isEmpty()) return true;
        if (parent.isValid()) return true;

        auto* model = qobject_cast<FileListModel*>(sourceModel());
        if (!model) return true;

        QString path = model->filePathAt(row);
        // 元数据未提取或无法解析 → 不满足筛选条件 → 隐藏
        if (!model->hasMetadata(path)) return false;

        MediaMetadata md = model->metadataFor(path);
        if (!md.isValid) return false;

        bool resOk = true, durOk = true, codecOk = true;

        if (m_criteria.minWidth > 0 || m_criteria.minHeight > 0)
            resOk = (md.width >= m_criteria.minWidth &&
                     md.height >= m_criteria.minHeight);

        if (m_criteria.minDurationMs > 0)
            durOk = (md.durationMs >= m_criteria.minDurationMs);

        if (!m_criteria.codec.isEmpty())
            codecOk = (md.codec.compare(m_criteria.codec, Qt::CaseInsensitive) == 0);

        if (m_criteria.matchAll)
            return resOk && durOk && codecOk;
        else
            return resOk || durOk || codecOk;
    }

private:
    FilterCriteria m_criteria;
};

#endif // METAFILTERPROXYMODEL_H
