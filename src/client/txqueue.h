#pragma once

#include <QByteArray>

#include <vector>

namespace mc {

// 有界环形队列：满时丢弃最旧帧（实时性优先）
class TxQueue
{
public:
    explicit TxQueue(int capacity);

    void push(const QByteArray &frame);
    bool pop(QByteArray *out);
    void clear();

    int size() const { return m_count; }
    int capacity() const { return m_cap; }
    quint64 dropped() const { return m_dropped; }

private:
    std::vector<QByteArray> m_buf;
    int m_cap = 0;
    int m_head = 0;
    int m_count = 0;
    quint64 m_dropped = 0;
};

} // namespace mc
