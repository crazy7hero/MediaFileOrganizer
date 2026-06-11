#include "filecopierwindow.h"
#include "dynamicthreadpool.h"
#include "filelistmodel.h"
#include "filescanner.h"
#include "filecopierengine.h"
#include "diskinfo.h"
#include "mediahelper.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QFileDialog>
#include <QMessageBox>
#include <QProcess>
#include <QFileInfo>
#include <QDir>
#include <QDateTime>
#include <QTextStream>
#include <QApplication>
#include <QFont>
#include <QTimer>
#include <QRegularExpression>
#include <QLocale>
#include <QTextCursor>
#include <QShortcut>
#include <QMenu>
#include <QClipboard>
#include <QSettings>
#include <QInputDialog>
#include <QCloseEvent>
#include <QStyledItemDelegate>
#include <QPainter>
#include <thread>

// checkbox 列代理: 不受选中高亮影响, 保持原生勾子颜色
class CheckBoxDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    void paint(QPainter* p, const QStyleOptionViewItem& opt, const QModelIndex& idx) const override {
        QStyleOptionViewItem o = opt;
        o.state &= ~QStyle::State_Selected; // 去掉选中态
        QStyledItemDelegate::paint(p, o, idx);
    }
};

// ═══════════════════════════════════════════════════════════════
FileCopierWindow::FileCopierWindow(QWidget* parent)
    : QWidget(parent)
{
    m_pool    = new DynamicThreadPool(4, 8, 5000, 1000);
    m_model   = new FileListModel(this);
    m_scanner = new FileScanner(this);
    m_proxy   = new MetaFilterProxyModel(this);
    m_proxy->setSourceModel(m_model);
    m_proxy->setSortRole(Qt::UserRole);

    m_scanner->setFilters({
        "*.mp4","*.avi","*.mkv","*.mov","*.wmv","*.flv","*.webm",
        "*.mp3","*.wav","*.flac","*.aac","*.ogg",
        "*.jpg","*.jpeg","*.png","*.gif","*.bmp","*.tiff","*.webp","*.heic"
    });

    setupUI();
    loadSettings();

    setWindowTitle("媒体文件整理工具");
    setMinimumSize(900, 620);
    resize(1050, 720);

    // 扫描器
    connect(m_scanner, &FileScanner::batchFound,
            this, &FileCopierWindow::onBatchFound);
    connect(m_scanner, &FileScanner::progressUpdated,
            this, &FileCopierWindow::onScanProgress);
    connect(m_scanner, &FileScanner::scanFinished,
            this, &FileCopierWindow::onScanFinished);

    // 文件统计
    connect(m_model, &FileListModel::countChanged, this, [this](int count, qint64 size) {
        updateSummaryLabel(count, size);
        m_exportBtn->setEnabled(count > 0);
        updateCopyButtonState();
    });

    // 勾选统计
    connect(m_model, &FileListModel::checkedCountChanged, this, [this](int n) {
        m_checkedLabel->setText(n > 0 ? QString("已勾选 %1 个").arg(QLocale().toString(n)) : "");
        updateCopyButtonState();
    });

    // 表格选中变化 → 更新位置提示
    connect(m_fileView->selectionModel(), &QItemSelectionModel::selectionChanged,
            this, [this]() {
        updateCopyButtonState();
        QModelIndexList sel = m_fileView->selectionModel()->selectedRows();
        if (sel.isEmpty()) {
            m_selectedLabel->setText("");
        } else {
            int row = sel.first().row() + 1; // 用户看到的行号从 1 开始
            m_selectedLabel->setText(QString("第 %1 条").arg(QLocale().toString(row)));
        }
    });

    appendLog("就绪");
}

FileCopierWindow::~FileCopierWindow()
{
    if (m_scanner) m_scanner->cancel();
    for (auto* c : m_copiers) { if (c) c->cancel(); }
    if (m_pool) { m_pool->shutdown(); delete m_pool; m_pool = nullptr; }
    m_copiers.clear();
}

void FileCopierWindow::closeEvent(QCloseEvent* event)
{
    saveSettings();
    event->accept();
}

// ═══════════════════════════════════════════════════════════════
// 布局
// ═══════════════════════════════════════════════════════════════

void FileCopierWindow::setupUI()
{
    QVBoxLayout* main = new QVBoxLayout(this);
    main->setSpacing(8);
    main->setContentsMargins(14, 10, 14, 10);

    QHBoxLayout* titleRow = new QHBoxLayout();
    QLabel* title = new QLabel("📁 媒体文件整理", this);
    QFont f = title->font(); f.setPointSize(13); f.setBold(true);
    title->setFont(f);
    titleRow->addWidget(title);
    titleRow->addStretch();

    m_cmbScheme = new QComboBox(this);
    m_cmbScheme->setFixedWidth(140);
    m_cmbScheme->setStyleSheet("QComboBox{font-size:11px;}");
    m_cmbScheme->setToolTip("方案: 一键切换源/目标/筛选配置");
    connect(m_cmbScheme, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &FileCopierWindow::onSchemeLoad);
    titleRow->addWidget(m_cmbScheme);

    QPushButton* btnSaveScheme = new QPushButton("💾", this);
    btnSaveScheme->setFixedSize(26, 26);
    btnSaveScheme->setToolTip("保存当前方案");
    btnSaveScheme->setStyleSheet("QPushButton{border:none;font-size:14px;}");
    connect(btnSaveScheme, &QPushButton::clicked, this, &FileCopierWindow::onSchemeSave);
    titleRow->addWidget(btnSaveScheme);

    m_exportBtn = new QPushButton("📋 导出清单", this);
    m_exportBtn->setEnabled(false);
    m_exportBtn->setFixedHeight(28);
    m_exportBtn->setStyleSheet("QPushButton{background:#FF9800;color:white;font-weight:bold;"
                                "padding:4px 14px;border-radius:3px;font-size:12px;}"
                                "QPushButton:hover{background:#F57C00;}"
                                "QPushButton:disabled{background:#ccc;}");
    connect(m_exportBtn, &QPushButton::clicked, this, &FileCopierWindow::onExport);
    titleRow->addWidget(m_exportBtn);
    main->addLayout(titleRow);

    setupSourceSection(main);
    setupFileTable(main);
    setupMetaFilterPanel(main);
    setupDestinationSection(main);
    setupLogSection(main);
}

// ═══════════════════════════════════════════════════════════════
// 源目录区
// ═══════════════════════════════════════════════════════════════

