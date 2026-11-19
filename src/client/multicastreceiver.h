#pragma once

#include "client/txqueue.h"
#include "common/config.h"
#include "common/stats.h"

#include <QHostAddress>
#include <QNetworkInterface>
#include <QObject>

class QUdpSocket;

namespace mc {

// 在指定网卡上入组，接收组播并按协议封帧后压入队列
class MulticastReceiver : public QObject
{
    Q_OBJECT
public:
    MulticastReceiver(const Config &cfg, TxQueue *queue, QObject *parent = nullptr);

    bool open(QString *err);
    const Stats &stats() const { return m_stats; }

signals:
    void packetQueued();

private slots:
    void onReadyRead();

private:
    Config m_cfg;
    TxQueue *m_queue = nullptr;
    QUdpSocket *m_sock = nullptr;
    QHostAddress m_group;
    QNetworkInterface m_iface;
    Stats m_stats;
    bool m_warnedNoDst = false;
};

} // namespace mc
