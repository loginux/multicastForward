#include "gui/mainwindow.h"

#include "client/multicastreceiver.h"
#include "client/tunnelclient.h"
#include "common/log.h"
#include "common/protocol.h"
#include "gitinfo.h"
#include "gui/updateclient.h"
#include "server/multicastsender.h"
#include "server/tunnelserver.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QCoreApplication>
#include <QDateTime>
#include <QCryptographicHash>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QNetworkInterface>
#include <QPlainTextEdit>
#include <QProgressDialog>
#include <QPushButton>
#include <QRadioButton>
#include <QSpinBox>
#include <QStatusBar>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>

namespace mc {

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    buildUi();

    // 日志同时输出到窗口
    Log::setSink([this](int level, const QString &line) { appendLog(level, line); });
    Log::init(QString(), LogLevel::Info);

    logI(QStringLiteral("组播转发 MulticastForward ") + QStringLiteral(MCFW_VERSION_FULL));

    const QString path = defaultConfigPath();
    m_lbConfigPath->setText(QStringLiteral("配置文件: %1").arg(path));

    Config cfg;
    QString err;
    if (QFileInfo::exists(path) && Config::loadAuto(path, &cfg, &err)) {
        loadFromConfig(cfg);
        logI(QStringLiteral("运行角色: %1, 组播 %2:%3")
                 .arg(cfg.role == Config::Server ? QStringLiteral("服务端") : QStringLiteral("客户端"))
                 .arg(cfg.group).arg(cfg.mcPort));
        if (!cfg.logFile.isEmpty())
            Log::init(cfg.logFile, cfg.logLevel);
        Log::setLevel(cfg.logLevel);
        logI(QStringLiteral("已自动加载配置: %1").arg(path));

        QString verr;
        if (!cfg.validate(&verr))
            logW(QStringLiteral("配置有待修正（启动前必须解决）: %1").arg(verr));
    } else {
        if (!err.isEmpty())
            logW(QStringLiteral("加载配置失败，使用默认值: %1").arg(err));

        // 交付目录里带 client.ini / server.ini 两份模板，提示直接导入
        const QString dir = QCoreApplication::applicationDirPath();
        QString tpl;
        for (const char *name : {"client.ini", "server.ini"}) {
            if (QFileInfo::exists(dir + QLatin1Char('/') + QLatin1String(name))) {
                if (!tpl.isEmpty())
                    tpl += QStringLiteral(" / ");
                tpl += QString::fromLatin1(name);
            }
        }
        if (!tpl.isEmpty()) {
            logI(QStringLiteral("未找到 config.ini，可使用【文件 → 导入配置】载入 %1（导入后自动保存）")
                     .arg(tpl));
        }

        Config def;
        def.role = Config::Client;
        def.group = QStringLiteral("239.255.10.128");
        def.mcPort = 9302;
        def.ttl = 1;
        def.loop = true;
        def.remoteHost = QStringLiteral("172.16.5.213");
        def.remotePort = 25000;
        def.listenHost = QStringLiteral("0.0.0.0");
        def.listenPort = 25000;
        def.password = QByteArray("change-me");
        loadFromConfig(def);
        logI(QStringLiteral("未找到配置文件，已载入默认值"));
    }

    m_statsTimer->start(1000);
    setRunning(false, QStringLiteral("未运行"));
}

MainWindow::~MainWindow()
{
    Log::setSink(LogSink());
    stopEngine();
}

