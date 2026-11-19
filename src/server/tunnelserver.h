#pragma once

#include "common/config.h"
#include "common/protocol.h"
#include "common/stats.h"

#include <QElapsedTimer>
#include <QHash>
#include <QObject>

class QTcpServer;
class QTcpSocket;
class QTimer;
class QFile;

namespace mc {

class MulticastSender;

// 主机 B：监听、握手认证、单活动会话、解帧后组播转发
class TunnelServer : public QObject
{
    Q_OBJECT
public:
    TunnelServer(const Config &cfg, MulticastSender *sender, QObject *parent = nullptr);
    ~TunnelServer() override;

    bool listen(QString *err);
    const Stats &stats() const { return m_stats; }

private slots:
    void onNewConnection();
    void onReadyRead();
    void onDisconnected();
    void onTick();
    void logStats();

private:
    struct Session
    {
        QByteArray buf;
        int phase = 0; // 0=等 HELLO, 1=等 AUTH, 2=已认证, 3=拒绝中
        QByteArray nonce;
        QElapsedTimer since;
        QElapsedTimer lastRx;
        FrameReader reader;

        // 远程更新接收状态（未在传输时 updFile 为空）
        QFile *updFile = nullptr;
        QByteArray updSha;
        quint32 updSize = 0;
        quint32 updWritten = 0;
    };

    void handleHandshake(QTcpSocket *s, Session *ss);
    void handleFrames(QTcpSocket *s, Session *ss);
    void handleUpdateBegin(QTcpSocket *s, Session *ss, const QByteArray &payload);
    void handleUpdateChunk(QTcpSocket *s, Session *ss, const QByteArray &payload);
    void handleUpdateEnd(QTcpSocket *s, Session *ss);
    void abortUpdate(Session *ss);
    bool startLauncher(const QString &stagedPath, QString *err);
    void promote(QTcpSocket *s);
    void dropSession(QTcpSocket *s, const QString &reason, bool logAsWarn);
    int handshakingCount() const;

    static constexpr int kMaxHandshaking = 8;

    Config m_cfg;
    MulticastSender *m_sender = nullptr;
    QTcpServer *m_tcp = nullptr;
    QTcpSocket *m_active = nullptr;

    QHash<QTcpSocket *, Session *> m_sessions;

    QTimer *m_tickTimer = nullptr;
    QTimer *m_statsTimer = nullptr;

    Stats m_stats;
};

} // namespace mc