void FileCopierWindow::setupSourceSection(QVBoxLayout* main)
{
    QLabel* label = new QLabel("源目录", this);
    QFont f = label->font(); f.setBold(true); f.setPointSize(11);
    label->setFont(f);
    main->addWidget(label);

    m_sourceList = new QListWidget(this);
    m_sourceList->setMaximumHeight(105);
    m_sourceList->setAlternatingRowColors(true);
    m_sourceList->setStyleSheet(
        "QListWidget{border:1px solid #d0d0d0;border-radius:4px;font-size:12px;}"
        "QListWidget::item{padding:3px 6px;}"
        "QListWidget::item:selected{background:#1565C0;}"
        "QListWidget::item:selected:!active{background:#1565C0;}"
        "QListWidget::item:alternate{background:#fafafa;}"
        "QListWidget::item:selected:alternate{background:#1565C0;}");
    m_sourceList->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_sourceList, &QListWidget::customContextMenuRequested, this,
            [this](const QPoint& pos) {
        QListWidgetItem* item = m_sourceList->itemAt(pos);
        if (!item) return;
        QMenu menu;
        QAction* aDel  = menu.addAction("移除");
        QAction* aOpen = menu.addAction("打开目录");
        QAction* c = menu.exec(m_sourceList->viewport()->mapToGlobal(pos));
        if (c == aDel)  delete m_sourceList->takeItem(m_sourceList->row(item));
        if (c == aOpen) openFileLocation(item->data(Qt::UserRole).toString());
    });
    connect(m_sourceList, &QListWidget::itemChanged,
            this, &FileCopierWindow::onSourceItemChanged);
    QShortcut* delSrc = new QShortcut(QKeySequence::Delete, m_sourceList);
    delSrc->setContext(Qt::WidgetShortcut);
    connect(delSrc, &QShortcut::activated, this, [this]() {
        for (auto* item : m_sourceList->selectedItems())
            delete m_sourceList->takeItem(m_sourceList->row(item));
    });
    main->addWidget(m_sourceList);

    // 按钮行
    QHBoxLayout* row = new QHBoxLayout(); row->setSpacing(6);
    m_addSrcBtn = new QPushButton("＋ 添加", this);
    m_addSrcBtn->setFixedHeight(28);
    m_autoSrcBtn = new QPushButton("🖥 检测设备", this);
    m_autoSrcBtn->setFixedHeight(28);
    m_autoSrcBtn->setStyleSheet("QPushButton{color:#555;font-size:11px;}");
    QPushButton* toggleFilter = new QPushButton("⚙ 筛选", this);
    toggleFilter->setFixedHeight(28); toggleFilter->setCheckable(true);
    toggleFilter->setStyleSheet("QPushButton{color:#555;font-size:11px;}");
    row->addWidget(m_addSrcBtn); row->addWidget(m_autoSrcBtn);
    row->addWidget(toggleFilter); row->addStretch();
    m_scanBtn = new QPushButton("🔍 扫描", this);
    m_scanBtn->setFixedHeight(30);
    m_scanBtn->setStyleSheet("QPushButton{background:#2196F3;color:white;font-weight:bold;"
                              "padding:4px 28px;border-radius:4px;font-size:13px;}"
                              "QPushButton:hover{background:#1976D2;}QPushButton:disabled{background:#ccc;}");
    m_cancelScanBtn = new QPushButton("取消", this);
    m_cancelScanBtn->setFixedHeight(30); m_cancelScanBtn->setVisible(false);
    m_cancelScanBtn->setStyleSheet("QPushButton{background:#f44336;color:white;font-weight:bold;"
                                    "padding:4px 16px;border-radius:4px;}QPushButton:hover{background:#d32f2f;}");
    row->addWidget(m_scanBtn); row->addWidget(m_cancelScanBtn);
    main->addLayout(row);

    // 筛选栏
    QWidget* filterBar = new QWidget(this);
    QHBoxLayout* fl = new QHBoxLayout(filterBar);
    fl->setContentsMargins(0, 2, 0, 2);
    fl->addWidget(new QLabel("文件类型:", this));
    QLineEdit* filterEdit = new QLineEdit(this);
    filterEdit->setText("*.mp4 *.avi *.mkv *.mov *.wmv *.flv *.webm "
                         "*.mp3 *.wav *.flac *.aac *.ogg "
                         "*.jpg *.jpeg *.png *.gif *.bmp *.tiff *.webp *.heic");
    filterEdit->setStyleSheet("font-size:11px;padding:2px 4px;");
    filterEdit->setToolTip("空格分隔，修改后下次扫描生效");
    connect(filterEdit, &QLineEdit::textChanged, this, [this](const QString& t) {
        m_scanner->setFilters(t.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts));
    });
    connect(filterEdit, &QLineEdit::textChanged, this, [this]() { saveSettings(); });
    fl->addWidget(filterEdit, 1);
    filterBar->setVisible(false);
    main->addWidget(filterBar);
    connect(toggleFilter, &QPushButton::toggled, filterBar, &QWidget::setVisible);

    // 进度
    QHBoxLayout* progRow = new QHBoxLayout();
    m_scanProgress = new QProgressBar(this);
    m_scanProgress->setRange(0, 0); m_scanProgress->setVisible(false);
    m_scanProgress->setMaximumHeight(14);
    m_scanStatus = new QLabel("", this);
    m_scanStatus->setStyleSheet("color:#666;font-size:11px;");
    progRow->addWidget(m_scanProgress, 1);
    progRow->addWidget(m_scanStatus);
    main->addLayout(progRow);

    connect(m_addSrcBtn, &QPushButton::clicked, this, &FileCopierWindow::onAddSourcePath);
    connect(m_autoSrcBtn, &QPushButton::clicked, this, &FileCopierWindow::onAutoDetectSources);
    connect(m_scanBtn, &QPushButton::clicked, this, &FileCopierWindow::onScanStart);
    connect(m_cancelScanBtn, &QPushButton::clicked, this, &FileCopierWindow::onCancelScan);
}

// ═══════════════════════════════════════════════════════════════
// 文件表格 + 批处理按钮
// ═══════════════════════════════════════════════════════════════

void FileCopierWindow::setupFileTable(QVBoxLayout* main)
{
    m_fileView = new QTableView(this);
    m_fileView->setModel(m_proxy);
    m_fileView->setSortingEnabled(true);
    m_fileView->setAlternatingRowColors(true);
    m_fileView->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_fileView->setSelectionMode(QAbstractItemView::SingleSelection);
    m_fileView->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_fileView->setShowGrid(false);
    m_fileView->setWordWrap(false);
    m_fileView->verticalHeader()->setVisible(false);
    m_fileView->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    m_fileView->setContextMenuPolicy(Qt::CustomContextMenu);

    QHeaderView* hdr = m_fileView->horizontalHeader();
    hdr->setSectionResizeMode(FileListModel::ColCheck, QHeaderView::Fixed);
    m_fileView->setColumnWidth(FileListModel::ColCheck, 32);
    m_fileView->setItemDelegateForColumn(FileListModel::ColCheck,
                                          new CheckBoxDelegate(m_fileView));
    hdr->setSectionResizeMode(FileListModel::ColPath, QHeaderView::Stretch);
    hdr->setSectionResizeMode(FileListModel::ColSize, QHeaderView::ResizeToContents);
    hdr->setSectionResizeMode(FileListModel::ColRes, QHeaderView::ResizeToContents);
    hdr->setSectionResizeMode(FileListModel::ColDuration, QHeaderView::ResizeToContents);
    hdr->setSectionResizeMode(FileListModel::ColCodec, QHeaderView::ResizeToContents);
    hdr->setSectionResizeMode(FileListModel::ColAction, QHeaderView::Fixed);
    m_fileView->setColumnWidth(FileListModel::ColAction, 70);
    hdr->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    hdr->setStretchLastSection(false);
    hdr->setSectionsMovable(true);        // 列可拖拽排序
    m_fileView->setMouseTracking(true);

    connect(m_fileView, &QTableView::clicked,
            this, &FileCopierWindow::onTableClicked);
    connect(m_fileView, &QTableView::doubleClicked,
            this, &FileCopierWindow::onTableDoubleClicked);
    connect(m_fileView, &QTableView::customContextMenuRequested,
            this, &FileCopierWindow::onTableContextMenu);

    m_fileView->setStyleSheet(
        "QTableView{border:1px solid #d8d8d8;border-radius:4px;"
        "font-family:Consolas,monospace;font-size:12px;}"
        "QTableView::item:selected{background:#2196F3;color:white;}"
        "QHeaderView::section{background:#f5f5f5;padding:5px 8px;"
        "border:none;border-bottom:2px solid #e0e0e0;font-weight:bold;font-size:11px;}");

    main->addWidget(m_fileView, 3);

    // 批处理按钮行
    QHBoxLayout* checkRow = new QHBoxLayout();
    checkRow->setSpacing(6);
    m_selectAllBtn = new QPushButton("全选", this);
    m_selectAllBtn->setFixedHeight(24);
    m_selectAllBtn->setStyleSheet("QPushButton{font-size:11px;padding:2px 10px;}");
    m_checkedLabel = new QLabel("", this);
    m_checkedLabel->setStyleSheet("color:#888;font-size:11px;");

    checkRow->addWidget(m_selectAllBtn);
    checkRow->addWidget(m_checkedLabel);

    m_selectedLabel = new QLabel("", this);
    m_selectedLabel->setStyleSheet("color:#888;font-size:11px;margin-left:8px;");
    checkRow->addWidget(m_selectedLabel);

    // 筛选下拉框
    m_typeFilterCombo = new QComboBox(this);
    m_typeFilterCombo->addItem("全部文件");
    m_typeFilterCombo->addItem("🎬 仅视频");
    m_typeFilterCombo->addItem("🖼 仅图片");
    m_typeFilterCombo->addItem("🎵 仅音频");
    m_typeFilterCombo->setFixedHeight(24);
    m_typeFilterCombo->setStyleSheet("QComboBox{font-size:11px;}");
    connect(m_typeFilterCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &FileCopierWindow::onTypeFilterChanged);
    checkRow->addWidget(m_typeFilterCombo);

    // 高级筛选按钮
    QPushButton* btnMeta = new QPushButton("🔽 高级", this);
    btnMeta->setFixedHeight(24); btnMeta->setCheckable(true);
    btnMeta->setStyleSheet("QPushButton{font-size:11px;padding:2px 8px;}");
    connect(btnMeta, &QPushButton::toggled, this, [this](bool on) {
        if (m_metaPanel) m_metaPanel->setVisible(on);
    });
    checkRow->addWidget(btnMeta);

    checkRow->addStretch();
    main->addLayout(checkRow);

    connect(m_selectAllBtn, &QPushButton::clicked, this, &FileCopierWindow::onSelectAll);

    // 汇总标签
    m_summaryLabel = new QLabel("尚未扫描", this);
    m_summaryLabel->setStyleSheet("color:#888;padding:2px 4px;font-size:11px;");
    main->addWidget(m_summaryLabel);
}