void MainWindow::buildUi()
{
    setWindowTitle(QStringLiteral("组播转发 MulticastForward ") + QStringLiteral(MCFW_VERSION_FULL));
    resize(1000, 780);

    auto *central = new QWidget(this);
    auto *rootH = new QHBoxLayout(central);
    rootH->setContentsMargins(12, 12, 12, 12);
    rootH->setSpacing(12);

    auto *leftV = new QVBoxLayout();
    leftV->setSpacing(10);
    auto *rightV = new QVBoxLayout();
    rightV->setSpacing(10);

    // ---------- 角色 ----------
    auto *roleBox = new QGroupBox(QStringLiteral("运行角色"), central);
    auto *roleLay = new QHBoxLayout(roleBox);
    m_roleClient = new QRadioButton(QStringLiteral("客户端（收组播 → 转发）"), roleBox);
    m_roleServer = new QRadioButton(QStringLiteral("服务端（接收 → 组播发出）"), roleBox);
    m_roleClient->setChecked(true);
    roleLay->addWidget(m_roleClient);
    roleLay->addWidget(m_roleServer);
    roleLay->addStretch();
    leftV->addWidget(roleBox);

    // ---------- 组播 ----------
    auto *mcBox = new QGroupBox(QStringLiteral("组播参数"), central);
    auto *mcForm = new QFormLayout(mcBox);
    m_edGroup = new QLineEdit(mcBox);
    m_edGroup->setPlaceholderText(QStringLiteral("例如 239.255.10.129"));

    m_spMcPort = new QSpinBox(mcBox);
    m_spMcPort->setRange(1, 65535);
    m_spMcPort->setValue(9302);

    m_cbIface = new QComboBox(mcBox);
    m_cbIface->setMinimumWidth(300);
    auto *ifaceRow = new QHBoxLayout();
    ifaceRow->addWidget(m_cbIface, 1);
    auto *btnRefresh = new QPushButton(QStringLiteral("刷新"), mcBox);
    btnRefresh->setFixedWidth(60);
    ifaceRow->addWidget(btnRefresh);

    m_spTtl = new QSpinBox(mcBox);
    m_spTtl->setRange(1, 255);
    m_spTtl->setValue(1);

    m_chkLoop = new QCheckBox(QStringLiteral("允许组播回环（同机调试用）"), mcBox);
    m_chkLoop->setChecked(true);

    mcForm->addRow(QStringLiteral("组播地址"), m_edGroup);
    mcForm->addRow(QStringLiteral("端口"), m_spMcPort);
    mcForm->addRow(QStringLiteral("网卡"), ifaceRow);
    mcForm->addRow(QStringLiteral("TTL"), m_spTtl);
    mcForm->addRow(QString(), m_chkLoop);
    leftV->addWidget(mcBox);

    // ---------- 客户端连接 ----------
    m_boxClient = new QGroupBox(QStringLiteral("服务端连接（客户端）"), central);
    auto *cf = new QFormLayout(m_boxClient);
    m_edServerHost = new QLineEdit(m_boxClient);
    m_edServerHost->setPlaceholderText(QStringLiteral("例如 172.16.5.213"));
    m_spServerPort = new QSpinBox(m_boxClient);
    m_spServerPort->setRange(1, 65535);
    m_spServerPort->setValue(25000);
    cf->addRow(QStringLiteral("服务端地址"), m_edServerHost);
    cf->addRow(QStringLiteral("端口"), m_spServerPort);
    leftV->addWidget(m_boxClient);

    // ---------- 服务端监听 ----------
    m_boxServer = new QGroupBox(QStringLiteral("监听（服务端）"), central);
    auto *sf = new QFormLayout(m_boxServer);
    m_edListenHost = new QLineEdit(m_boxServer);
    m_edListenHost->setText(QStringLiteral("0.0.0.0"));
    m_spListenPort = new QSpinBox(m_boxServer);
    m_spListenPort->setRange(1, 65535);
    m_spListenPort->setValue(25000);
    sf->addRow(QStringLiteral("监听地址"), m_edListenHost);
    sf->addRow(QStringLiteral("端口"), m_spListenPort);
    leftV->addWidget(m_boxServer);

    // ---------- 认证 ----------
    auto *authBox = new QGroupBox(QStringLiteral("认证"), central);
    auto *af = new QFormLayout(authBox);
    m_edPassword = new QLineEdit(authBox);
    m_edPassword->setEchoMode(QLineEdit::Password);
    m_chkShowPassword = new QCheckBox(QStringLiteral("显示"), authBox);
    auto *pwRow = new QHBoxLayout();
    pwRow->addWidget(m_edPassword, 1);
    pwRow->addWidget(m_chkShowPassword);
    af->addRow(QStringLiteral("口令"), pwRow);
    leftV->addWidget(authBox);

    // ---------- 运行参数 ----------
    auto *rtBox = new QGroupBox(QStringLiteral("运行参数"), central);
    auto *rt = new QFormLayout(rtBox);
    m_spQueue = new QSpinBox(rtBox);
    m_spQueue->setRange(16, 65536);
    m_spQueue->setValue(1024);
    m_spReMin = new QSpinBox(rtBox);
    m_spReMin->setRange(1, 600);
    m_spReMin->setValue(1);
    m_spReMax = new QSpinBox(rtBox);
    m_spReMax->setRange(1, 3600);
    m_spReMax->setValue(30);
    m_spHbInterval = new QSpinBox(rtBox);
    m_spHbInterval->setRange(1, 600);
    m_spHbInterval->setValue(10);
    m_spHbTimeout = new QSpinBox(rtBox);
    m_spHbTimeout->setRange(1, 3600);
    m_spHbTimeout->setValue(30);
    m_cbLogLevel = new QComboBox(rtBox);
    m_cbLogLevel->addItem(QStringLiteral("error"), int(LogLevel::Error));
    m_cbLogLevel->addItem(QStringLiteral("warn"), int(LogLevel::Warn));
    m_cbLogLevel->addItem(QStringLiteral("info"), int(LogLevel::Info));
    m_cbLogLevel->addItem(QStringLiteral("debug"), int(LogLevel::Debug));
    m_cbLogLevel->setCurrentIndex(2);

    auto *qRow = new QHBoxLayout();
    qRow->addWidget(m_spQueue);
    rt->addRow(QStringLiteral("发送队列(包)"), qRow);

    auto *reRow = new QHBoxLayout();
    reRow->addWidget(new QLabel(QStringLiteral("最小"), rtBox));
    reRow->addWidget(m_spReMin);
    reRow->addWidget(new QLabel(QStringLiteral("秒   最大"), rtBox));
    reRow->addWidget(m_spReMax);
    reRow->addWidget(new QLabel(QStringLiteral("秒"), rtBox));
    rt->addRow(QStringLiteral("重连间隔"), reRow);

    auto *hbRow = new QHBoxLayout();
    hbRow->addWidget(new QLabel(QStringLiteral("间隔"), rtBox));
    hbRow->addWidget(m_spHbInterval);
    hbRow->addWidget(new QLabel(QStringLiteral("秒   超时"), rtBox));
    hbRow->addWidget(m_spHbTimeout);
    hbRow->addWidget(new QLabel(QStringLiteral("秒"), rtBox));
    rt->addRow(QStringLiteral("心跳"), hbRow);

    rt->addRow(QStringLiteral("日志级别"), m_cbLogLevel);
    leftV->addWidget(rtBox);

    // ---------- 按钮 ----------
    auto *btnRow = new QHBoxLayout();
    m_btnStart = new QPushButton(QStringLiteral("启动"), central);
    m_btnStart->setMinimumHeight(38);
    m_btnStart->setStyleSheet(QStringLiteral(
        "QPushButton{background:#1a7f4b;color:#fff;border:none;border-radius:6px;font-size:14px;}"
        "QPushButton:hover{background:#15683d;}"));
    auto *btnImport = new QPushButton(QStringLiteral("导入配置…"), central);
    auto *btnExport = new QPushButton(QStringLiteral("导出配置…"), central);
    btnImport->setMinimumHeight(38);
    btnExport->setMinimumHeight(38);
    btnRow->addWidget(m_btnStart, 2);
    btnRow->addWidget(btnImport, 1);
    btnRow->addWidget(btnExport, 1);
    leftV->addLayout(btnRow);

    leftV->addStretch();

    // 配置文件路径改到状态栏显示，字号正常、可选中复制
    statusBar()->setSizeGripEnabled(true);
    m_lbConfigPath = new QLabel(this);
    // 字号跟随系统默认字体，避免高 DPI 下固定 px 显得很小
    const int statusPt = qMax(8, QApplication::font().pointSize() - 1);
    m_lbConfigPath->setStyleSheet(
        QStringLiteral("color:#666;font-size:%1pt;padding-left:4px;").arg(statusPt));
    m_lbConfigPath->setTextInteractionFlags(Qt::TextSelectableByMouse);
    statusBar()->addWidget(m_lbConfigPath, 1);

    // 右侧常驻版本号，方便现场确认装的是哪一版
    auto *lbVersion = new QLabel(QStringLiteral(MCFW_VERSION_FULL), this);
    lbVersion->setStyleSheet(
        QStringLiteral("color:#888;font-size:%1pt;padding-right:6px;").arg(statusPt));
    statusBar()->addPermanentWidget(lbVersion);

    // ---------- 右侧：状态 + 日志 ----------
    auto *stBox = new QGroupBox(QStringLiteral("状态"), central);
    auto *stLay = new QVBoxLayout(stBox);
    m_lbStatus = new QLabel(QStringLiteral("未运行"), stBox);
    m_lbStatus->setStyleSheet(QStringLiteral("font-size:18px;font-weight:600;color:#555;"));

    // 进出口流量表：方向 / 包数 / 字节数 / 速率
    // 进出口流量表：方向 / 包数 / 字节数 / 包每秒 / 字节每秒
    m_tblStats = new QTableWidget(2, 5, stBox);
    m_tblStats->setHorizontalHeaderLabels(
        {QStringLiteral("方向"), QStringLiteral("包数"), QStringLiteral("字节数"),
         QStringLiteral("包/秒"), QStringLiteral("字节/秒")});
    m_tblStats->verticalHeader()->setVisible(false);
    m_tblStats->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_tblStats->setSelectionMode(QAbstractItemView::NoSelection);
    m_tblStats->setFocusPolicy(Qt::NoFocus);
    m_tblStats->setAlternatingRowColors(true);
    m_tblStats->setShowGrid(false);
    m_tblStats->setMaximumHeight(108);
    m_tblStats->verticalHeader()->setDefaultSectionSize(32);
    m_tblStats->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_tblStats->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Fixed);
    m_tblStats->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Fixed);
    m_tblStats->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Fixed);
    m_tblStats->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Stretch);
    m_tblStats->setColumnWidth(1, 74);
    m_tblStats->setColumnWidth(2, 92);
    m_tblStats->setColumnWidth(3, 74);
    m_tblStats->horizontalHeader()->setHighlightSections(false);
    for (int c = 1; c <= 3; ++c)
        m_tblStats->horizontalHeaderItem(c)->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_tblStats->horizontalHeaderItem(4)->setTextAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    m_tblStats->setStyleSheet(QStringLiteral(
        "QTableWidget{font-size:13px;border:1px solid #ddd;background:#fff;}"
        "QTableWidget::item{padding:2px 8px;}"
        "QHeaderView::section{background:#f4f5f7;border:none;border-bottom:1px solid #ddd;"
        "padding:4px 8px;font-size:12px;color:#555;}"));
    for (int r = 0; r < 2; ++r) {
        for (int c = 0; c < 5; ++c) {
            auto *it = new QTableWidgetItem(QStringLiteral("0"));
            if (c >= 1 && c <= 3)
                it->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
            else
                it->setTextAlignment(Qt::AlignLeft | Qt::AlignVCenter);
            m_tblStats->setItem(r, c, it);
        }
    }
    m_tblStats->item(0, 0)->setText(QStringLiteral("进口"));
    m_tblStats->item(1, 0)->setText(QStringLiteral("出口"));

    stLay->addWidget(m_lbStatus);
    stLay->addWidget(m_tblStats);
    rightV->addWidget(stBox);

    auto *logBox = new QGroupBox(QStringLiteral("运行日志"), central);
    auto *logLay = new QVBoxLayout(logBox);
    m_log = new QPlainTextEdit(logBox);
    m_log->setReadOnly(true);
    m_log->setMaximumBlockCount(3000);
    m_log->setStyleSheet(QStringLiteral(
        "QPlainTextEdit{font-family:Consolas,'Courier New',monospace;font-size:12px;"
        "background:#fbfbfb;border:1px solid #ddd;}"));
    logLay->addWidget(m_log);
    rightV->addWidget(logBox, 1);

    rootH->addLayout(leftV, 0);
    rootH->addLayout(rightV, 1);
    setCentralWidget(central);

    m_configWidgets << roleBox << mcBox << m_boxClient << m_boxServer
                    << authBox << rtBox << btnImport << btnExport;

    buildMenus();
    m_statsTimer = new QTimer(this);

    connect(m_actImport, &QAction::triggered, this, &MainWindow::onImportConfig);
    connect(m_actExport, &QAction::triggered, this, &MainWindow::onExportConfig);
    connect(m_roleClient, &QRadioButton::toggled, this, &MainWindow::onRoleChanged);
    connect(m_btnStart, &QPushButton::clicked, this, &MainWindow::onStartClicked);
    connect(btnImport, &QPushButton::clicked, this, &MainWindow::onImportConfig);
    connect(btnExport, &QPushButton::clicked, this, &MainWindow::onExportConfig);
    connect(btnRefresh, &QPushButton::clicked, this, &MainWindow::onRefreshInterfaces);
    connect(m_chkShowPassword, &QCheckBox::toggled, this, [this](bool on) {
        m_edPassword->setEchoMode(on ? QLineEdit::Normal : QLineEdit::Password);
    });
    // 日志级别改了就立刻生效，不必等到点启动
    connect(m_cbLogLevel, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) {
        Log::setLevel(LogLevel(m_cbLogLevel->currentData().toInt()));
    });
    connect(m_statsTimer, &QTimer::timeout, this, &MainWindow::refreshStats);

    onRoleChanged();
}

