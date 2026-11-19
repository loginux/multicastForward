#include "server/multicastsender.h"

#include "common/log.h"

namespace mc {

MulticastSender::MulticastSender(const Config &cfg)
    : m_cfg(cfg)
{
}

bool MulticastSender::open(QString *err)
{
    QHostAddress localIp;
    if (!m_cfg.pickInterface(&m_iface, &localIp, err))
        return false;

    if (!m_group.setAddress(m_cfg.group)) {
        *err = QStringLiteral("组播地址非法: %1").arg(m_cfg.group);
        return false;
    }

    if (!m_sock.bind(QHostAddress(QHostAddress::AnyIPv4), 0)) {
        *err = QStringLiteral("发送 socket 绑定失败: %1").arg(m_sock.errorString());
        return false;
    }

    m_sock.setMulticastInterface(m_iface);
    m_sock.setSocketOption(QAbstractSocket::MulticastTtlOption, int(m_cfg.ttl));
    m_sock.setSocketOption(QAbstractSocket::MulticastLoopbackOption, m_cfg.loop ? 1 : 0);

    logI(QStringLiteral("组播发送就绪: group=%1 port=%2 iface=%3(%4) ttl=%5 loop=%6")
             .arg(m_cfg.group)
             .arg(m_cfg.mcPort)
             .arg(localIp.toString(), m_iface.humanReadableName())
             .arg(m_cfg.ttl)
             .arg(m_cfg.loop ? "on" : "off"));
    return true;
}

bool MulticastSender::send(const QByteArray &payload)
{
    const qint64 n = m_sock.writeDatagram(payload, m_group, m_cfg.mcPort);
    return n == qint64(payload.size());
}

} // namespace mc