// ═══════════════════════════════════════════════════════════════
// 目标区
// ═══════════════════════════════════════════════════════════════

// ═══════════════════════════════════════════════════════════════
// 高级筛选面板 (可折叠)
// ═══════════════════════════════════════════════════════════════

void FileCopierWindow::setupMetaFilterPanel(QVBoxLayout* main)
{
    m_metaPanel = new QWidget(this);
    m_metaPanel->setVisible(false);
    QHBoxLayout* row = new QHBoxLayout(m_metaPanel);
    row->setContentsMargins(4, 2, 4, 4);
    row->setSpacing(8);

    // 分辨率
    row->addWidget(new QLabel("分辨率:", this));
    m_cmbRes = new QComboBox(this);
    m_cmbRes->addItem("不限", 0);
    m_cmbRes->addItem("≥ 4K (3840×2160)", 3840);
    m_cmbRes->addItem("≥ 1080p (1920×1080)", 1920);
    m_cmbRes->addItem("≥ 720p (1280×720)", 1280);
    m_cmbRes->setStyleSheet("QComboBox{font-size:11px;}");
    connect(m_cmbRes, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &FileCopierWindow::onMetaFilterApply);
    row->addWidget(m_cmbRes);

    // 时长
    row->addWidget(new QLabel("时长:", this));
    m_cmbDur = new QComboBox(this);
    m_cmbDur->addItem("不限", 0);
    m_cmbDur->addItem("≥ 1 分钟", 60000LL);
    m_cmbDur->addItem("≥ 5 分钟", 300000LL);
    m_cmbDur->addItem("≥ 10 分钟", 600000LL);
    m_cmbDur->addItem("≥ 30 分钟", 1800000LL);
    m_cmbDur->addItem("≥ 1 小时", 3600000LL);
    m_cmbDur->setStyleSheet("QComboBox{font-size:11px;}");
    connect(m_cmbDur, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &FileCopierWindow::onMetaFilterApply);
    row->addWidget(m_cmbDur);

    // 编码
    row->addWidget(new QLabel("编码:", this));
    m_cmbCodec = new QComboBox(this);
    m_cmbCodec->addItem("不限", "");
    m_cmbCodec->addItem("H.264 (AVC)", "AVC");
    m_cmbCodec->addItem("H.265 (HEVC)", "HEVC");
    m_cmbCodec->addItem("ProRes", "ProRes");
    m_cmbCodec->addItem("VP9", "VP9");
    m_cmbCodec->addItem("AV1", "AV1");
    m_cmbCodec->setStyleSheet("QComboBox{font-size:11px;}");
    connect(m_cmbCodec, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &FileCopierWindow::onMetaFilterApply);
    row->addWidget(m_cmbCodec);

    // 预设
    row->addWidget(new QLabel("预设:", this));
    m_cmbPreset = new QComboBox(this);
    m_cmbPreset->addItem("(无)");
    m_cmbPreset->setStyleSheet("QComboBox{font-size:11px;}");
    connect(m_cmbPreset, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &FileCopierWindow::onFilterPresetLoad);
    row->addWidget(m_cmbPreset);

    QPushButton* btnSavePre = new QPushButton("保存", this);
    btnSavePre->setFixedHeight(24);
    btnSavePre->setStyleSheet("QPushButton{font-size:11px;padding:2px 8px;}");
    connect(btnSavePre, &QPushButton::clicked, this, &FileCopierWindow::onFilterPresetSave);
    row->addWidget(btnSavePre);

    QPushButton* btnClear = new QPushButton("清除", this);
    btnClear->setFixedHeight(24);
    btnClear->setStyleSheet("QPushButton{font-size:11px;padding:2px 8px;}");
    connect(btnClear, &QPushButton::clicked, this, &FileCopierWindow::onMetaFilterClear);
    row->addWidget(btnClear);

    row->addStretch();
    main->addWidget(m_metaPanel);
}

// ═══════════════════════════════════════════════════════════════
// 目标区
// ═══════════════════════════════════════════════════════════════

void FileCopierWindow::setupDestinationSection(QVBoxLayout* main)
{
    QLabel* label = new QLabel("目标位置", this);
    QFont f = label->font(); f.setBold(true); f.setPointSize(11);
    label->setFont(f);
    main->addWidget(label);

    m_destList = new QListWidget(this);
    m_destList->setMaximumHeight(95);
    m_destList->setAlternatingRowColors(true);
    m_destList->setStyleSheet(
        "QListWidget{border:1px solid #d0d0d0;border-radius:4px;font-size:12px;}"
        "QListWidget::item{padding:3px 6px;}"
        "QListWidget::item:selected{background:#2E7D32;}"
        "QListWidget::item:selected:!active{background:#2E7D32;}"
        "QListWidget::item:alternate{background:#fafafa;}"
        "QListWidget::item:selected:alternate{background:#2E7D32;}");
    m_destList->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_destList, &QListWidget::customContextMenuRequested, this,
            [this](const QPoint& pos) {
        QListWidgetItem* item = m_destList->itemAt(pos);
        if (!item) return;
        QMenu menu;
        QAction* aDel = menu.addAction("移除");
        QAction* aOpen = menu.addAction("打开目录");
        QAction* c = menu.exec(m_destList->viewport()->mapToGlobal(pos));
        if (c == aDel)  delete m_destList->takeItem(m_destList->row(item));
        if (c == aOpen) openFileLocation(item->data(Qt::UserRole).toString());
    });
    QShortcut* delDest = new QShortcut(QKeySequence::Delete, m_destList);
    delDest->setContext(Qt::WidgetShortcut);
    connect(delDest, &QShortcut::activated, this, [this]() {
        for (auto* item : m_destList->selectedItems())
            delete m_destList->takeItem(m_destList->row(item));
        updateCopyButtonState();
    });
    main->addWidget(m_destList);

    // 选项
    QHBoxLayout* optRow = new QHBoxLayout();
    m_skipExistingCb = new QCheckBox("跳过已存在的文件", this);
    m_preserveTsCb   = new QCheckBox("保留原始时间戳", this);
    m_preserveTsCb->setChecked(true);
    m_skipExistingCb->setStyleSheet("font-size:11px;color:#555;");
    m_preserveTsCb->setStyleSheet("font-size:11px;color:#555;");
    connect(m_skipExistingCb, &QCheckBox::toggled, this, [this]() { saveSettings(); });
    connect(m_preserveTsCb,   &QCheckBox::toggled, this, [this]() { saveSettings(); });
    optRow->addWidget(m_skipExistingCb);
    optRow->addWidget(m_preserveTsCb);
    optRow->addStretch();
    main->addLayout(optRow);

    // 按钮行
    QHBoxLayout* row = new QHBoxLayout(); row->setSpacing(6);
    m_addDestBtn = new QPushButton("＋ 添加", this);
    m_addDestBtn->setFixedHeight(28);
    m_autoDestBtn = new QPushButton("🖥 检测磁盘", this);
    m_autoDestBtn->setFixedHeight(28);
    m_autoDestBtn->setStyleSheet("QPushButton{color:#555;font-size:11px;}");
    row->addWidget(m_addDestBtn); row->addWidget(m_autoDestBtn); row->addStretch();

    m_copyBtn = new QPushButton("▶ 开始复制", this);
    m_copyBtn->setFixedHeight(32); m_copyBtn->setEnabled(false);
    m_copyBtn->setStyleSheet("QPushButton{background:#4CAF50;color:white;font-weight:bold;"
                              "padding:4px 28px;border-radius:4px;font-size:13px;}"
                              "QPushButton:hover{background:#388E3C;}QPushButton:disabled{background:#ccc;}");
    m_cancelCopyBtn = new QPushButton("取消", this);
    m_cancelCopyBtn->setFixedHeight(32); m_cancelCopyBtn->setVisible(false);
    m_cancelCopyBtn->setStyleSheet("QPushButton{background:#f44336;color:white;font-weight:bold;"
                                    "padding:4px 16px;border-radius:4px;}QPushButton:hover{background:#d32f2f;}");
    row->addWidget(m_copyBtn); row->addWidget(m_cancelCopyBtn);
    main->addLayout(row);

    // 进度 (上下两行: 进度条在上, 文字在下)
    QVBoxLayout* progBox = new QVBoxLayout();
    progBox->setSpacing(2);
    m_copyProgress = new QProgressBar(this);
    m_copyProgress->setRange(0, 100); m_copyProgress->setValue(0);
    m_copyProgress->setVisible(false); m_copyProgress->setMaximumHeight(12);
    m_copyProgress->setTextVisible(false);
    m_copyStatus = new QLabel("", this);
    m_copyStatus->setStyleSheet("color:#666;font-size:11px;");
    progBox->addWidget(m_copyProgress);
    progBox->addWidget(m_copyStatus);
    main->addLayout(progBox);

    connect(m_addDestBtn, &QPushButton::clicked, this, &FileCopierWindow::onAddDestPath);
    connect(m_autoDestBtn, &QPushButton::clicked, this, &FileCopierWindow::onAutoDetectDestinations);
    connect(m_copyBtn, &QPushButton::clicked, this, &FileCopierWindow::onCopyStart);
    connect(m_cancelCopyBtn, &QPushButton::clicked, this, &FileCopierWindow::onCancelCopy);
}