void MainWindow::buildMenus()
{
    QMenu *fileMenu = menuBar()->addMenu(QStringLiteral("文件(&F)"));

    m_actImport = fileMenu->addAction(QStringLiteral("导入配置(&I)…"));
    m_actImport->setShortcut(QKeySequence::Open);
    m_actImport->setStatusTip(QStringLiteral("从 INI 文件读取参数并填入界面"));

    m_actExport = fileMenu->addAction(QStringLiteral("导出配置(&E)…"));
    m_actExport->setShortcut(QKeySequence::Save);
    m_actExport->setStatusTip(QStringLiteral("把当前界面参数保存为 INI 文件"));

    fileMenu->addSeparator();
    QAction *quit = fileMenu->addAction(QStringLiteral("退出(&Q)"));
    quit->setShortcut(QKeySequence::Quit);
    connect(quit, &QAction::triggered, this, &MainWindow::close);

    QMenu *toolMenu = menuBar()->addMenu(QStringLiteral("工具(&T)"));
    m_actRemoteUpdate = toolMenu->addAction(QStringLiteral("远程更新服务端…"));
    m_actRemoteUpdate->setStatusTip(QStringLiteral("把本地 exe 推送到服务端，校验通过后由启动器替换并重启"));
    connect(m_actRemoteUpdate, &QAction::triggered, this, &MainWindow::onRemoteUpdate);

    QMenu *helpMenu = menuBar()->addMenu(QStringLiteral("帮助(&H)"));
    QAction *about = helpMenu->addAction(QStringLiteral("关于…"));
    connect(about, &QAction::triggered, this, [this]() {
        QMessageBox::about(this, QStringLiteral("关于"),
                           QStringLiteral("<b>组播转发 Multicast Forward</b> ") + QStringLiteral(MCFW_VERSION_FULL) + QStringLiteral("<br><br>"
                                          "把一侧网段的 UDP 组播经 TCP 隧道搬到另一侧重新发出。<br>"
                                          "协议版本 0x01，HMAC-SHA256 挑战应答认证。"));
    });
}

