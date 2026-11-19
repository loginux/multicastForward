// 组播发包器：用于隧道验收（AC-07 超长包、AC-13 吞吐等）
// 另带 TCP 直连模式：绕过组播，直接向主机 B 灌协议帧
#include "common/log.h"
#include "common/protocol.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QHostAddress>
#include <QNetworkInterface>
#include <QTcpSocket>
#include <QThread>
#include <QTimer>
#include <QUdpSocket>

#include <cstdio>
#include <cstring>

#ifdef Q_OS_WIN
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace {

QByteArray makePayload(int size, quint64 seq)
{
    QByteArray p(size, Qt::Uninitialized);
    memset(p.data(), 0x5A, size_t(size));
    for (int i = 0; i < 8 && i < size; ++i)
        p[i] = char((seq >> (8 * (7 - i))) & 0xFF);
    return p;
}

bool readExact(QTcpSocket &s, int n, QByteArray *out)
{
    QByteArray buf;
    QElapsedTimer t;
    t.start();
    while (buf.size() < n) {
        if (s.bytesAvailable() > 0) {
            buf.append(s.read(n - buf.size()));
            continue;
        }
        if (t.elapsed() > 5000)
            return false;
        s.waitForReadyRead(200);
    }
    *out = buf;
    return true;
}

// 直连主机 B：完成握手后按速率灌协议帧，用于绕过组播验证 B 的转发路径
int runTcp(const QString &host, quint16 port, const QByteArray &password,
           int size, int rate, int durationSec)
{
    QTcpSocket sock;
    sock.connectToHost(host, port);
    if (!sock.waitForConnected(3000)) {
        fprintf(stderr, "[error] 连接 %s:%u 失败: %s\n",
                host.toUtf8().constData(), unsigned(port),
                sock.errorString().toUtf8().constData());
        return 1;
    }

    sock.write(mc::makeHello());
    sock.flush();

    QByteArray nonce;
    if (!readExact(sock, mc::kChallengeLen, &nonce)) {
        fprintf(stderr, "[error] 未收到挑战码\n");
        return 1;
    }

    sock.write(mc::computeAuth(password, nonce));
    sock.flush();

    QByteArray result;
    if (!readExact(sock, 1, &result)) {
        fprintf(stderr, "[error] 未收到认证结果\n");
        return 1;
    }
    if (quint8(result.at(0)) != mc::kAuthOk) {
        fprintf(stderr, "[error] 认证失败（口令不一致）\n");
        return 1;
    }

    printf("认证成功，开始发送 %d 字节/包，%d pps，%d 秒\n", size, rate, durationSec);
    fflush(stdout);

    QElapsedTimer clock;
    clock.start();
    QElapsedTimer pacer;
    pacer.start();

    quint64 seq = 0, sent = 0, bytes = 0;
    const int intervalMs = qMax(1, 1000 / qMax(1, rate));
    qint64 nextAt = 0;

    while (durationSec <= 0 || clock.elapsed() < qint64(durationSec) * 1000) {
        const QByteArray payload = makePayload(size, seq);
        if (sock.write(mc::encodeFrame(mc::kTypeData, payload)) < 0)
            break;
        ++seq;
        ++sent;
        bytes += quint64(payload.size());
        sock.flush();

        nextAt += intervalMs;
        const qint64 now = pacer.elapsed();
        if (nextAt > now)
            QThread::msleep(ulong(nextAt - now));
    }

    sock.waitForBytesWritten(2000);
    sock.disconnectFromHost();

    const double secs = double(clock.elapsed()) / 1000.0;
    printf("完成: sent=%llu bytes=%llu 用时=%.2fs 实际速率=%.1f pps\n",
           static_cast<unsigned long long>(sent),
           static_cast<unsigned long long>(bytes),
           secs, secs > 0 ? double(sent) / secs : 0.0);
    return 0;
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("mc_sender"));

#ifdef Q_OS_WIN
    SetConsoleOutputCP(CP_UTF8);
#endif

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("向组播组灌包，载荷前 8 字节为大端序号\n"
                                                    "示例: mc_sender -i 192.168.1.10 -s 1024 -r 100 -d 600"));
    const QCommandLineOption oGroup({QStringLiteral("g"), QStringLiteral("group")},
                                    QStringLiteral("组播组地址"), QStringLiteral("addr"),
                                    QStringLiteral("239.1.1.1"));
    const QCommandLineOption oPort({QStringLiteral("p"), QStringLiteral("port")},
                                   QStringLiteral("目标端口"), QStringLiteral("port"),
                                   QStringLiteral("5001"));
    const QCommandLineOption oIface({QStringLiteral("i"), QStringLiteral("iface")},
                                    QStringLiteral("出口网卡 IPv4 地址（必填）"), QStringLiteral("ip"));
    const QCommandLineOption oSize({QStringLiteral("s"), QStringLiteral("size")},
                                   QStringLiteral("单包载荷字节数"), QStringLiteral("bytes"),
                                   QStringLiteral("64"));
    const QCommandLineOption oRate({QStringLiteral("r"), QStringLiteral("rate")},
                                   QStringLiteral("每秒包数"), QStringLiteral("pps"),
                                   QStringLiteral("100"));
    const QCommandLineOption oDuration({QStringLiteral("d"), QStringLiteral("duration")},
                                       QStringLiteral("持续秒数，0 表示不限"), QStringLiteral("sec"),
                                       QStringLiteral("10"));
    const QCommandLineOption oTcp(QStringLiteral("tcp"),
                                  QStringLiteral("直连主机 B（host:port），跳过组播直接灌协议帧"),
                                  QStringLiteral("host:port"));
    const QCommandLineOption oPassword(QStringLiteral("password"),
                                       QStringLiteral("TCP 模式下的认证口令"), QStringLiteral("pw"),
                                       QStringLiteral("change-me"));
    parser.addOption(oTcp);
    parser.addOption(oPassword);
    parser.addOption(oGroup);
    parser.addOption(oPort);
    parser.addOption(oIface);
    parser.addOption(oSize);
    parser.addOption(oRate);
    parser.addOption(oDuration);
    parser.addHelpOption();

    if (!parser.parse(app.arguments())) {
        fprintf(stderr, "%s\n", parser.errorText().toUtf8().constData());
        return 1;
    }
    if (parser.isSet(QStringLiteral("help"))) {
        printf("%s", parser.helpText().toUtf8().constData());
        return 0;
    }

    const QString groupText = parser.value(oGroup);
    const quint16 port = quint16(parser.value(oPort).toUInt());
    const QString ifaceText = parser.value(oIface);
    const int size = parser.value(oSize).toInt();
    const int rate = parser.value(oRate).toInt();
    const int durationSec = parser.value(oDuration).toInt();

    // TCP 直连模式：无需组播与网卡
    if (parser.isSet(oTcp)) {
        const QString spec = parser.value(oTcp);
        const int pos = spec.lastIndexOf(QLatin1Char(':'));
        if (pos <= 0) {
            fprintf(stderr, "[error] --tcp 格式应为 host:port\n");
            return 1;
        }
        const QString host = spec.left(pos);
        const quint16 port = quint16(spec.mid(pos + 1).toUInt());
        if (port == 0) {
            fprintf(stderr, "[error] --tcp 端口非法\n");
            return 1;
        }
        return runTcp(host, port, parser.value(oPassword).toUtf8(), size, rate, durationSec);
    }

    if (ifaceText.isEmpty()) {
        fprintf(stderr, "[error] 必须用 -i 指定出口网卡 IPv4 地址\n");
        return 1;
    }
    if (size < 1 || size > 65507) {
        fprintf(stderr, "[error] 包长必须在 1-65507 之间\n");
        return 1;
    }
    if (port == 0) {
        fprintf(stderr, "[error] 端口非法\n");
        return 1;
    }
    if (rate < 1) {
        fprintf(stderr, "[error] 速率必须大于 0\n");
        return 1;
    }

    QHostAddress group;
    if (!group.setAddress(groupText)) {
        fprintf(stderr, "[error] 组播地址非法: %s\n", groupText.toUtf8().constData());
        return 1;
    }

    QHostAddress wantIp;
    if (!wantIp.setAddress(ifaceText)) {
        fprintf(stderr, "[error] 网卡地址非法: %s\n", ifaceText.toUtf8().constData());
        return 1;
    }
    QNetworkInterface iface;
    for (const auto &ni : QNetworkInterface::allInterfaces()) {
        for (const auto &e : ni.addressEntries()) {
            if (e.ip() == wantIp) {
                iface = ni;
                break;
            }
        }
        if (iface.isValid())
            break;
    }
    if (!iface.isValid()) {
        fprintf(stderr, "[error] 找不到地址为 %s 的网卡\n", ifaceText.toUtf8().constData());
        return 1;
    }

    QUdpSocket sock;
    sock.bind(QHostAddress(QHostAddress::AnyIPv4), 0);
    sock.setMulticastInterface(iface);
    sock.setSocketOption(QAbstractSocket::MulticastTtlOption, 1);
    sock.setSocketOption(QAbstractSocket::MulticastLoopbackOption, 1);

    QByteArray payload(size, Qt::Uninitialized);
    memset(payload.data(), 0x5A, size_t(size));

    quint64 sent = 0;
    quint64 failed = 0;
    quint64 bytes = 0;
    QElapsedTimer clock;
    clock.start();

    QTimer timer;
    timer.setInterval(qMax(1, 1000 / rate));
    QObject::connect(&timer, &QTimer::timeout, [&]() {
        // 前 8 字节写入大端序号，便于接收侧核对连续性与边界
        for (int i = 0; i < 8 && i < size; ++i)
            payload[i] = char((sent >> (8 * (7 - i))) & 0xFF);

        const qint64 n = sock.writeDatagram(payload, group, port);
        if (n == qint64(payload.size())) {
            ++sent;
            bytes += quint64(payload.size());
        } else {
            ++failed;
        }

        if (durationSec > 0 && clock.elapsed() >= qint64(durationSec) * 1000) {
            timer.stop();
            QCoreApplication::quit();
        }
    });
    timer.start();

    printf("发送中: group=%s port=%u iface=%s size=%d rate=%dpps duration=%ds\n",
           groupText.toUtf8().constData(), unsigned(port), ifaceText.toUtf8().constData(),
           size, rate, durationSec);
    fflush(stdout);

    app.exec();

    const double secs = double(clock.elapsed()) / 1000.0;
    printf("完成: sent=%llu failed=%llu bytes=%llu 用时=%.2fs 实际速率=%.1f pps\n",
           static_cast<unsigned long long>(sent),
           static_cast<unsigned long long>(failed),
           static_cast<unsigned long long>(bytes),
           secs, secs > 0 ? double(sent) / secs : 0.0);
    return failed == 0 ? 0 : 1;
}