// ═══════════════════════════════════════════════════════════════
// 日志
// ═══════════════════════════════════════════════════════════════

void FileCopierWindow::setupLogSection(QVBoxLayout* main)
{
    QHBoxLayout* header = new QHBoxLayout();
    QPushButton* toggleLog = new QPushButton("📋 日志 ▾", this);
    toggleLog->setFlat(true); toggleLog->setCursor(Qt::PointingHandCursor);
    toggleLog->setStyleSheet("QPushButton{font-weight:bold;font-size:11px;color:#555;"
                              "padding:2px 6px;border:none;}");
    header->addWidget(toggleLog); header->addStretch();
    main->addLayout(header);

    m_logEdit = new QTextEdit(this);
    m_logEdit->setReadOnly(true); m_logEdit->setMaximumHeight(120);
    m_logEdit->setFont(QFont("Consolas", 9));
    m_logEdit->setStyleSheet("QTextEdit{background:#1e1e1e;color:#d4d4d4;border:1px solid #444;"
                              "border-radius:4px;padding:5px;}");
    main->addWidget(m_logEdit);

    connect(toggleLog, &QPushButton::clicked, this, [this, toggleLog]() {
        bool v = m_logEdit->isVisible();
        m_logEdit->setVisible(!v);
        toggleLog->setText(v ? "📋 日志 ▸" : "📋 日志 ▾");
    });
}

// ═══════════════════════════════════════════════════════════════
// 配置持久化
// ═══════════════════════════════════════════════════════════════

static const char* SET_ORG = "MediaTool";
static const char* SET_APP = "FileCopier";

void FileCopierWindow::loadSettings()
{
    QSettings s(SET_ORG, SET_APP);
    s.beginGroup("MainWindow");
    if (s.contains("geometry")) restoreGeometry(s.value("geometry").toByteArray());
    if (s.contains("headerState"))
        m_fileView->horizontalHeader()->restoreState(s.value("headerState").toByteArray());
    s.endGroup();

    // 源
    s.beginGroup("Sources");
    int nSrc = s.beginReadArray("paths");
    for (int i = 0; i < nSrc; ++i) {
        s.setArrayIndex(i);
        QString p = s.value("path").toString();
        bool ck   = s.value("checked", true).toBool();
        if (p.isEmpty() || !QDir(p).exists()) continue;
        bool ex = false;
        for (int j = 0; j < m_sourceList->count(); ++j)
            if (m_sourceList->item(j)->data(Qt::UserRole).toString() == p) { ex = true; break; }
        if (ex) continue;
        QListWidgetItem* item = new QListWidgetItem(p);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(ck ? Qt::Checked : Qt::Unchecked);
        item->setData(Qt::UserRole, p); item->setToolTip(p);
        m_sourceList->addItem(item);
    }
    s.endArray(); s.endGroup();

    // 目标
    s.beginGroup("Destinations");
    int nDst = s.beginReadArray("paths");
    for (int i = 0; i < nDst; ++i) {
        s.setArrayIndex(i);
        QString p = s.value("path").toString();
        if (p.isEmpty() || !QDir(p).exists()) continue;
        bool ex = false;
        for (int j = 0; j < m_destList->count(); ++j)
            if (m_destList->item(j)->data(Qt::UserRole).toString() == p) { ex = true; break; }
        if (ex) continue;
        DiskInfo info = DiskDetector::detectForPath(p);
        QListWidgetItem* item = new QListWidgetItem(
            QString("%1  [%2]").arg(p).arg(info.typeLabel()));
        item->setData(Qt::UserRole, p);
        item->setToolTip(QString("卷标:%1 | 可用:%2 | 策略:%3线程")
                             .arg(info.volumeName).arg(formatSize(info.freeBytes))
                             .arg(info.recommendedConcurrency()));
        m_destList->addItem(item);
    }
    s.endArray(); s.endGroup();

    m_skipExistingCb->setChecked(s.value("Options/SkipExisting", false).toBool());
    m_preserveTsCb->setChecked(s.value("Options/PreserveTimestamp", true).toBool());

    QString flt = s.value("Options/Filter").toString();
    if (!flt.isEmpty()) {
        m_scanner->setFilters(flt.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts));
        QLineEdit* fe = findChild<QLineEdit*>();
        if (fe) fe->setText(flt);
    }
    updateCopyButtonState();
    refreshSchemeCombo();
    refreshPresetCombo();
}

void FileCopierWindow::saveSettings()
{
    QSettings s(SET_ORG, SET_APP);
    s.beginGroup("MainWindow");
    s.setValue("geometry", saveGeometry());
    s.setValue("headerState", m_fileView->horizontalHeader()->saveState());
    s.endGroup();

    s.beginGroup("Sources"); s.beginWriteArray("paths");
    for (int i = 0; i < m_sourceList->count(); ++i) {
        s.setArrayIndex(i);
        auto* item = m_sourceList->item(i);
        s.setValue("path", item->data(Qt::UserRole).toString());
        s.setValue("checked", item->checkState() == Qt::Checked);
    }
    s.endArray(); s.endGroup();

    s.beginGroup("Destinations"); s.beginWriteArray("paths");
    for (int i = 0; i < m_destList->count(); ++i) {
        s.setArrayIndex(i);
        s.setValue("path", m_destList->item(i)->data(Qt::UserRole).toString());
    }
    s.endArray(); s.endGroup();

    s.setValue("Options/SkipExisting",      m_skipExistingCb->isChecked());
    s.setValue("Options/PreserveTimestamp", m_preserveTsCb->isChecked());
    QLineEdit* fe = findChild<QLineEdit*>();
    if (fe) s.setValue("Options/Filter", fe->text());
}

// ═══════════════════════════════════════════════════════════════
// 源路径
// ═══════════════════════════════════════════════════════════════

void FileCopierWindow::onAddSourcePath()
{
    QString p = QFileDialog::getExistingDirectory(this, "选择源目录", QDir::homePath(),
                                                   QFileDialog::ShowDirsOnly);
    if (p.isEmpty()) return;
    for (int i = 0; i < m_sourceList->count(); ++i)
        if (m_sourceList->item(i)->data(Qt::UserRole).toString() == p) return;

    DiskInfo info = DiskDetector::detectForPath(p);
    QString label = info.volumeName.isEmpty() ? p
                    : QString("%1  (%2)").arg(p).arg(info.volumeName);
    QListWidgetItem* item = new QListWidgetItem(label);
    item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
    item->setCheckState(Qt::Checked);
    item->setData(Qt::UserRole, p);
    item->setToolTip(QString("类型:%1 | 可用:%2")
                         .arg(info.typeLabel()).arg(formatSize(info.freeBytes)));
    m_sourceList->addItem(item);
    appendLog("已添加: " + p);
}

void FileCopierWindow::onAutoDetectSources()
{
    QVector<DiskInfo> disks = DiskDetector::allAvailableDisks();
    int added = 0;
    for (const auto& d : disks) {
        if (!d.isValidTarget()) continue;
        bool ex = false;
        for (int i = 0; i < m_sourceList->count(); ++i)
            if (m_sourceList->item(i)->data(Qt::UserRole).toString() == d.rootPath)
            { ex = true; break; }
        if (ex) continue;
        QListWidgetItem* item = new QListWidgetItem(
            QString("%1  (%2 · %3)").arg(d.rootPath).arg(d.volumeName).arg(formatSize(d.totalBytes)));
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(d.isSystemDisk ? Qt::Unchecked : Qt::Checked);
        item->setData(Qt::UserRole, d.rootPath);
        item->setToolTip(QString("类型:%1 | 可用:%2")
                             .arg(d.typeLabel()).arg(formatSize(d.freeBytes)));
        m_sourceList->addItem(item); ++added;
    }
    appendLog(QString("检测到 %1 个设备 (系统盘默认不勾选)").arg(added));
}

void FileCopierWindow::onSourceItemChanged(QListWidgetItem*) { updateCopyButtonState(); }