void MainWindow::onRoleChanged()
{
    const bool client = m_roleClient->isChecked();
    m_boxClient->setVisible(client);
    m_boxServer->setVisible(!client);
}

void MainWindow::populateInterfaces(const QString &preferred)
{
    const QString keep = preferred.isEmpty() ? m_cbIface->currentData().toString() : preferred;

    m_cbIface->clear();
    for (const auto &ni : QNetworkInterface::allInterfaces()) {
        if (!(ni.flags() & QNetworkInterface::IsUp))
            continue;
        for (const auto &e : ni.addressEntries()) {
            if (e.ip().protocol() != QAbstractSocket::IPv4Protocol)
                continue;
            m_cbIface->addItem(QStringLiteral("%1  (%2)").arg(e.ip().toString(), ni.humanReadableName()),
                               e.ip().toString());
        }
    }

    int idx = keep.isEmpty() ? -1 : m_cbIface->findData(keep);
    if (idx < 0 && !keep.isEmpty()) {
        // 配置里的网卡本机不存在，仍然显示出来，便于用户发现配置不对
        m_cbIface->addItem(QStringLiteral("%1  (本机不存在)").arg(keep), keep);
        idx = m_cbIface->count() - 1;
    }
    if (idx >= 0)
        m_cbIface->setCurrentIndex(idx);
}

