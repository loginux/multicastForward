#include "client/multicastreceiver.h"

#include "common/log.h"
#include "common/protocol.h"

#include <QNetworkDatagram>
#include <QUdpSocket>

namespace mc {

MulticastReceiver::MulticastReceiver(const Config &cfg, TxQueue *queue, QObject *parent)
    : QObject(parent)
    , m_cfg(cfg)
    , m_queue(queue)
{
}

bool MulticastReceiver::open(QString *err)
{
    QHostAddress localIp;
    if (!m_cfg.pickInterface(&m_iface, &localIp, err))
        return false;

    if (!m_group.setAddress(m_cfg.group)) {
        *err = QStringLiteral("组播地址非法: %1").arg(m_cfg.group);
        return false;
    }

    m_sock = new QUdpSocket(this);

    // 绑定 0.0.0.0：入组不依赖绑定地址，绑到网卡 IP 反而收不到组播。
    // 用 ShareAddress 允许与同机上其他组播接收程序共用同一端口（现场常见）。
    if (!m_sock->bind(QHostAddress(QHostAddress::AnyIPv4), m_cfg.mcPort,
                      QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint)) {
        *err = QStringLiteral("绑定 UDP 端口 %1 失败: %2").arg(m_cfg.mcPort).arg(m_sock->errorString());
        return false;
    }

    if (!m_sock->joinMulticastGroup(m_group, m_iface)) {
        *err = QStringLiteral("在网卡 %1 上加入组 %2 失败: %3")
                   .arg(m_iface.humanReadableName(), m_cfg.group, m_sock->errorString());
        return false;
    }

    connect(m_sock, &QUdpSocket::readyRead, this, &MulticastReceiver::onReadyRead);

    logI(QStringLiteral("组播入组成功: group=%1 port=%2 iface=%3(%4)")
             .arg(m_cfg.group)
             .arg(m_cfg.mcPort)
             .arg(localIp.toString(), m_iface.humanReadableName()));
    return true;
}

void MulticastReceiver::onReadyRead()
{
    while (m_sock->hasPendingDatagrams()) {
        const qint64 sz = m_sock->pendingDatagramSize();

        if (sz <= 0) {
            char dummy = 0;
            m_sock->readDatagram(&dummy, 0);
            ++m_stats.dropZero;
            continue;
        }

        // 超长包：先按真实长度读出再丢弃，绝不截断（截断会篡改载荷，破坏 AC-02/AC-07）
        if (sz > kMaxPayload) {
            QByteArray discard(int(sz), Qt::Uninitialized);
            m_sock->readDatagram(discard.data(), int(sz));
            ++m_stats.dropOversize;
            logD(QStringLiteral("丢弃超长组播包: %1 字节").arg(sz));
            continue;
        }

        const QNetworkDatagram dg = m_sock->receiveDatagram(int(sz));
        if (!dg.isValid())
            continue;

        const QHostAddress dst = dg.destinationAddress();
        if (dst.isNull()) {
            if (!m_warnedNoDst) {
                m_warnedNoDst = true;
                logW(QStringLiteral("平台未提供收包目的地址，无法按组过滤；请确保同一端口只有本组流量"));
            }
        } else if (dst != m_group) {
            ++m_stats.dropWrongGroup;
            continue;
        }

        const QByteArray payload = dg.data();
        if (payload.isEmpty()) {
            ++m_stats.dropZero;
            continue;
        }

        ++m_stats.rxPackets;
        m_stats.rxBytes += quint64(payload.size());
        m_queue->push(encodeFrame(kTypeData, payload));
        emit packetQueued();
    }
}

} // namespace mc