QStringList FileCopierWindow::getCheckedSourcePaths() const
{
    QStringList r;
    for (int i = 0; i < m_sourceList->count(); ++i) {
        auto* item = m_sourceList->item(i);
        if (item->checkState() == Qt::Checked)
            r << item->data(Qt::UserRole).toString();
    }
    return r;
}

// ═══════════════════════════════════════════════════════════════
// 扫描
// ═══════════════════════════════════════════════════════════════

void FileCopierWindow::onScanStart()
{
    QStringList paths = getCheckedSourcePaths();
    if (paths.isEmpty()) { QMessageBox::warning(this, "提示", "请先勾选源目录。"); return; }

    m_scanning = true;
    m_scanBtn->setVisible(false); m_cancelScanBtn->setVisible(true);
    m_exportBtn->setEnabled(false);
    m_scanProgress->setVisible(true); m_scanStatus->setText("扫描中...");
    m_copyBtn->setEnabled(false);
    m_model->clear();
    m_scanner->reset();

    QMap<QString, QStringList> groups = DiskDetector::groupByPhysicalDisk(paths);
    appendLog(QString("扫描 %1 个源 (分布在 %2 个物理磁盘)...")
                  .arg(paths.size()).arg(groups.size()));

    m_pool->enqueue([this, paths]() {
        for (const QString& p : paths) {
            if (m_scanner->isCancelled()) break;
            m_scanner->scan(p);
        }
    });
}

void FileCopierWindow::onCancelScan()
{
    m_scanner->cancel();
    m_cancelScanBtn->setVisible(false); m_scanBtn->setVisible(true);
    appendLog("已取消扫描");
}

void FileCopierWindow::onBatchFound(const QVector<FileEntry>& batch)
{
    m_model->addFiles(batch);
    m_scanStatus->setText(QString("已发现 %1 个文件")
                              .arg(QLocale().toString(m_model->fileCount())));
}

void FileCopierWindow::onScanProgress(int n)
{
    m_scanStatus->setText(QString("已扫描 %1 个文件...").arg(QLocale().toString(n)));
}

void FileCopierWindow::onScanFinished(int total, qint64 size)
{
    m_scanning = false;
    m_cancelScanBtn->setVisible(false); m_scanBtn->setVisible(true);
    m_scanProgress->setVisible(false); m_scanStatus->setText("");
    appendLog(QString("扫描完成: %1 个文件, %2")
                  .arg(QLocale().toString(total)).arg(formatSize(size)));
    updateCopyButtonState();

    // 自动开始提取元数据
    if (total > 0) startMetadataExtraction();
}

// ═══════════════════════════════════════════════════════════════
// 元数据提取 (MediaInfo)
// ═══════════════════════════════════════════════════════════════

void FileCopierWindow::startMetadataExtraction()
{
    m_extractingMeta = true;
    updateCopyButtonState();
    appendLog("开始提取元数据（并行）...");
    m_scanStatus->setText("提取元数据中...");
    m_scanProgress->setVisible(true);
    m_scanProgress->setRange(0, 0);

    QStringList allPaths;
    allPaths.reserve(m_model->fileCount());
    for (int i = 0; i < m_model->fileCount(); ++i)
        allPaths << m_model->filePathAt(i);

    const int total = allPaths.size();
    const int BATCH = 200;

    auto processed     = std::make_shared<int>(0);
    auto lastProcessed = std::make_shared<int>(0);

    // ═══ 看门狗: 30 秒无进度 → 强制结束 ═══
    QTimer* wd = new QTimer(this);
    connect(wd, &QTimer::timeout, this, [this, processed, lastProcessed, wd, total]() {
        if (*processed == *lastProcessed && *processed < total) {
            wd->stop(); wd->deleteLater();
            m_extractingMeta = false;
            m_scanProgress->setVisible(false);
            m_scanStatus->setText("");
            appendLog(QString("元数据提取结束: %1/%2 (剩余批次超时跳过)")
                          .arg(QLocale().toString(*processed))
                          .arg(QLocale().toString(total)));
        }
        *lastProcessed = *processed;
    });
    wd->start(5000);

    // ═══ 并行提交所有批次到线程池 ═══
    for (int i = 0; i < total; i += BATCH) {
        QStringList batch = allPaths.mid(i, BATCH);
        int batchSize = batch.size();

        m_pool->enqueue([this, batch, batchSize, processed, total, wd]() {
            QVector<MediaMetadata> results = extractMetadata(batch);

            QMetaObject::invokeMethod(this, [this, results, batchSize, processed, total, wd]() {
                m_model->setMetadata(results);
                for (const auto& md : results) {
                    if (!md.isValid && md.fileSize > 0)
                        appendLog(QString("  无法解析: %1")
                                      .arg(QFileInfo(md.filePath).fileName()));
                }
                *processed += batchSize;

                m_scanStatus->setText(QString("元数据: %1 / %2")
                                          .arg(QLocale().toString(*processed))
                                          .arg(QLocale().toString(total)));

                if (*processed >= total) {
                    wd->stop(); wd->deleteLater();
                    m_extractingMeta = false;
                    updateCopyButtonState();
                    m_scanProgress->setVisible(false);
                    m_scanStatus->setText("");
                    int has = 0;
                    for (int r = 0; r < m_model->fileCount(); ++r)
                        if (m_model->hasMetadata(m_model->filePathAt(r))) ++has;
                    appendLog(QString("元数据完成: %1/%2 个文件 (可解析)")
                                  .arg(QLocale().toString(has))
                                  .arg(QLocale().toString(total)));
                }
            }, Qt::QueuedConnection);
        });
    }
}

void FileCopierWindow::onMetadataBatch(const QVector<MediaMetadata>& batch)
{
    m_model->setMetadata(batch);
}

void FileCopierWindow::onTypeFilterChanged(int index)
{
    // 0=全部, 1=视频, 2=图片, 3=音频
    if (index == 0) {
        m_proxy->setFilterRegularExpression(QRegularExpression());
        m_proxy->setFilterKeyColumn(-1);
    } else {
        QStringList exts;
        switch (index) {
        case 1: exts << "mp4"<<"avi"<<"mkv"<<"mov"<<"wmv"<<"flv"<<"webm"<<"mts"<<"m2ts"<<"ts"<<"3gp"; break;
        case 2: exts << "jpg"<<"jpeg"<<"png"<<"gif"<<"bmp"<<"tiff"<<"tif"<<"webp"<<"heic"<<"heif"<<"raw"<<"cr2"<<"nef"<<"arw"; break;
        case 3: exts << "mp3"<<"wav"<<"flac"<<"aac"<<"ogg"<<"wma"<<"m4a"<<"opus"; break;
        }
        QString pattern = "\\.(" + exts.join("|") + ")$";
        m_proxy->setFilterRegularExpression(
            QRegularExpression(pattern, QRegularExpression::CaseInsensitiveOption));
        m_proxy->setFilterKeyColumn(FileListModel::ColPath);
    }
}

void FileCopierWindow::updateSummaryLabel(int count, qint64 size)
{
    if (count == 0) {
        m_summaryLabel->setText("尚未扫描");
        return;
    }
    QString text = QString("共 %1 个文件  ·  %2")
                       .arg(QLocale().toString(count)).arg(formatSize(size));
    // 分类统计
    if (m_model->videoCount() > 0)
        text += QString("    🎬 视频: %1 (%2)")
                    .arg(QLocale().toString(m_model->videoCount()))
                    .arg(formatSize(m_model->videoSize()));
    if (m_model->imageCount() > 0)
        text += QString("    🖼 图片: %1 (%2)")
                    .arg(QLocale().toString(m_model->imageCount()))
                    .arg(formatSize(m_model->imageSize()));
    if (m_model->audioCount() > 0)
        text += QString("    🎵 音频: %1 (%2)")
                    .arg(QLocale().toString(m_model->audioCount()))
                    .arg(formatSize(m_model->audioSize()));
    m_summaryLabel->setText(text);
}

// ═══════════════════════════════════════════════════════════════
// Checkbox 批处理
// ═══════════════════════════════════════════════════════════════

void FileCopierWindow::onSelectAll()
{
    bool allChecked = m_model->checkedCount() == m_model->fileCount();
    m_model->setAllChecked(!allChecked);
    m_selectAllBtn->setText(allChecked ? "全选" : "取消全选");
}

// ═══════════════════════════════════════════════════════════════
// 目标路径
// ═══════════════════════════════════════════════════════════════