void MainWindow::loadFromConfig(const Config &c)
{
    m_roleClient->setChecked(c.role == Config::Client);
    m_roleServer->setChecked(c.role == Config::Server);

    m_edGroup->setText(c.group);
    m_spMcPort->setValue(int(c.mcPort));
    populateInterfaces(c.ifaceAddr);
    m_spTtl->setValue(int(c.ttl));
    m_chkLoop->setChecked(c.loop);

    m_edServerHost->setText(c.remoteHost);
    m_spServerPort->setValue(int(c.remotePort));

    m_edListenHost->setText(c.listenHost);
    m_spListenPort->setValue(int(c.listenPort));

    m_edPassword->setText(QString::fromUtf8(c.password));

    m_spQueue->setValue(c.queueSize);
    m_spReMin->setValue(c.reconnectMinSec);
    m_spReMax->setValue(c.reconnectMaxSec);
    m_spHbInterval->setValue(c.heartbeatIntervalSec);
    m_spHbTimeout->setValue(c.heartbeatTimeoutSec);

    const int lvIdx = m_cbLogLevel->findData(int(c.logLevel));
    if (lvIdx >= 0)
        m_cbLogLevel->setCurrentIndex(lvIdx);

    m_statsIntervalSec = c.statsIntervalSec;
    m_logFile = c.logFile;

    onRoleChanged();
}

