#include "gui/updateclient.h"

#include "common/protocol.h"

#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QTcpSocket>

namespace mc {

namespace {

constexpr int kConnectTimeoutMs = 5000;
constexpr int kHandshakeTimeoutMs = 5000;
constexpr int kIoTimeoutMs = 15000;

bool readExact(QTcpSocket &sock, int n, QByteArray *out, int timeoutMs, QString *err)
{
    QByteArray buf;
    QElapsedTimer timer;
    timer.start();

    while (buf.size() < n) {
        if (sock.bytesAvailable() > 0) {
            buf.append(sock.read(n - buf.size()));
            continue;
        }
        const int remain = timeoutMs - int(timer.elapsed());
        if (remain <= 0) {
            *err = QStringLiteral("等待服务端响应超时");
            return false;
        }
        if (!sock.waitForReadyRead(remain)) {
            *err = QStringLiteral("读取失败: %1").arg(sock.errorString());
            return false;
        }
    }
    *out = buf;
    return true;
}

// 读一个完整帧（3 字节头 + 载荷）
bool readFrame(QTcpSocket &sock, quint8 *type, QByteArray *payload, QString *err)
{
    QByteArray head;
    if (!readExact(sock, kHeaderLen, &head, kIoTimeoutMs, err))
        return false;

    *type = quint8(head.at(0));
    const int len = (quint8(head.at(1)) << 8) | quint8(head.at(2));
    if (len < 0 || len > kUpdateResultMax) {
        *err = QStringLiteral("服务端返回了非法帧长度 %1").arg(len);
        return false;
    }
    if (len == 0) {
        payload->clear();
        return true;
    }
    return readExact(sock, len, payload, kIoTimeoutMs, err);
}

QByteArray sha256Of(const QByteArray &data)
{
    return QCryptographicHash::hash(data, QCryptographicHash::Sha256);
}

} // namespace

UpdateResult UpdateClient::push(const QString &host,
                                quint16 port,
                                const QByteArray &password,
                                const QString &localFile,
                                const std::function<void(int)> &onProgress)
{
    UpdateResult r;

    QFile f(localFile);
    if (!f.open(QIODevice::ReadOnly)) {
        r.message = QStringLiteral("无法读取本地文件: %1").arg(f.errorString());
        return r;
    }
    const QByteArray content = f.readAll();
    f.close();

    if (content.isEmpty()) {
        r.message = QStringLiteral("文件为空，已取消");
        return r;
    }

    QTcpSocket sock;
    sock.connectToHost(host, port);
    if (!sock.waitForConnected(kConnectTimeoutMs)) {
        r.message = QStringLiteral("连接 %1:%2 失败: %3").arg(host).arg(port).arg(sock.errorString());
        return r;
    }

    QString err;

    // 1) HELLO
    sock.write(makeHello());
    if (!sock.waitForBytesWritten(kHandshakeTimeoutMs)) {
        r.message = QStringLiteral("发送 HELLO 失败: %1").arg(sock.errorString());
        return r;
    }

    // 2) CHALLENGE
    QByteArray nonce;
    if (!readExact(sock, kChallengeLen, &nonce, kHandshakeTimeoutMs, &err)) {
        r.message = err;
        return r;
    }

    // 3) AUTH
    sock.write(computeAuth(password, nonce));
    if (!sock.waitForBytesWritten(kHandshakeTimeoutMs)) {
        r.message = QStringLiteral("发送 AUTH 失败: %1").arg(sock.errorString());
        return r;
    }

    // 4) AUTH_RESULT
    QByteArray authResult;
    if (!readExact(sock, 1, &authResult, kHandshakeTimeoutMs, &err)) {
        r.message = err;
        return r;
    }
    if (quint8(authResult.at(0)) != kAuthOk) {
        r.message = QStringLiteral("认证被拒绝：口令不一致");
        return r;
    }

    // 5) UPDATE_BEGIN
    const QByteArray sha = sha256Of(content);
    sock.write(makeUpdateBegin(quint32(content.size()), sha));
    if (!sock.waitForBytesWritten(kIoTimeoutMs)) {
        r.message = QStringLiteral("发送 UPDATE_BEGIN 失败: %1").arg(sock.errorString());
        return r;
    }

    // 6) 分块推送
    quint32 offset = 0;
    while (offset < quint32(content.size())) {
        const int n = qMin(kUpdateChunkSize, content.size() - int(offset));
        sock.write(makeUpdateChunk(offset, content.mid(int(offset), n)));
        if (!sock.waitForBytesWritten(kIoTimeoutMs)) {
            r.message = QStringLiteral("分块发送失败（偏移 %1）: %2")
                            .arg(offset).arg(sock.errorString());
            return r;
        }
        offset += quint32(n);
        if (onProgress)
            onProgress(int(qint64(offset) * 95 / content.size())); // 留 5% 给校验
    }

    // 7) UPDATE_END
    sock.write(makeUpdateEnd());
    if (!sock.waitForBytesWritten(kIoTimeoutMs)) {
        r.message = QStringLiteral("发送 UPDATE_END 失败: %1").arg(sock.errorString());
        return r;
    }

    // 8) UPDATE_RESULT
    quint8 type = 0;
    QByteArray payload;
    if (!readFrame(sock, &type, &payload, &err)) {
        r.message = err;
        return r;
    }
    if (type != kTypeUpdateResult) {
        r.message = QStringLiteral("期望 UPDATE_RESULT，收到 0x%1")
                        .arg(type, 2, 16, QLatin1Char('0'));
        return r;
    }

    quint8 code = 0xFF;
    QString msg;
    if (!parseUpdateResult(payload, &code, &msg)) {
        r.message = QStringLiteral("无法解析服务端结果");
        return r;
    }

    if (onProgress)
        onProgress(100);

    if (code == kUpdateOk) {
        r.ok = true;
        r.serverWillRestart = true;
        r.message = msg.isEmpty() ? QStringLiteral("服务端校验通过，正在重启") : msg;
    } else {
        r.message = msg.isEmpty()
            ? QStringLiteral("服务端拒绝更新（错误码 0x%1）").arg(code, 2, 16, QLatin1Char('0'))
            : msg;
    }
    return r;
}

} // namespace mc