void FileCopierWindow::onAddDestPath()
{
    QString p = QFileDialog::getExistingDirectory(this, "选择目标目录", QDir::homePath(),
                                                   QFileDialog::ShowDirsOnly);
    if (p.isEmpty()) return;
    for (int i = 0; i < m_destList->count(); ++i)
        if (m_destList->item(i)->data(Qt::UserRole).toString() == p) return;

    DiskInfo info = DiskDetector::detectForPath(p);
    QListWidgetItem* item = new QListWidgetItem(
        QString("%1  [%2]").arg(p).arg(info.typeLabel()));
    item->setData(Qt::UserRole, p);
    item->setToolTip(QString("卷标:%1 | 可用:%2 | 策略:%3线程")
                         .arg(info.volumeName).arg(formatSize(info.freeBytes))
                         .arg(info.recommendedConcurrency()));
    m_destList->addItem(item);
    updateCopyButtonState();
    appendLog(QString("目标: %1 (%2, 可用 %3)")
                  .arg(p).arg(info.typeLabel()).arg(formatSize(info.freeBytes)));
}

void FileCopierWindow::onAutoDetectDestinations()
{
    QVector<DiskInfo> disks = DiskDetector::allAvailableDisks();
    int added = 0;
    for (const auto& d : disks) {
        if (!d.isValidTarget() || d.isSystemDisk) continue;
        bool ex = false;
        for (int i = 0; i < m_destList->count(); ++i)
            if (m_destList->item(i)->data(Qt::UserRole).toString() == d.rootPath)
            { ex = true; break; }
        if (ex) continue;
        QListWidgetItem* item = new QListWidgetItem(
            QString("%1  [%2]").arg(d.rootPath).arg(d.typeLabel()));
        item->setData(Qt::UserRole, d.rootPath);
        item->setToolTip(QString("卷标:%1 | 可用:%2 | 策略:%3线程")
                             .arg(d.volumeName).arg(formatSize(d.freeBytes))
                             .arg(d.recommendedConcurrency()));
        m_destList->addItem(item); ++added;
    }
    updateCopyButtonState();
    appendLog(QString("检测到 %1 个可用磁盘作为目标").arg(added));
}

QStringList FileCopierWindow::getDestinationPaths() const
{
    QStringList r;
    for (int i = 0; i < m_destList->count(); ++i)
        r << m_destList->item(i)->data(Qt::UserRole).toString();
    return r;
}

// ═══════════════════════════════════════════════════════════════
// 复制 (多目标 + checkbox + timestamp)
// ═══════════════════════════════════════════════════════════════