Config MainWindow::collectConfig() const
{
    Config c;
    c.role = m_roleServer->isChecked() ? Config::Server : Config::Client;

    c.group = m_edGroup->text().trimmed();
    c.mcPort = quint16(m_spMcPort->value());
    c.ifaceAddr = m_cbIface->currentData().toString();
    c.ttl = quint8(m_spTtl->value());
    c.loop = m_chkLoop->isChecked();

    c.remoteHost = m_edServerHost->text().trimmed();
    c.remotePort = quint16(m_spServerPort->value());

    c.listenHost = m_edListenHost->text().trimmed();
    if (c.listenHost.isEmpty())
        c.listenHost = QStringLiteral("0.0.0.0");
    c.listenPort = quint16(m_spListenPort->value());

    c.password = m_edPassword->text().toUtf8();

    c.queueSize = m_spQueue->value();
    c.reconnectMinSec = m_spReMin->value();
    c.reconnectMaxSec = m_spReMax->value();
    c.heartbeatIntervalSec = m_spHbInterval->value();
    c.heartbeatTimeoutSec = m_spHbTimeout->value();

    c.statsIntervalSec = m_statsIntervalSec;
    c.logFile = m_logFile;
    c.logLevel = LogLevel(m_cbLogLevel->currentData().toInt());
    return c;
}

QString MainWindow::defaultConfigPath() const
{
    return QCoreApplication::applicationDirPath() + QStringLiteral("/config.ini");
}

void MainWindow::saveConfig()
{
    const Config c = collectConfig();
    QString err;
    if (!c.save(defaultConfigPath(), &err))
        logW(QStringLiteral("保存配置失败: %1").arg(err));
    else
        logD(QStringLiteral("配置已保存: %1").arg(defaultConfigPath()));
}

void MainWindow::onRefreshInterfaces()
{
    populateInterfaces(QString());
}

void MainWindow::onStartClicked()
{
    if (m_running) {
        stopEngine();
        setRunning(false, QStringLiteral("已停止"));
        return;
    }

    Config c = collectConfig();
    QString err;
    if (!c.validate(&err)) {
        QMessageBox::warning(this, QStringLiteral("配置有问题"), err);
        return;
    }

    saveConfig();
    Log::setLevel(c.logLevel);
    onRoleChanged();

    const bool ok = (c.role == Config::Client) ? startClient(c, &err) : startServer(c, &err);
    if (!ok) {
        stopEngine();
        setRunning(false, QStringLiteral("启动失败"));
        QMessageBox::critical(this, QStringLiteral("启动失败"), err);
        return;
    }

    setRunning(true, QStringLiteral("运行中"));
}

bool MainWindow::startClient(const Config &c, QString *err)
{
    m_queue = new TxQueue(c.queueSize);
    m_rx = new MulticastReceiver(c, m_queue, this);
    if (!m_rx->open(err))
        return false;

    m_client = new TunnelClient(c, m_queue, this);
    connect(m_rx, &MulticastReceiver::packetQueued, m_client, &TunnelClient::pump);
    m_client->start();
    return true;
}

bool MainWindow::startServer(const Config &c, QString *err)
{
    m_sender = new MulticastSender(c);
    if (!m_sender->open(err))
        return false;

    m_server = new TunnelServer(c, m_sender, this);
    if (!m_server->listen(err))
        return false;
    return true;
}

void MainWindow::stopEngine()
{
    if (m_client) {
        m_client->stop();
        delete m_client;
        m_client = nullptr;
    }
    if (m_server) {
        delete m_server;
        m_server = nullptr;
    }
    delete m_sender;
    m_sender = nullptr;

    if (m_rx) {
        delete m_rx;
        m_rx = nullptr;
    }
    delete m_queue;
    m_queue = nullptr;
}

void MainWindow::setRunning(bool running, const QString &statusText)
{
    m_running = running;

    for (QWidget *w : m_configWidgets)
        w->setEnabled(!running);
    if (m_actImport)
        m_actImport->setEnabled(!running);
    if (m_actExport)
        m_actExport->setEnabled(!running);
    if (m_actRemoteUpdate)
        m_actRemoteUpdate->setEnabled(!running);

    m_btnStart->setText(running ? QStringLiteral("停止") : QStringLiteral("启动"));
    m_btnStart->setStyleSheet(running
        ? QStringLiteral("QPushButton{background:#a33;color:#fff;border:none;border-radius:6px;font-size:14px;}"
                         "QPushButton:hover{background:#8a2b2b;}")
        : QStringLiteral("QPushButton{background:#1a7f4b;color:#fff;border:none;border-radius:6px;font-size:14px;}"
                         "QPushButton:hover{background:#15683d;}"));

    m_lbStatus->setText(statusText);
    m_lbStatus->setStyleSheet(running
        ? QStringLiteral("font-size:18px;font-weight:600;color:#1a7f4b;")
        : QStringLiteral("font-size:18px;font-weight:600;color:#555;"));

    if (!running) {
        m_lbStatus->setText(statusText);
        resetRateBase();
        refreshStats(); // 停止后把表格归零
    } else {
        resetRateBase();
    }
}

