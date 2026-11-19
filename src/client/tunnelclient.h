#pragma once

#include "client/txqueue.h"
#include "common/config.h"
#include "common/protocol.h"
#include "common/stats.h"

#include <QByteArray>
#include <QObject>

class QTcpSocket;
class QTimer;

namespace mc {

// 主机 A 的隧道客户端：连接 / 握手 / 发送泵 / 心跳 / 重连退避
class TunnelClient : public QObject
{
    Q_OBJECT
public:
    TunnelClient(const Config &cfg, TxQueue *queue, QObject *parent = nullptr);

    void start();
    void stop();

    const Stats &stats() const { return m_stats; }
    bool isRunning() const { return m_state == State::Running; }

public slots:
    void pump();

private slots:
    void connectNow();
    void onConnected();
    void onReadyRead();
    void onSocketError();
    void onDisconnected();
    void onHeartbeat();
    void onHandshakeTimeout();
    void onReconnectTimer();
    void logStats();

private:
    enum class State { Idle, Connecting, Handshaking, Running, Backoff };
    enum class Phase { WaitChallenge, WaitResult };

    void setState(State s);
    void handleHandshake();
    void handleRunning();
    void scheduleReconnect(bool authFailure);
    void stopAllTimers();
    static const char *stateName(State s);

    Config m_cfg;
    TxQueue *m_queue = nullptr;
    QTcpSocket *m_sock = nullptr;

    State m_state = State::Idle;
    Phase m_phase = Phase::WaitChallenge;

    QByteArray m_in;
    FrameReader m_reader;

    QTimer *m_handshakeTimer = nullptr;
    QTimer *m_heartbeatTimer = nullptr;
    QTimer *m_pumpTimer = nullptr;
    QTimer *m_statsTimer = nullptr;
    QTimer *m_reconnectTimer = nullptr;

    int m_backoffSec = 1;
    int m_failStreak = 0; // 连续连接失败次数，用于抑制重复日志
    int m_pingsOut = 0;
    bool m_stopping = false;

    Stats m_stats;
};

} // namespace mc
