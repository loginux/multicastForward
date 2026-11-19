#pragma once

#include "client/txqueue.h"
#include "common/config.h"

#include <QMainWindow>

class QAction;
class QCheckBox;
class QComboBox;
class QGroupBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QRadioButton;
class QSpinBox;
class QTableWidget;
class QTimer;

namespace mc {

class MulticastReceiver;
class TunnelClient;
class MulticastSender;
class TunnelServer;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

protected:
    void closeEvent(QCloseEvent *event) override;

private slots:
    void onRoleChanged();
    void onStartClicked();
    void onImportConfig();
    void onExportConfig();
    void onRemoteUpdate();
    void onRefreshInterfaces();
    void refreshStats();
    void appendLog(int level, const QString &line);

private:
    void buildUi();
    void buildMenus();
    void populateInterfaces(const QString &preferred);
    void updateStatsTable(quint64 inCount, quint64 inBytes, quint64 outCount, quint64 outBytes,
                          const QString &inLabel, const QString &outLabel);
    void resetRateBase();
    void loadFromConfig(const Config &c);
    Config collectConfig() const;
    void saveConfig();
    QString defaultConfigPath() const;

    bool startClient(const Config &c, QString *err);
    bool startServer(const Config &c, QString *err);
    void stopEngine();
    void setRunning(bool running, const QString &statusText);

    // 角色
    QRadioButton *m_roleClient = nullptr;
    QRadioButton *m_roleServer = nullptr;

    // 配置项
    QLineEdit *m_edGroup = nullptr;
    QSpinBox *m_spMcPort = nullptr;
    QComboBox *m_cbIface = nullptr;
    QSpinBox *m_spTtl = nullptr;
    QCheckBox *m_chkLoop = nullptr;

    QGroupBox *m_boxClient = nullptr;
    QLineEdit *m_edServerHost = nullptr;
    QSpinBox *m_spServerPort = nullptr;

    QGroupBox *m_boxServer = nullptr;
    QLineEdit *m_edListenHost = nullptr;
    QSpinBox *m_spListenPort = nullptr;

    QLineEdit *m_edPassword = nullptr;
    QCheckBox *m_chkShowPassword = nullptr;

    QSpinBox *m_spQueue = nullptr;
    QSpinBox *m_spReMin = nullptr;
    QSpinBox *m_spReMax = nullptr;
    QSpinBox *m_spHbInterval = nullptr;
    QSpinBox *m_spHbTimeout = nullptr;
    QComboBox *m_cbLogLevel = nullptr;

    // 控制与显示
    QPushButton *m_btnStart = nullptr;
    QAction *m_actImport = nullptr;
    QAction *m_actExport = nullptr;
    QAction *m_actRemoteUpdate = nullptr;
    QLabel *m_lbStatus = nullptr;
    QTableWidget *m_tblStats = nullptr;

    // 速率计算基准（每次刷新取增量）
    quint64 m_prevInCount = 0;
    quint64 m_prevInBytes = 0;
    quint64 m_prevOutCount = 0;
    quint64 m_prevOutBytes = 0;
    qint64 m_prevStampMs = 0;
    QPlainTextEdit *m_log = nullptr;
    QTimer *m_statsTimer = nullptr;
    QLabel *m_lbConfigPath = nullptr;

    QList<QWidget *> m_configWidgets;

    // 引擎
    TxQueue *m_queue = nullptr;
    MulticastReceiver *m_rx = nullptr;
    TunnelClient *m_client = nullptr;
    MulticastSender *m_sender = nullptr;
    TunnelServer *m_server = nullptr;

    // 未在界面暴露但需原样保留的字段
    int m_statsIntervalSec = 5;
    QString m_logFile;

    bool m_running = false;
};

} // namespace mc