void MainWindow::refreshStats()
{
    // 列名与单位按当前角色决定
    const bool server = m_roleServer->isChecked();
    const QString inLabel  = server ? QStringLiteral("进口（隧道收到）") : QStringLiteral("进口（组播收包）");
    const QString outLabel = server ? QStringLiteral("出口（组播发出）") : QStringLiteral("出口（隧道转发）");

    quint64 inCnt = 0, inBytes = 0, outCnt = 0, outBytes = 0;

    if (m_running && m_client) {
        const Stats s = m_client->stats();
        const Stats r = m_rx ? m_rx->stats() : Stats();
        inCnt = r.rxPackets;
        inBytes = r.rxBytes;
        outCnt = s.txFrames;
        outBytes = s.txBytes;

        m_lbStatus->setText(m_client->isRunning() ? QStringLiteral("运行中 · 已连接")
                                                  : QStringLiteral("运行中 · 未连接（重连中）"));
        m_lbStatus->setStyleSheet(m_client->isRunning()
            ? QStringLiteral("font-size:18px;font-weight:600;color:#1a7f4b;")
            : QStringLiteral("font-size:18px;font-weight:600;color:#b8860b;"));
    } else if (m_running && m_server) {
        const Stats s = m_server->stats();
        inCnt = s.framesIn;
        inBytes = s.framesInBytes;
        outCnt = s.mcSent;
        outBytes = s.mcSentBytes;

        m_lbStatus->setText(s.connected ? QStringLiteral("运行中 · 客户端已连接")
                                        : QStringLiteral("运行中 · 等待客户端"));
        m_lbStatus->setStyleSheet(s.connected
            ? QStringLiteral("font-size:18px;font-weight:600;color:#1a7f4b;")
            : QStringLiteral("font-size:18px;font-weight:600;color:#b8860b;"));
    }

    updateStatsTable(inCnt, inBytes, outCnt, outBytes, inLabel, outLabel);
}

void MainWindow::updateStatsTable(quint64 inCount, quint64 inBytes, quint64 outCount,
                                  quint64 outBytes, const QString &inLabel,
                                  const QString &outLabel)
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    double dt = 1.0;
    if (m_prevStampMs > 0) {
        dt = double(now - m_prevStampMs) / 1000.0;
        if (dt <= 0.0 || dt > 10.0)
            dt = 1.0;
    }

    const double inPps  = double(inCount - m_prevInCount) / dt;
    const double inBps  = double(inBytes - m_prevInBytes) / dt;
    const double outPps = double(outCount - m_prevOutCount) / dt;
    const double outBps = double(outBytes - m_prevOutBytes) / dt;

    m_prevInCount = inCount;
    m_prevInBytes = inBytes;
    m_prevOutCount = outCount;
    m_prevOutBytes = outBytes;
    m_prevStampMs = now;

    m_tblStats->item(0, 0)->setText(inLabel);
    m_tblStats->item(0, 1)->setText(QString::number(inCount));
    m_tblStats->item(0, 2)->setText(formatBytes(inBytes));
    m_tblStats->item(0, 3)->setText(QString::number(inPps, 'f', 1));
    m_tblStats->item(0, 4)->setText(QString::number(qRound64(inBps)));

    m_tblStats->item(1, 0)->setText(outLabel);
    m_tblStats->item(1, 1)->setText(QString::number(outCount));
    m_tblStats->item(1, 2)->setText(formatBytes(outBytes));
    m_tblStats->item(1, 3)->setText(QString::number(outPps, 'f', 1));
    m_tblStats->item(1, 4)->setText(QString::number(qRound64(outBps)));
}

void MainWindow::resetRateBase()
{
    m_prevInCount = 0;
    m_prevInBytes = 0;
    m_prevOutCount = 0;
    m_prevOutBytes = 0;
    m_prevStampMs = 0;
}

void MainWindow::appendLog(int level, const QString &line)
{
    if (!m_log)
        return;
    QString color = QStringLiteral("#334");
    if (level == int(LogLevel::Error))
        color = QStringLiteral("#b3261e");
    else if (level == int(LogLevel::Warn))
        color = QStringLiteral("#9a6700");
    else if (level == int(LogLevel::Debug))
        color = QStringLiteral("#777");

    m_log->appendHtml(QStringLiteral("<span style=\"color:%1\">%2</span>")
                          .arg(color, line.toHtmlEscaped()));
}

