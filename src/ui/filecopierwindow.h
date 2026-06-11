#ifndef FILECOPIERWINDOW_H
#define FILECOPIERWINDOW_H

#include <QWidget>
#include <QListWidget>
#include <QLineEdit>
#include <QPushButton>
#include <QLabel>
#include <QCheckBox>
#include <QComboBox>
#include <QTableView>
#include <QTextEdit>
#include <QProgressBar>
#include <QElapsedTimer>
#include <QVBoxLayout>
#include <QMap>
#include "mediahelper.h"
#include "metafilterproxymodel.h"

class DynamicThreadPool;
class FileListModel;
class FileScanner;
class FileCopierEngine;
struct FileEntry;

class FileCopierWindow : public QWidget
{
    Q_OBJECT

public:
    explicit FileCopierWindow(QWidget* parent = nullptr);
    ~FileCopierWindow() override;

protected:
    void closeEvent(QCloseEvent* event) override;

private slots:
    // 源路径
    void onAddSourcePath();
    void onAutoDetectSources();
    void onSourceItemChanged(QListWidgetItem* item);

    // 扫描
    void onScanStart();
    void onCancelScan();
    void onBatchFound(const QVector<FileEntry>& batch);
    void onScanProgress(int filesFound);
    void onScanFinished(int totalFiles, qint64 totalSize);
    void onMetadataBatch(const QVector<MediaMetadata>& batch);

    // Checkbox 批处理
    void onSelectAll();

    // 目标路径
    void onAddDestPath();
    void onAutoDetectDestinations();

    // 复制
    void onCopyStart();
    void onCancelCopy();
    void onCopyProgress(int percent, int copied, int failed, int total,
                        qint64 bytesCopied, const QString& destPath);
    void onCopyFinished(int total, int success, int failed, qint64 totalBytes,
                        const QString& destPath);
    void onFileError(const QString& path, const QString& error);

    // 筛选
    void onTypeFilterChanged(int index);
    void onMetaFilterApply();
    void onMetaFilterClear();
    void onFilterPresetLoad(int index);
    void onFilterPresetSave();
    void onFilterPresetDelete();

    // 方案管理
    void onSchemeLoad(int index);
    void onSchemeSave();
    void onSchemeDelete();

    // 表格
    void onTableClicked(const QModelIndex& index);
    void onTableDoubleClicked(const QModelIndex& index);
    void onTableContextMenu(const QPoint& pos);

    // 导出 / 服务
    void onExport();
    void onManageService();
    void appendLog(const QString& msg);

private:
    void setupUI();
    void setupSourceSection(QVBoxLayout* mainLayout);
    void setupFileTable(QVBoxLayout* mainLayout);
    void setupMetaFilterPanel(QVBoxLayout* mainLayout);
    void setupDestinationSection(QVBoxLayout* mainLayout);
    void setupLogSection(QVBoxLayout* mainLayout);

    void loadSettings();
    void saveSettings();
    void loadAllSettings();
    void saveAllSettings();
    void loadScheme(const QString& name);
    void saveScheme(const QString& name);
    void refreshSchemeCombo();
    void refreshPresetCombo();
    QString currentScheme() const;

    void openFileLocation(const QString& filePath);
    void updateCopyButtonState();
    QString formatSize(qint64 bytes) const;
    QStringList getCheckedSourcePaths() const;
    QStringList getDestinationPaths() const;
    void startMetadataExtraction();
    void updateSummaryLabel(int fileCount, qint64 totalSize);

    // ─── 核心 ───
    DynamicThreadPool*      m_pool      = nullptr;
    FileListModel*          m_model     = nullptr;
    MetaFilterProxyModel*   m_proxy     = nullptr;
    FileScanner*            m_scanner   = nullptr;
    QVector<FileCopierEngine*> m_copiers;

    // ─── UI: 源 ───
    QListWidget*    m_sourceList    = nullptr;
    QPushButton*    m_addSrcBtn     = nullptr;
    QPushButton*    m_autoSrcBtn    = nullptr;
    QPushButton*    m_scanBtn       = nullptr;
    QPushButton*    m_cancelScanBtn = nullptr;
    QCheckBox*      m_extractMetaCb = nullptr;
    QPushButton*    m_exportBtn     = nullptr;
    QProgressBar*   m_scanProgress  = nullptr;
    QLabel*         m_scanStatus    = nullptr;

    // ─── UI: 表格 + 批处理 ───
    QTableView*     m_fileView      = nullptr;
    QLabel*         m_summaryLabel  = nullptr;
    QPushButton*    m_selectAllBtn  = nullptr;
    QLabel*         m_checkedLabel  = nullptr;
    QLabel*         m_selectedLabel = nullptr;
    QComboBox*      m_typeFilterCombo = nullptr;

    // ─── UI: 高级筛选面板 ───
    QWidget*        m_metaPanel     = nullptr;
    QComboBox*      m_cmbRes        = nullptr;
    QComboBox*      m_cmbDur        = nullptr;
    QComboBox*      m_cmbCodec      = nullptr;
    QComboBox*      m_cmbPreset     = nullptr;

    // ─── UI: 方案 ───
    QComboBox*      m_cmbScheme     = nullptr;

    // ─── UI: 目标 ───
    QListWidget*    m_destList      = nullptr;
    QPushButton*    m_addDestBtn    = nullptr;
    QPushButton*    m_autoDestBtn   = nullptr;
    QCheckBox*      m_skipExistingCb = nullptr;
    QCheckBox*      m_preserveTsCb  = nullptr;
    QPushButton*    m_copyBtn       = nullptr;
    QPushButton*    m_cancelCopyBtn = nullptr;
    QProgressBar*   m_copyProgress  = nullptr;
    QLabel*         m_copyTimeLabel = nullptr;
    QLabel*         m_copyStatus    = nullptr;

    // ─── UI: 日志 ───
    QTextEdit*      m_logEdit       = nullptr;

    // ─── 状态 ───
    bool m_scanning       = false;
    bool m_extractingMeta = false;
    int  m_activeCopies = 0;
    int  m_totalCopies  = 0;
    int  m_totalSuccess = 0;
    int  m_totalFailed  = 0;
    QElapsedTimer m_copyTimer;

    QStringList           m_snapshotFiles;
    QMap<QString, qint64> m_snapshotSizeMap;
};

#endif // FILECOPIERWINDOW_H
