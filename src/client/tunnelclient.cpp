#include "client/tunnelclient.h"

#include "common/log.h"

#include <QTcpSocket>
#include <QTimer>

namespace mc {

// TCP 待发字节高水位：超过即暂停发送泵，把"背压"挡在自管队列里
static const qint64 kHighWatermark = 64 * 1024;
// 连接不可用时的固定重试间隔
static const int kReconnectDelayMs = 3000;

const char *TunnelClient::stateName(State s)
{
    switch (s) {
    case State::Idle:        return "idle";
    case State::Connecting:  return "connecting";
    case State::Handshaking: return "handshaking";
    case State::Running:     return "running";
    case State::Backoff:     return "backoff";
    }
    return "?";
}

TunnelClient::TunnelClient(const Config &cfg, TxQueue *queue, QObject *parent)
    : QObject(parent)
    , m_cfg(cfg)
    , m_queue(queue)
    , m_backoffSec(cfg.reconnectMinSec)
{
    m_sock = new QTcpSocket(this);

    m_handshakeTimer = new QTimer(this);
    m_handshakeTimer->setSingleShot(true);
    m_heartbeatTimer = new QTimer(this);
    m_pumpTimer = new QTimer(this);
    m_statsTimer = new QTimer(this);
    m_reconnectTimer = new QTimer(this);
    m_reconnectTimer->setSingleShot(true);

    connect(m_sock, &QTcpSocket::connected, this, &TunnelClient::onConnected);
    connect(m_sock, &QTcpSocket::readyRead, this, &TunnelClient::onReadyRead);
    connect(m_sock, &QTcpSocket::disconnected, this, &TunnelClient::onDisconnected);
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    connect(m_sock, &QAbstractSocket::errorOccurred, this, &TunnelClient::onSocketError);
#else
    connect(m_sock, QOverload<QAbstractSocket::SocketError>::of(&QAbstractSocket::error),
            this, &TunnelClient::onSocketError);
#endif
    connect(m_sock, &QTcpSocket::bytesWritten, this, &TunnelClient::pump);

    connect(m_handshakeTimer, &QTimer::timeout, this, &TunnelClient::onHandshakeTimeout);
    connect(m_heartbeatTimer, &QTimer::timeout, this, &TunnelClient::onHeartbeat);
    connect(m_pumpTimer, &QTimer::timeout, this, &TunnelClient::pump);
    connect(m_statsTimer, &QTimer::timeout, this, &TunnelClient::logStats);
    connect(m_reconnectTimer, &QTimer::timeout, this, &TunnelClient::onReconnectTimer);
}

void TunnelClient::setState(State s)
{
    if (m_state == s)
        return;
    logD(QStringLiteral("连接状态 %1 -> %2")
             .arg(QString::fromLatin1(stateName(m_state)), QString::fromLatin1(stateName(s))));
    m_state = s;
    m_stats.connected = (s == State::Running) ? 1 : 0;
}

void TunnelClient::start()
{
    m_stopping = false;
    m_statsTimer->start(m_cfg.statsIntervalSec * 1000);
    connectNow();
}

void TunnelClient::stop()
{
    m_stopping = true;
    stopAllTimers();
    m_queue->clear();
    setState(State::Idle);
    m_sock->abort();
    logI(QStringLiteral("已停止。进口 %1 包 / %2，出口 %3 帧 / %4")
             .arg(m_stats.rxPackets).arg(formatBytes(m_stats.rxBytes))
             .arg(m_stats.txFrames).arg(formatBytes(m_stats.txBytes)));
    logD(QStringLiteral("完整统计 %1").arg(m_stats.toLogLine()));
}

void TunnelClient::stopAllTimers()
{
    m_handshakeTimer->stop();
    m_heartbeatTimer->stop();
    m_pumpTimer->stop();
    m_reconnectTimer->stop();
}

void TunnelClient::connectNow()
{
    if (m_stopping)
        return;

    m_in.clear();
    m_reader.reset();
    m_phase = Phase::WaitChallenge;
    m_pingsOut = 0;

    setState(State::Connecting);
    if (m_failStreak == 0)
        logI(QStringLiteral("连接 %1:%2 ...").arg(m_cfg.remoteHost).arg(m_cfg.remotePort));
    else
        logD(QStringLiteral("第 %1 次尝试连接 %2:%3")
                 .arg(m_failStreak + 1).arg(m_cfg.remoteHost).arg(m_cfg.remotePort));
    m_sock->connectToHost(m_cfg.remoteHost, m_cfg.remotePort);
    m_handshakeTimer->start(5000); // 需求 5.2-5：TCP 建立到 AUTH_RESULT 共 5 秒
}

void TunnelClient::onConnected()
{
    if (m_stopping)
        return;

    m_sock->setSocketOption(QAbstractSocket::LowDelayOption, 1); // TCP_NODELAY
    setState(State::Handshaking);
    m_sock->write(makeHello());
    if (m_failStreak == 0)
        logI(QStringLiteral("TCP 已连接，发送 HELLO"));
    else
        logD(QStringLiteral("TCP 已重新连接，发送 HELLO"));
}

void TunnelClient::onReadyRead()
{
    switch (m_state) {
    case State::Handshaking:
        m_in.append(m_sock->readAll());
        handleHandshake();
        break;
    case State::Running:
        m_reader.feed(m_sock->readAll());
        handleRunning();
        break;
    default:
        m_sock->readAll(); // 丢弃
        break;
    }
}

void TunnelClient::handleHandshake()
{
    if (m_phase == Phase::WaitChallenge) {
        if (m_in.size() < kChallengeLen)
            return;
        const QByteArray nonce = m_in.left(kChallengeLen);
        m_in.remove(0, kChallengeLen);
        m_sock->write(computeAuth(m_cfg.password, nonce));
        m_phase = Phase::WaitResult;
        logD(QStringLiteral("收到挑战码，已回 AUTH"));
    }

    if (m_phase == Phase::WaitResult) {
        if (m_in.isEmpty())
            return;
        const quint8 result = quint8(m_in.at(0));
        m_in.remove(0, 1);

        if (result != kAuthOk) {
            ++m_stats.authFails;
            logE(QStringLiteral("认证失败（服务端返回 0x%1）：口令不一致或协议不匹配")
                     .arg(result, 2, 16, QLatin1Char('0')));
            scheduleReconnect(true);
            return;
        }

        m_handshakeTimer->stop();
        setState(State::Running);
        m_backoffSec = m_cfg.reconnectMinSec;
        m_failStreak = 0; // 连上了，下次断线重新从 warn 开始记
        m_pingsOut = 0;
        m_heartbeatTimer->start(m_cfg.heartbeatIntervalSec * 1000);
        m_pumpTimer->start(5);
        logI(QStringLiteral("认证成功，隧道进入工作状态"));

        if (!m_in.isEmpty()) {
            m_reader.feed(m_in);
            m_in.clear();
        }
        handleRunning();
        pump();
    }
}

void TunnelClient::handleRunning()
{
    quint8 type = 0;
    QByteArray payload;
    QString why;
    for (;;) {
        const FrameReader::Result r = m_reader.next(&type, &payload, &why);
        if (r == FrameReader::NeedMore)
            return;
        if (r == FrameReader::Illegal) {
            ++m_stats.illegalFrames;
            logE(QStringLiteral("非法帧：%1，断开重连").arg(why));
            scheduleReconnect(false);
            return;
        }
        if (type == kTypePong) {
            m_pingsOut = 0;
        } else if (type == kTypePing) {
            logW(QStringLiteral("收到服务端的 PING，方向错误，已忽略"));
        } else { // kTypeData：服务端不应向客户端送数据
            ++m_stats.illegalFrames;
            logE(QStringLiteral("收到服务端的数据帧，违反单向隧道约定，断开重连"));
            scheduleReconnect(false);
            return;
        }
    }
}

void TunnelClient::pump()
{
    if (m_state != State::Running)
        return;

    while (m_sock->bytesToWrite() < kHighWatermark) {
        QByteArray frame;
        if (!m_queue->pop(&frame))
            break;
        m_sock->write(frame);
        ++m_stats.txFrames;
        m_stats.txBytes += quint64(frame.size() - kHeaderLen); // 只计载荷
    }
    m_stats.queueDepth = m_queue->size();
    m_stats.dropQueueFull = m_queue->dropped();
}

void TunnelClient::onHeartbeat()
{
    if (m_state != State::Running)
        return;

    m_sock->write(encodePing());
    if (++m_pingsOut >= 3) {
        logW(QStringLiteral("连续 3 次未收到 PONG，判定连接失效"));
        scheduleReconnect(false);
    }
}

void TunnelClient::onHandshakeTimeout()
{
    if (m_state != State::Connecting && m_state != State::Handshaking)
        return;
    if (m_failStreak == 0)
        logW(QStringLiteral("握手超时（5 秒）"));
    else
        logD(QStringLiteral("握手超时（第 %1 次尝试）").arg(m_failStreak + 1));
    scheduleReconnect(false);
}

void TunnelClient::onSocketError()
{
    if (m_stopping)
        return;
    logD(QStringLiteral("socket 错误: %1").arg(m_sock->errorString()));

    // TCP 根本没连上时不必再等 5 秒握手超时，直接按固定间隔重试
    if (m_state == State::Connecting)
        scheduleReconnect(false);
}

void TunnelClient::onDisconnected()
{
    if (m_stopping)
        return;
    if (m_state == State::Running || m_state == State::Handshaking || m_state == State::Connecting) {
        logW(QStringLiteral("连接断开：%1").arg(m_sock->errorString()));
        scheduleReconnect(false);
    }
}

void TunnelClient::scheduleReconnect(bool authFailure)
{
    if (m_stopping)
        return;
    if (m_state == State::Backoff)
        return; // 已在退避中

    int delayMs = 0;
    if (authFailure) {
        // 需求 FR-A5：认证失败按最大退避间隔重连
        delayMs = m_cfg.reconnectMaxSec * 1000;
        m_backoffSec = m_cfg.reconnectMinSec;
    } else {
        // 连接失败/断开：固定 3 秒重试，避免长时间无连接和日志刷屏
        delayMs = kReconnectDelayMs;
        m_backoffSec = m_cfg.reconnectMinSec;
    }

    ++m_stats.reconnects;
    ++m_failStreak;
    setState(State::Backoff);

    m_heartbeatTimer->stop();
    m_pumpTimer->stop();
    m_handshakeTimer->stop();

    m_sock->abort();

    // 断线期间的数据不缓存、不补发
    m_queue->clear();

    // 只有连续失败的第一条记 warn，后续重试降到 debug，避免每 3 秒刷一行
    if (m_failStreak == 1)
        logW(QStringLiteral("连接不可用（%1），每 %2 秒重试一次")
                 .arg(m_sock->errorString())
                 .arg(delayMs / 1000));
    else
        logD(QStringLiteral("连续失败 %1 次，%2 秒后重试")
                 .arg(m_failStreak).arg(delayMs / 1000));

    m_reconnectTimer->start(delayMs);
}

void TunnelClient::onReconnectTimer()
{
    connectNow();
}

void TunnelClient::logStats()
{
    m_stats.queueDepth = m_queue->size();
    m_stats.dropQueueFull = m_queue->dropped();

    // 常规只记一行摘要，完整计数放到 debug，避免日志过长
    logI(QStringLiteral("进口 %1 包 / %2，出口 %3 帧 / %4")
             .arg(m_stats.rxPackets).arg(formatBytes(m_stats.rxBytes))
             .arg(m_stats.txFrames).arg(formatBytes(m_stats.txBytes)));
    logD(QStringLiteral("统计 %1").arg(m_stats.toLogLine()));
}

} // namespace mc