void FileCopierWindow::onCopyStart()
{
    QStringList dests = getDestinationPaths();
    if (dests.isEmpty()) { QMessageBox::warning(this, "提示", "请先添加目标。"); return; }

    if (m_extractingMeta) {
        QMessageBox::warning(this, "提示", "元数据提取尚未完成，请稍候再试。");
        return;
    }

    // 确定要复制的文件: 必须勾选
    QStringList filesToCopy = m_model->checkedFiles();
    if (filesToCopy.isEmpty()) {
        QMessageBox::warning(this, "提示",
                             "请先在文件列表中勾选要复制的文件。\n"
                             "可使用 [全选] 按钮快速勾选全部文件。");
        return;
    }
    appendLog(QString("复制已勾选的 %1 个文件。").arg(QLocale().toString(filesToCopy.size())));

    // 只选了部分文件 → 弹确认框
    int totalFiles = m_model->fileCount();
    if (filesToCopy.size() < totalFiles) {
        QMessageBox box(this);
        box.setWindowTitle("确认复制");
        box.setText(QString("你只勾选了 %1 / %2 个文件。")
                        .arg(QLocale().toString(filesToCopy.size()))
                        .arg(QLocale().toString(totalFiles)));
        box.setInformativeText("请选择操作：");
        box.setIcon(QMessageBox::Question);

        QPushButton* btnExport = box.addButton("导出清单", QMessageBox::AcceptRole);
        QPushButton* btnCopy   = box.addButton("确认复制", QMessageBox::YesRole);
        QPushButton* btnCancel = box.addButton("取消复制", QMessageBox::RejectRole);
        box.setDefaultButton(btnCopy);
        box.exec();

        if (box.clickedButton() == btnCancel) return;
        if (box.clickedButton() == btnExport) {
            onExport();
            return;
        }
        // btnCopy → 继续复制
    }

    // 多目标二次确认
    if (dests.size() > 1) {
        QStringList names;
        for (const QString& d : dests)
            names << QString("  • %1").arg(d);
        QMessageBox box(this);
        box.setWindowTitle("确认复制");
        box.setText(QString("将复制 %1 个文件到 %2 个目标位置：")
                        .arg(QLocale().toString(filesToCopy.size()))
                        .arg(dests.size()));
        box.setInformativeText(names.join("\n"));
        box.setIcon(QMessageBox::Question);
        QPushButton* btnOk  = box.addButton("确认", QMessageBox::AcceptRole);
        box.addButton("取消", QMessageBox::RejectRole);
        box.setDefaultButton(btnOk);
        box.exec();
        if (box.clickedButton() != btnOk) return;
    }

    if (filesToCopy.isEmpty()) { QMessageBox::warning(this, "提示", "没有文件可复制。"); return; }

    // 锁定快照
    m_snapshotFiles = filesToCopy;
    m_snapshotSizeMap.clear();
    for (const QString& f : m_snapshotFiles)
        m_snapshotSizeMap[f] = 0;
    for (int i = 0; i < m_model->fileCount(); ++i) {
        QString fp = m_model->filePathAt(i);
        if (m_snapshotSizeMap.contains(fp))
            m_snapshotSizeMap[fp] = m_model->fileSizeAt(i);
    }

    // ─── 磁盘空间检查 ───
    qint64 totalNeeded = 0;
    for (const QString& f : m_snapshotFiles)
        totalNeeded += m_snapshotSizeMap.value(f, 0);

    QStringList insufficient;
    for (const QString& dst : dests) {
        DiskInfo info = DiskDetector::detectForPath(dst);
        if (info.freeBytes > 0 && totalNeeded > info.freeBytes) {
            insufficient << QString("  %1  (需要 %2, 可用 %3)")
                            .arg(dst)
                            .arg(formatSize(totalNeeded))
                            .arg(formatSize(info.freeBytes));
        }
    }

    if (!insufficient.isEmpty()) {
        QString msg = QString("目标磁盘空间不足！\n\n"
                               "总计需要: %1\n\n"
                               "空间不足的目标:\n%2\n\n"
                               "是否仍要继续复制？")
                           .arg(formatSize(totalNeeded))
                           .arg(insufficient.join("\n"));
        QMessageBox::StandardButton btn = QMessageBox::warning(
            this, "磁盘空间不足", msg,
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (btn != QMessageBox::Yes) return;
    }

    // 清理旧引擎
    for (auto* c : m_copiers) { c->cancel(); c->deleteLater(); }
    m_copiers.clear();

    m_totalCopies = dests.size();
    m_activeCopies = 0;
    m_totalSuccess = 0;
    m_totalFailed  = 0;

    bool skipExist  = m_skipExistingCb->isChecked();
    bool preserveTs = m_preserveTsCb->isChecked();

    for (const QString& dst : dests) {
        auto* copier = new FileCopierEngine(this);
        copier->setSkipExisting(skipExist);
        copier->setPreserveTimestamp(preserveTs);
        connect(copier, &FileCopierEngine::progressUpdated, this,
                [this, dst](int pct, int cp, int fl, int tot, qint64 b) {
                    onCopyProgress(pct, cp, fl, tot, b, dst);
                });
        connect(copier, &FileCopierEngine::copyFinished, this,
                [this, dst](int tot, int ok, int fl, qint64 b) {
                    onCopyFinished(tot, ok, fl, b, dst);
                });
        connect(copier, &FileCopierEngine::fileError, this, &FileCopierWindow::onFileError);
        connect(copier, &FileCopierEngine::logMessage, this, &FileCopierWindow::appendLog);
        m_copiers.append(copier);
    }

    m_copyBtn->setVisible(false); m_cancelCopyBtn->setVisible(true);
    m_scanBtn->setEnabled(false);
    m_copyProgress->setVisible(true); m_copyProgress->setValue(0);
    m_copyTimer.start();
    m_copyStatus->setText(QString("启动 %1 个任务...").arg(m_totalCopies));
    appendLog(QString("═══ 复制 %1 个文件 → %2 个目标 ═══")
                  .arg(QLocale().toString(m_snapshotFiles.size())).arg(m_totalCopies));
    if (skipExist)  appendLog("  跳过已存在: 开启");
    if (preserveTs) appendLog("  保留时间戳: 开启");

    for (int i = 0; i < m_copiers.size(); ++i) {
        m_activeCopies++;
        FileCopierEngine* copier = m_copiers[i];
        QString dp = dests[i];
        DiskInfo info = DiskDetector::detectForPath(dp);
        appendLog(QString("  → %1 (%2, %3线程)")
                      .arg(dp).arg(info.typeLabel()).arg(info.recommendedConcurrency()));
        m_pool->enqueue([copier, dp, this]() {
            copier->start(m_snapshotFiles, m_snapshotSizeMap, dp);
        });
    }
}

void FileCopierWindow::onCancelCopy()
{
    for (auto* c : m_copiers) c->cancel();
    m_cancelCopyBtn->setVisible(false); m_copyBtn->setVisible(true);
    appendLog("已取消所有复制任务");
}

void FileCopierWindow::onCopyProgress(int pct, int copied, int failed,
                                       int total, qint64 bytes, const QString& dst)
{
    m_copyProgress->setValue(pct);
    // 速率 / 已用时间 / 预估剩余
    qint64 elap = m_copyTimer.elapsed() / 1000;
    QString speed, eta;
    if (elap > 0 && bytes > 0) {
        speed = formatSize((qint64)(bytes / (double)elap)) + "/s";
        int done = copied + failed;
        if (done > 0 && done < total) {
            qint64 e = elap * (total - done) / done;
            if (e >= 3600) eta = QString(" 约%1h%2m").arg(e/3600).arg((e%3600)/60);
            else if (e >= 60) eta = QString(" 约%1m%2s").arg(e/60).arg(e%60);
            else eta = QString(" 约%1s").arg(e);
        }
    }

    m_copyStatus->setText(
        QString("[%1] %2/%3  成功%4 失败%5  %6  %7%8")
            .arg(QFileInfo(dst).fileName().isEmpty() ? dst : QFileInfo(dst).fileName())
            .arg(copied + failed).arg(total).arg(copied).arg(failed)
            .arg(formatSize(bytes)).arg(speed).arg(eta));
}

void FileCopierWindow::onCopyFinished(int /*total*/, int success, int failed,
                                       qint64 bytes, const QString& dst)
{
    m_activeCopies--;
    m_totalSuccess += success;
    m_totalFailed  += failed;

    appendLog(QString("  完成 [%1]: 成功 %2, 失败 %3, %4")
                  .arg(QFileInfo(dst).fileName().isEmpty() ? dst : QFileInfo(dst).fileName())
                  .arg(success).arg(failed).arg(formatSize(bytes)));

    if (m_activeCopies <= 0) {
        m_cancelCopyBtn->setVisible(false); m_copyBtn->setVisible(true);
        m_copyBtn->setEnabled(true); m_scanBtn->setEnabled(true);
        m_copyProgress->setVisible(false); m_copyStatus->setText("");
        appendLog("══════════════════════════════");
        appendLog(QString("全部完成! 成功:%1  失败:%2  目标:%3")
                      .arg(m_totalSuccess).arg(m_totalFailed).arg(m_totalCopies));
        if (m_totalFailed == 0) appendLog("✓ 所有文件复制成功");
        updateCopyButtonState();
    }
}

void FileCopierWindow::onFileError(const QString& path, const QString& error)
{
    appendLog(QString("  失败: %1 — %2").arg(QFileInfo(path).fileName()).arg(error));
}

// ═══════════════════════════════════════════════════════════════
// 表格交互
// ═══════════════════════════════════════════════════════════════

void FileCopierWindow::onTableClicked(const QModelIndex& idx)
{
    if (idx.column() == FileListModel::ColAction) {
        QModelIndex src = m_proxy->mapToSource(idx);
        QString p = m_model->filePathAt(src.row());
        if (!p.isEmpty()) openFileLocation(p);
    }
}

void FileCopierWindow::onTableDoubleClicked(const QModelIndex& idx)
{
    QModelIndex src = m_proxy->mapToSource(idx);
    QString p = m_model->filePathAt(src.row());
    if (!p.isEmpty()) openFileLocation(p);
}

void FileCopierWindow::onTableContextMenu(const QPoint& pos)
{
    QModelIndex proxyIdx = m_fileView->indexAt(pos);
    if (!proxyIdx.isValid()) return;
    QModelIndex srcIdx = m_proxy->mapToSource(proxyIdx);
    QString fp = m_model->filePathAt(srcIdx.row());
    if (fp.isEmpty()) return;

    QMenu menu;
    QAction* aOpen  = menu.addAction("📂 打开文件位置");
    menu.addSeparator();
    QAction* aPath  = menu.addAction("📋 复制完整路径");
    QAction* aName  = menu.addAction("📋 复制文件名");
    QAction* chosen = menu.exec(m_fileView->viewport()->mapToGlobal(pos));
    if (chosen == aOpen) {
        openFileLocation(fp);
    } else if (chosen == aPath) {
        QApplication::clipboard()->setText(fp);
        appendLog("已复制: " + fp);
    } else if (chosen == aName) {
        QString n = QFileInfo(fp).fileName();
        QApplication::clipboard()->setText(n);
        appendLog("已复制: " + n);
    }
}

void FileCopierWindow::openFileLocation(const QString& path)
{
    if (!QFileInfo::exists(path)) { appendLog("文件不存在: " + path); return; }
#ifdef Q_OS_WIN
    QProcess::startDetached("explorer", {"/select,", QDir::toNativeSeparators(path)});
#endif
}

// ═══════════════════════════════════════════════════════════════
// 导出
// ═══════════════════════════════════════════════════════════════

void FileCopierWindow::onExport()
{
    int n = m_model->fileCount();
    if (n == 0) { QMessageBox::warning(this, "提示", "无数据"); return; }

    QString fn = QString("文件清单_%1.txt")
                     .arg(QDateTime::currentDateTime().toString("yyyyMMdd_hhmmss"));
    QString path = QFileDialog::getSaveFileName(this, "导出", fn, "文本(*.txt);;CSV(*.csv)");
    if (path.isEmpty()) return;

    QStringList lines; lines.reserve(n + 10);
    bool csv = path.endsWith(".csv", Qt::CaseInsensitive);
    if (csv) {
        lines << "文件路径,字节,格式化";
        for (int i = 0; i < n; ++i) {
            FileEntry e = m_model->entryAt(i);
            lines << QString("\"%1\",%2,\"%3\"")
                         .arg(e.filePath).arg(e.fileSize).arg(formatSize(e.fileSize));
        }
    } else {
        lines << "════════ 文件清单 ════════";
        lines << QString("时间: %1").arg(QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss"));
        lines << QString("数量: %1").arg(QLocale().toString(n));
        lines << QString("大小: %1").arg(formatSize(m_model->totalSize()));
        lines << "════════════════════════";
        for (int i = 0; i < n; ++i) {
            FileEntry e = m_model->entryAt(i);
            lines << QString("%1\t%2").arg(e.filePath).arg(formatSize(e.fileSize));
        }
        lines << "════════════════════════";
    }

    appendLog("导出中...");
    m_pool->enqueue([this, path, lines]() {
        QFile f(path);
        bool ok = f.open(QIODevice::WriteOnly | QIODevice::Text);
        if (ok) { QTextStream s(&f); s.setCodec("UTF-8"); s << lines.join("\n"); f.close(); }
        QString msg = ok ? QString("导出完成: %1").arg(QFileInfo(path).fileName()) : "导出失败";
        QMetaObject::invokeMethod(this, [this, msg, ok]() {
            appendLog(msg);
            if (ok) QMessageBox::information(this, "完成", "文件清单已导出。");
        }, Qt::QueuedConnection);
    });
}

// ═══════════════════════════════════════════════════════════════
// 工具
// ═══════════════════════════════════════════════════════════════

// ═══════════════════════════════════════════════════════════════
// 高级筛选
// ═══════════════════════════════════════════════════════════════

void FileCopierWindow::onMetaFilterApply()
{
    FilterCriteria c;
    c.minWidth  = m_cmbRes->currentData().toInt();
    c.minHeight = m_cmbRes->currentData().toInt();
    c.minDurationMs = m_cmbDur->currentData().toLongLong();
    c.codec     = m_cmbCodec->currentData().toString();
    c.matchAll  = true;
    m_proxy->setCriteria(c);
}

void FileCopierWindow::onMetaFilterClear()
{
    m_cmbRes->setCurrentIndex(0);
    m_cmbDur->setCurrentIndex(0);
    m_cmbCodec->setCurrentIndex(0);
    m_proxy->clearCriteria();
}

void FileCopierWindow::onFilterPresetLoad(int index)
{
    if (index <= 0) return; // (无)
    QString name = m_cmbPreset->currentText();
    QSettings s("MediaTool", "FileCopier");
    s.beginGroup("FilterPresets/" + name);
    m_cmbRes->setCurrentIndex(s.value("resIdx", 0).toInt());
    m_cmbDur->setCurrentIndex(s.value("durIdx", 0).toInt());
    m_cmbCodec->setCurrentIndex(s.value("codecIdx", 0).toInt());
    s.endGroup();
    onMetaFilterApply();
}

void FileCopierWindow::onFilterPresetSave()
{
    QString name = m_cmbPreset->currentText();
    if (name.isEmpty() || name == "(无)") {
        // 弹出输入框
        name = QInputDialog::getText(this, "保存筛选预设", "预设名称:");
        if (name.isEmpty()) return;
    }
    QSettings s("MediaTool", "FileCopier");
    s.beginGroup("FilterPresets/" + name);
    s.setValue("resIdx",   m_cmbRes->currentIndex());
    s.setValue("durIdx",   m_cmbDur->currentIndex());
    s.setValue("codecIdx", m_cmbCodec->currentIndex());
    s.endGroup();
    refreshPresetCombo();
    m_cmbPreset->setCurrentText(name);
    appendLog("筛选预设已保存: " + name);
}

void FileCopierWindow::onFilterPresetDelete()
{
    QString name = m_cmbPreset->currentText();
    if (name.isEmpty() || name == "(无)") return;
    QSettings s("MediaTool", "FileCopier");
    s.remove("FilterPresets/" + name);
    refreshPresetCombo();
    appendLog("筛选预设已删除: " + name);
}

void FileCopierWindow::refreshPresetCombo()
{
    m_cmbPreset->blockSignals(true);
    m_cmbPreset->clear();
    m_cmbPreset->addItem("(无)");
    QSettings s("MediaTool", "FileCopier");
    s.beginGroup("FilterPresets");
    for (const QString& k : s.childGroups())
        m_cmbPreset->addItem(k);
    s.endGroup();
    m_cmbPreset->blockSignals(false);
}

// ═══════════════════════════════════════════════════════════════
// 方案管理
// ═══════════════════════════════════════════════════════════════

QString FileCopierWindow::currentScheme() const
{
    QString name = m_cmbScheme->currentText();
    return (name.isEmpty() || name == "(默认)") ? QString() : name;
}

void FileCopierWindow::onSchemeLoad(int index)
{
    if (index < 0) return;
    QString name = m_cmbScheme->currentText();
    if (name == "(默认)") name.clear();
    loadScheme(name);
}

void FileCopierWindow::onSchemeSave()
{
    QString name = QInputDialog::getText(this, "保存方案",
                                          "方案名称 (例如: 工作盘备份):");
    if (name.isEmpty()) return;
    saveScheme(name);
    refreshSchemeCombo();
    m_cmbScheme->setCurrentText(name);
    appendLog("方案已保存: " + name);
}

void FileCopierWindow::onSchemeDelete()
{
    QString name = m_cmbScheme->currentText();
    if (name.isEmpty() || name == "(默认)") return;
    QSettings s("MediaTool", "FileCopier");
    s.remove("Schemes/" + name);
    refreshSchemeCombo();
    appendLog("方案已删除: " + name);
}

void FileCopierWindow::refreshSchemeCombo()
{
    m_cmbScheme->blockSignals(true);
    m_cmbScheme->clear();
    m_cmbScheme->addItem("(默认)");
    QSettings s("MediaTool", "FileCopier");
    s.beginGroup("Schemes");
    for (const QString& k : s.childGroups())
        m_cmbScheme->addItem(k);
    s.endGroup();
    m_cmbScheme->blockSignals(false);
}

void FileCopierWindow::loadScheme(const QString& name)
{
    QSettings s("MediaTool", "FileCopier");
    QString prefix = name.isEmpty() ? QString() : "Schemes/" + name + "/";

    // 清空当前
    m_sourceList->clear();
    m_destList->clear();

    // 源
    int nSrc = s.beginReadArray(prefix + "Sources");
    for (int i = 0; i < nSrc; ++i) {
        s.setArrayIndex(i);
        QString p = s.value("path").toString();
        bool ck   = s.value("checked", true).toBool();
        if (p.isEmpty() || !QDir(p).exists()) continue;
        QListWidgetItem* item = new QListWidgetItem(p);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(ck ? Qt::Checked : Qt::Unchecked);
        item->setData(Qt::UserRole, p); item->setToolTip(p);
        m_sourceList->addItem(item);
    }
    s.endArray();

    // 目标
    int nDst = s.beginReadArray(prefix + "Destinations");
    for (int i = 0; i < nDst; ++i) {
        s.setArrayIndex(i);
        QString p = s.value("path").toString();
        if (p.isEmpty() || !QDir(p).exists()) continue;
        DiskInfo info = DiskDetector::detectForPath(p);
        QListWidgetItem* item = new QListWidgetItem(
            QString("%1  [%2]").arg(p).arg(info.typeLabel()));
        item->setData(Qt::UserRole, p);
        item->setToolTip(QString("卷标:%1 | 可用:%2 | 策略:%3线程")
                             .arg(info.volumeName).arg(formatSize(info.freeBytes))
                             .arg(info.recommendedConcurrency()));
        m_destList->addItem(item);
    }
    s.endArray();

    // 选项
    m_skipExistingCb->setChecked(s.value(prefix + "SkipExisting", false).toBool());
    m_preserveTsCb->setChecked(s.value(prefix + "PreserveTimestamp", true).toBool());
    QString flt = s.value(prefix + "Filter").toString();
    QLineEdit* fe = findChild<QLineEdit*>();
    if (fe && !flt.isEmpty()) { fe->setText(flt); m_scanner->setFilters(
        flt.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts)); }

    // 高级筛选
    m_cmbRes->setCurrentIndex(s.value(prefix + "MetaResIdx", 0).toInt());
    m_cmbDur->setCurrentIndex(s.value(prefix + "MetaDurIdx", 0).toInt());
    m_cmbCodec->setCurrentIndex(s.value(prefix + "MetaCodecIdx", 0).toInt());
    onMetaFilterApply();

    // 类型筛选
    m_typeFilterCombo->setCurrentIndex(s.value(prefix + "TypeFilterIdx", 0).toInt());

    updateCopyButtonState();
    if (!name.isEmpty()) appendLog("方案已加载: " + name);
}

void FileCopierWindow::saveScheme(const QString& name)
{
    QSettings s("MediaTool", "FileCopier");
    QString prefix = "Schemes/" + name + "/";

    s.beginWriteArray(prefix + "Sources");
    for (int i = 0; i < m_sourceList->count(); ++i) {
        s.setArrayIndex(i);
        auto* item = m_sourceList->item(i);
        s.setValue("path", item->data(Qt::UserRole).toString());
        s.setValue("checked", item->checkState() == Qt::Checked);
    }
    s.endArray();

    s.beginWriteArray(prefix + "Destinations");
    for (int i = 0; i < m_destList->count(); ++i) {
        s.setArrayIndex(i);
        s.setValue("path", m_destList->item(i)->data(Qt::UserRole).toString());
    }
    s.endArray();

    s.setValue(prefix + "SkipExisting",       m_skipExistingCb->isChecked());
    s.setValue(prefix + "PreserveTimestamp",  m_preserveTsCb->isChecked());
    s.setValue(prefix + "MetaResIdx",         m_cmbRes->currentIndex());
    s.setValue(prefix + "MetaDurIdx",         m_cmbDur->currentIndex());
    s.setValue(prefix + "MetaCodecIdx",       m_cmbCodec->currentIndex());
    s.setValue(prefix + "TypeFilterIdx",      m_typeFilterCombo->currentIndex());
    QLineEdit* fe = findChild<QLineEdit*>();
    if (fe) s.setValue(prefix + "Filter", fe->text());
}

void FileCopierWindow::appendLog(const QString& msg)
{
    QString t = QDateTime::currentDateTime().toString("hh:mm:ss");
    m_logEdit->append(QString("[%1] %2").arg(t).arg(msg));
    QTextCursor c = m_logEdit->textCursor();
    c.movePosition(QTextCursor::End); m_logEdit->setTextCursor(c);
}

void FileCopierWindow::updateCopyButtonState()
{
    bool hasFiles = m_model->fileCount() > 0;
    bool hasDest  = m_destList->count() > 0;
    m_copyBtn->setEnabled(hasFiles && hasDest && !m_scanning && !m_extractingMeta && m_activeCopies == 0);
}

QString FileCopierWindow::formatSize(qint64 bytes) const
{
    if (bytes < 1024)             return QString::number(bytes) + " B";
    if (bytes < 1048576)          return QString::number(bytes / 1024.0, 'f', 1) + " KB";
    if (bytes < 1073741824LL)     return QString::number(bytes / 1048576.0, 'f', 2) + " MB";
    return QString::number(bytes / 1073741824.0, 'f', 2) + " GB";
}
