#include "server/tunnelserver.h"

#include "common/log.h"
#include "server/multicastsender.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QFile>
#include <QProcess>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>

namespace mc {

namespace {

// 常量时间比较，避免通过响应时间侧信道推断 HMAC
bool constantTimeEquals(const QByteArray &a, const QByteArray &b)
{
    if (a.size() != b.size())
        return false;
    unsigned char diff = 0;
    for (int i = 0; i < a.size(); ++i)
        diff |= static_cast<unsigned char>(a.at(i)) ^ static_cast<unsigned char>(b.at(i));
    return diff == 0;
}

} // namespace

TunnelServer::TunnelServer(const Config &cfg, MulticastSender *sender, QObject *parent)
    : QObject(parent)
    , m_cfg(cfg)
    , m_sender(sender)
{
}

TunnelServer::~TunnelServer()
{
    // 清理残留会话，顺带删掉未完成的 .new 暂存文件
    for (Session *ss : m_sessions) {
        abortUpdate(ss);
        delete ss;
    }
    m_sessions.clear();
}

bool TunnelServer::listen(QString *err)
{
    m_tcp = new QTcpServer(this);
    connect(m_tcp, &QTcpServer::newConnection, this, &TunnelServer::onNewConnection);

    QHostAddress addr;
    if (!addr.setAddress(m_cfg.listenHost)) {
        *err = QStringLiteral("listen.host 不是合法地址: %1").arg(m_cfg.listenHost);
        return false;
    }
    if (!m_tcp->listen(addr, m_cfg.listenPort)) {
        *err = QStringLiteral("监听 %1:%2 失败: %3")
                   .arg(m_cfg.listenHost)
                   .arg(m_cfg.listenPort)
                   .arg(m_tcp->errorString());
        return false;
    }

    m_tickTimer = new QTimer(this);
    m_tickTimer->setInterval(1000);
    connect(m_tickTimer, &QTimer::timeout, this, &TunnelServer::onTick);
    m_tickTimer->start();

    m_statsTimer = new QTimer(this);
    m_statsTimer->setInterval(m_cfg.statsIntervalSec * 1000);
    connect(m_statsTimer, &QTimer::timeout, this, &TunnelServer::logStats);
    m_statsTimer->start();

    logI(QStringLiteral("监听已启动: %1:%2").arg(m_cfg.listenHost).arg(m_cfg.listenPort));
    return true;
}

int TunnelServer::handshakingCount() const
{
    int n = 0;
    for (auto *ss : m_sessions) {
        if (ss->phase < 2)
            ++n;
    }
    return n;
}

void TunnelServer::onNewConnection()
{
    while (m_tcp->hasPendingConnections()) {
        QTcpSocket *s = m_tcp->nextPendingConnection();

        if (handshakingCount() >= kMaxHandshaking) {
            logW(QStringLiteral("握手中连接已达上限 %1，拒绝 %2:%3")
                     .arg(kMaxHandshaking)
                     .arg(s->peerAddress().toString())
                     .arg(s->peerPort()));
            s->abort();
            s->deleteLater();
            continue;
        }

        auto *ss = new Session;
        ss->since.start();
        ss->lastRx.start();
        m_sessions.insert(s, ss);

        connect(s, &QTcpSocket::readyRead, this, &TunnelServer::onReadyRead);
        connect(s, &QTcpSocket::disconnected, this, &TunnelServer::onDisconnected);

        logI(QStringLiteral("新连接 %1:%2").arg(s->peerAddress().toString()).arg(s->peerPort()));
    }
}

void TunnelServer::onReadyRead()
{
    auto *s = qobject_cast<QTcpSocket *>(sender());
    if (!s)
        return;

    Session *ss = m_sessions.value(s, nullptr);
    if (!ss) {
        s->readAll();
        return;
    }

    ss->lastRx.restart();
    const QByteArray chunk = s->readAll();

    if (ss->phase == 3) {
        return; // 拒绝中，丢弃后续字节
    }
    if (ss->phase == 2) {
        // 已认证：字节流直接进解帧器
        ss->reader.feed(chunk);
        handleFrames(s, ss);
    } else {
        // 握手中：按定长报文累积
        ss->buf.append(chunk);
        handleHandshake(s, ss);
    }
}

void TunnelServer::handleHandshake(QTcpSocket *s, Session *ss)
{
    if (ss->phase == 0) {
        if (ss->buf.size() < kHelloLen)
            return;
        const QByteArray hello = ss->buf.left(kHelloLen);
        ss->buf.remove(0, kHelloLen);

        if (!hello.startsWith("MCFW")) {
            dropSession(s, QStringLiteral("HELLO magic 不匹配，关闭连接"), true);
            return;
        }
        if (quint8(hello.at(4)) != kVersion) {
            dropSession(s, QStringLiteral("HELLO 版本不支持(收到 0x%1)")
                               .arg(quint8(hello.at(4)), 2, 16, QLatin1Char('0')), true);
            return;
        }
        if (hello.at(5) != 0 || hello.at(6) != 0 || hello.at(7) != 0) {
            dropSession(s, QStringLiteral("HELLO flags/reserved 非 0，关闭连接"), true);
            return;
        }

        ss->nonce = makeNonce();
        s->write(ss->nonce);
        s->flush();
        ss->phase = 1;
        return;
    }

    if (ss->phase == 1) {
        if (ss->buf.size() < kAuthLen)
            return;
        const QByteArray got = ss->buf.left(kAuthLen);
        ss->buf.remove(0, kAuthLen);

        const QByteArray want = computeAuth(m_cfg.password, ss->nonce);
        if (!constantTimeEquals(got, want)) {
            ++m_stats.authFails;
            ss->phase = 3;
            // 先确保失败字节送达，再延迟 1 秒关闭，降低暴力破解速率
            s->write(QByteArray(1, char(kAuthFailed)));
            s->flush();
            logW(QStringLiteral("认证失败 %1:%2").arg(s->peerAddress().toString()).arg(s->peerPort()));
            QTimer::singleShot(1000, s, [this, s]() {
                dropSession(s, QStringLiteral("认证失败，关闭连接"), true);
            });
            return;
        }

        s->write(QByteArray(1, char(kAuthOk)));
        s->flush();
        ss->phase = 2;
        promote(s);
    }
}

void TunnelServer::promote(QTcpSocket *s)
{
    Session *ss = m_sessions.value(s, nullptr);
    if (!ss)
        return;

    QTcpSocket *old = m_active;

    // 先接管，再关闭旧连接，避免出现"无活动会话"的空窗
    m_active = s;
    m_stats.connected = 1;
    ss->reader.reset();
    if (!ss->buf.isEmpty()) {
        ss->reader.feed(ss->buf);
        ss->buf.clear();
    }

    logI(QStringLiteral("会话建立 %1:%2，认证通过").arg(s->peerAddress().toString()).arg(s->peerPort()));

    if (old && old != s) {
        logI(QStringLiteral("新会话接管，关闭旧会话"));
        dropSession(old, QStringLiteral("被新会话替换"), false);
    }

    handleFrames(s, ss);
}

void TunnelServer::handleFrames(QTcpSocket *s, Session *ss)
{
    for (;;) {
        quint8 type = 0;
        QByteArray payload;
        QString why;
        const FrameReader::Result r = ss->reader.next(&type, &payload, &why);

        if (r == FrameReader::NeedMore)
            return;
        if (r == FrameReader::Illegal) {
            ++m_stats.illegalFrames;
            logE(QStringLiteral("非法帧：%1").arg(why));
            dropSession(s, QStringLiteral("非法帧，断开连接"), false);
            return;
        }

        if (type == kTypePing) {
            s->write(encodePong());
            continue;
        }
        if (type == kTypePong) {
            logW(QStringLiteral("收到客户端的 PONG，方向错误，已忽略"));
            continue;
        }
        if (type == kTypeUpdateBegin) {
            handleUpdateBegin(s, ss, payload);
            continue;
        }
        if (type == kTypeUpdateChunk) {
            handleUpdateChunk(s, ss, payload);
            continue;
        }
        if (type == kTypeUpdateEnd) {
            handleUpdateEnd(s, ss);
            continue;
        }
        if (type == kTypeUpdateResult) {
            continue; // 结果帧只由服务端发出
        }

        // DATA
        ++m_stats.framesIn;
        m_stats.framesInBytes += quint64(payload.size());
        if (m_sender->send(payload)) {
            ++m_stats.mcSent;
            m_stats.mcSentBytes += quint64(payload.size());
        } else {
            ++m_stats.mcSendFail;
            logW(QStringLiteral("组播发送失败（%1 字节）").arg(payload.size()));
        }
    }
}

void TunnelServer::handleUpdateBegin(QTcpSocket *s, Session *ss, const QByteArray &payload)
{
    if (ss->updFile) {
        s->write(makeUpdateResult(kUpdateBusy, QStringLiteral("已有更新正在传输")));
        return;
    }

    UpdateBegin req;
    QString why;
    if (!parseUpdateBegin(payload, &req, &why)) {
        s->write(makeUpdateResult(kUpdateBadReq, why));
        return;
    }

    const QString staged =
        QCoreApplication::applicationDirPath() + QStringLiteral("/multicastForward.exe.new");
    auto *f = new QFile(staged);
    if (!f->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        const QString msg = f->errorString();
        delete f;
        logE(QStringLiteral("远程更新：无法写入暂存文件（%1）").arg(msg));
        s->write(makeUpdateResult(kUpdateWriteFail, msg));
        return;
    }

    ss->updFile = f;
    ss->updSha = req.sha256;
    ss->updSize = req.size;
    ss->updWritten = 0;

    logI(QStringLiteral("远程更新：开始接收 %1 字节，SHA-256 %2…")
             .arg(req.size)
             .arg(QString::fromLatin1(req.sha256.left(8).toHex())));
}

void TunnelServer::handleUpdateChunk(QTcpSocket *s, Session *ss, const QByteArray &payload)
{
    if (!ss->updFile) {
        s->write(makeUpdateResult(kUpdateBadReq, QStringLiteral("未收到 UPDATE_BEGIN")));
        return;
    }

    quint32 offset = 0;
    QByteArray data;
    QString why;
    if (!parseUpdateChunk(payload, &offset, &data, &why)) {
        s->write(makeUpdateResult(kUpdateBadReq, why));
        abortUpdate(ss);
        return;
    }
    if (offset != ss->updWritten) {
        s->write(makeUpdateResult(kUpdateBadReq, QStringLiteral("分块偏移不连续")));
        abortUpdate(ss);
        return;
    }
    if (quint64(ss->updWritten) + quint64(data.size()) > quint64(ss->updSize)) {
        s->write(makeUpdateResult(kUpdateBadReq, QStringLiteral("数据超过声明长度")));
        abortUpdate(ss);
        return;
    }
    if (ss->updFile->write(data) != data.size()) {
        s->write(makeUpdateResult(kUpdateWriteFail, ss->updFile->errorString()));
        abortUpdate(ss);
        return;
    }
    ss->updWritten += quint32(data.size());
}

void TunnelServer::handleUpdateEnd(QTcpSocket *s, Session *ss)
{
    if (!ss->updFile) {
        s->write(makeUpdateResult(kUpdateBadReq, QStringLiteral("未收到 UPDATE_BEGIN")));
        return;
    }

    QFile *f = ss->updFile;
    ss->updFile = nullptr; // 交给本函数接管
    const QString staged = f->fileName();

    f->flush();
    f->close();

    if (ss->updWritten != ss->updSize) {
        delete f;
        QFile::remove(staged);
        const QString msg = QStringLiteral("长度不符：收到 %1，声明 %2")
                                .arg(ss->updWritten).arg(ss->updSize);
        logW(QStringLiteral("远程更新失败：%1").arg(msg));
        s->write(makeUpdateResult(kUpdateHashFail, msg));
        return;
    }

    QByteArray actual;
    if (f->open(QIODevice::ReadOnly)) {
        QCryptographicHash h(QCryptographicHash::Sha256);
        if (h.addData(f))
            actual = h.result();
        f->close();
    }
    delete f;

    if (actual != ss->updSha) {
        QFile::remove(staged);
        logE(QStringLiteral("远程更新失败：SHA-256 不一致"));
        s->write(makeUpdateResult(kUpdateHashFail, QStringLiteral("SHA-256 不一致，已丢弃")));
        return;
    }

    QString err;
    if (!startLauncher(staged, &err)) {
        QFile::remove(staged);
        logE(QStringLiteral("远程更新失败：%1").arg(err));
        s->write(makeUpdateResult(kUpdateWriteFail, err));
        return;
    }

    logI(QStringLiteral("远程更新：校验通过，等待启动器替换并重启"));
    s->write(makeUpdateResult(kUpdateOk, QStringLiteral("校验通过，服务端即将重启")));
    s->flush();

    // 交棒给启动器，本进程稍后正常退出
    QTimer::singleShot(800, qApp, []() { QCoreApplication::quit(); });
}

void TunnelServer::abortUpdate(Session *ss)
{
    if (!ss->updFile)
        return;
    const QString path = ss->updFile->fileName();
    ss->updFile->close();
    delete ss->updFile;
    ss->updFile = nullptr;
    ss->updWritten = 0;
    QFile::remove(path);
}

bool TunnelServer::startLauncher(const QString &stagedPath, QString *err)
{
    // 已由启动器守护：本进程退出后启动器会发现 .new 并完成替换，无需再拉起
    if (qEnvironmentVariableIsSet("MCFW_LAUNCHED_BY_LAUNCHER"))
        return true;

    const QString dir = QCoreApplication::applicationDirPath();
    const QString launcher = dir + QStringLiteral("/multicastForwardLauncher.exe");
    if (!QFile::exists(launcher)) {
        *err = QStringLiteral("未找到启动器 multicastForwardLauncher.exe");
        return false;
    }

    // 启动器参数：--apply <主程序路径> <新文件路径>
    const bool ok = QProcess::startDetached(
        launcher,
        {QStringLiteral("--apply"),
         QCoreApplication::applicationFilePath(),
         stagedPath});
    if (!ok)
        *err = QStringLiteral("启动器启动失败");
    return ok;
}

void TunnelServer::dropSession(QTcpSocket *s, const QString &reason, bool logAsWarn)
{
    auto it = m_sessions.find(s);
    if (it == m_sessions.end()) {
        s->deleteLater();
        return;
    }

    const bool wasActive = (m_active == s);

    abortUpdate(it.value());
    delete it.value();
    m_sessions.erase(it);

    // 只有自己仍是活动会话时才清空，避免旧连接的关闭回调误伤新会话
    if (wasActive) {
        m_active = nullptr;
        m_stats.connected = 0;
    }

    s->disconnectFromHost();
    s->deleteLater();

    if (logAsWarn)
        logW(reason);
    else
        logI(reason);
}

void TunnelServer::onDisconnected()
{
    auto *s = qobject_cast<QTcpSocket *>(sender());
    if (!s)
        return;

    auto it = m_sessions.find(s);
    if (it != m_sessions.end()) {
        const bool wasActive = (m_active == s);
        delete it.value();
        m_sessions.erase(it);

        if (wasActive) {
            m_active = nullptr;
            m_stats.connected = 0;
            logW(QStringLiteral("活动会话断开 %1:%2").arg(s->peerAddress().toString()).arg(s->peerPort()));
        } else {
            logD(QStringLiteral("连接关闭 %1:%2").arg(s->peerAddress().toString()).arg(s->peerPort()));
        }
    }
    s->deleteLater();
}

void TunnelServer::onTick()
{
    // 握手超时
    QList<QTcpSocket *> handshakeTimeouts;
    for (auto it = m_sessions.begin(); it != m_sessions.end(); ++it) {
        Session *ss = it.value();
        if (ss->phase < 2 && ss->since.elapsed() > 5000)
            handshakeTimeouts.append(it.key());
    }
    for (auto *s : handshakeTimeouts)
        dropSession(s, QStringLiteral("握手超时（5 秒），关闭连接"), true);

    // 心跳超时
    QTcpSocket *active = m_active;
    if (active) {
        auto it = m_sessions.find(active);
        if (it != m_sessions.end()) {
            const qint64 limit = qint64(m_cfg.heartbeatTimeoutSec) * 1000;
            if (it.value()->lastRx.elapsed() > limit) {
                dropSession(active, QStringLiteral("心跳超时 %1 秒，判定对端失联")
                                        .arg(m_cfg.heartbeatTimeoutSec), true);
            }
        }
    }
}

void TunnelServer::logStats()
{
    // 常规只记一行摘要，完整计数放到 debug，避免日志过长
    logI(QStringLiteral("进口 %1 帧 / %2，出口 %3 包 / %4")
             .arg(m_stats.framesIn).arg(formatBytes(m_stats.framesInBytes))
             .arg(m_stats.mcSent).arg(formatBytes(m_stats.mcSentBytes)));
    logD(QStringLiteral("统计 %1").arg(m_stats.toLogLine()));
}

} // namespace mc