void MainWindow::onExportConfig()
{
    const QString role = m_roleServer->isChecked() ? QStringLiteral("server")
                                                   : QStringLiteral("client");
    const QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("导出配置"),
        QCoreApplication::applicationDirPath() + QStringLiteral("/config-%1.ini").arg(role),
        QStringLiteral("INI 配置 (*.ini)"));
    if (path.isEmpty())
        return;

    QString err;
    if (!collectConfig().save(path, &err)) {
        QMessageBox::warning(this, QStringLiteral("导出失败"), err);
        return;
    }
    logI(QStringLiteral("已导出配置: %1").arg(path));
    statusBar()->showMessage(QStringLiteral("已导出配置: %1").arg(path), 4000);
}

void MainWindow::onImportConfig()
{
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("导入配置"), QCoreApplication::applicationDirPath(),
        QStringLiteral("INI 配置 (*.ini)"));
    if (path.isEmpty())
        return;

    Config c;
    QString err;
    if (!Config::loadAuto(path, &c, &err)) {
        QMessageBox::warning(this, QStringLiteral("导入失败"), err);
        return;
    }

    loadFromConfig(c);
    saveConfig();

    QString verr;
    if (!c.validate(&verr))
        logW(QStringLiteral("导入的配置有待修正: %1").arg(verr));

    logI(QStringLiteral("已导入配置: %1").arg(path));
    statusBar()->showMessage(QStringLiteral("已导入配置: %1").arg(path), 4000);
}

void MainWindow::onRemoteUpdate()
{
    if (m_running) {
        QMessageBox::information(this, QStringLiteral("请先停止"),
                                 QStringLiteral("远程更新会占用隧道并让服务端重启，请先点击【停止】。"));
        return;
    }
    if (m_roleServer->isChecked()) {
        QMessageBox::information(this, QStringLiteral("角色不符"),
                                 QStringLiteral("远程更新由客户端发起：请选择「客户端」角色并填好服务端地址。"));
        return;
    }

    const Config c = collectConfig();
    if (c.remoteHost.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("缺少目标"),
                             QStringLiteral("请先填写服务端地址。"));
        return;
    }

    const QString file = QFileDialog::getOpenFileName(
        this, QStringLiteral("选择要推送到服务端的程序"),
        QCoreApplication::applicationDirPath(),
        QStringLiteral("可执行文件 (*.exe)"));
    if (file.isEmpty())
        return;

    QFile f(file);
    if (!f.open(QIODevice::ReadOnly)) {
        QMessageBox::warning(this, QStringLiteral("读取失败"), f.errorString());
        return;
    }
    const QByteArray data = f.readAll();
    f.close();
    if (data.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("读取失败"), QStringLiteral("文件为空。"));
        return;
    }

    const QString sha = QString::fromLatin1(
        QCryptographicHash::hash(data, QCryptographicHash::Sha256).left(12).toHex());

    const auto ret = QMessageBox::warning(
        this, QStringLiteral("确认远程更新"),
        QStringLiteral("目标：%1:%2\n文件：%3\n大小：%4\nSHA-256：%5…\n\n"
                       "服务端校验通过后会自行重启，转发中断约 5 秒。\n"
                       "若新程序启动异常，启动器会自动回滚到旧版本。\n\n是否继续？")
            .arg(c.remoteHost).arg(c.remotePort)
            .arg(QFileInfo(file).fileName())
            .arg(formatBytes(quint64(data.size())))
            .arg(sha),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (ret != QMessageBox::Yes)
        return;

    QProgressDialog dlg(QStringLiteral("正在上传并校验…"), QString(), 0, 100, this);
    dlg.setWindowTitle(QStringLiteral("远程更新"));
    dlg.setWindowModality(Qt::ApplicationModal);
    dlg.setMinimumDuration(0);
    dlg.setCancelButton(nullptr);
    dlg.setAutoClose(false);
    dlg.show();

    const UpdateResult r = UpdateClient::push(
        c.remoteHost, c.remotePort, c.password, file,
        [&dlg](int p) {
            dlg.setValue(p);
            QCoreApplication::processEvents();
        });
    dlg.close();

    if (r.ok) {
        logI(QStringLiteral("远程更新已提交: %1").arg(QFileInfo(file).fileName()));
        QMessageBox::information(this, QStringLiteral("更新已提交"),
                                 r.message + QStringLiteral("\n\n服务端重启期间会短暂断开，稍后自动重连。"));
    } else {
        logW(QStringLiteral("远程更新失败: %1").arg(r.message));
        QMessageBox::critical(this, QStringLiteral("更新失败"), r.message);
    }
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    stopEngine();
    saveConfig();
    event->accept();
}

} // namespace mc
