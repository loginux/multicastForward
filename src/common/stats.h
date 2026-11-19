#pragma once

#include <QString>

namespace mc {

// 字节数转成便于阅读的形式：B / KB / MB
inline QString formatBytes(quint64 n)
{
    if (n < 1024)
        return QStringLiteral("%1 B").arg(n);
    const double kb = double(n) / 1024.0;
    if (kb < 1024.0)
        return QStringLiteral("%1 KB").arg(kb, 0, 'f', 1);
    return QStringLiteral("%1 MB").arg(kb / 1024.0, 0, 'f', 2);
}

// 两端共用的统计项，未涉及的字段保持 0
struct Stats
{
    // 主机 A
    quint64 rxPackets = 0;
    quint64 rxBytes = 0;
    quint64 dropOversize = 0;
    quint64 dropZero = 0;
    quint64 dropWrongGroup = 0;
    quint64 dropQueueFull = 0;
    quint64 txFrames = 0;
    quint64 txBytes = 0;   // 写进隧道的载荷字节（不含 3 字节帧头）
    quint64 reconnects = 0;
    quint64 authFails = 0;

    // 主机 B
    quint64 framesIn = 0;
    quint64 framesInBytes = 0; // 从隧道收到的载荷字节
    quint64 mcSent = 0;
    quint64 mcSentBytes = 0;   // 组播成功发出的载荷字节
    quint64 mcSendFail = 0;
    quint64 illegalFrames = 0;

    // 公共
    int queueDepth = 0;
    int connected = 0;

    QString toLogLine() const
    {
        return QStringLiteral(
                   "rx_packets=%1 rx_bytes=%2 drop_oversize=%3 drop_zero=%4 "
                   "drop_wrong_group=%5 drop_queue_full=%6 tx_frames=%7 "
                   "reconnects=%8 auth_fail=%9 queue_depth=%10 conn=%11 "
                   "frames_in=%12 mc_sent=%13 mc_send_fail=%14 illegal_frames=%15 "
                   "tx_bytes=%16 frames_in_bytes=%17 mc_sent_bytes=%18")
            .arg(rxPackets)
            .arg(rxBytes)
            .arg(dropOversize)
            .arg(dropZero)
            .arg(dropWrongGroup)
            .arg(dropQueueFull)
            .arg(txFrames)
            .arg(reconnects)
            .arg(authFails)
            .arg(queueDepth)
            .arg(connected)
            .arg(framesIn)
            .arg(mcSent)
            .arg(mcSendFail)
            .arg(illegalFrames)
            .arg(txBytes)
            .arg(framesInBytes)
            .arg(mcSentBytes);
    }
};

} // namespace mc
