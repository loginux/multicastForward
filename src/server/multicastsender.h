#pragma once

#include "common/config.h"

#include <QHostAddress>
#include <QNetworkInterface>
#include <QString>
#include <QUdpSocket>

namespace mc {

// 从指定出口网卡以组播形式发出载荷
class MulticastSender
{
public:
    explicit MulticastSender(const Config &cfg);

    bool open(QString *err);
    bool send(const QByteArray &payload);

private:
    Config m_cfg;
    QUdpSocket m_sock;
    QHostAddress m_group;
    QNetworkInterface m_iface;
};

} // namespace mc
