#include "client/txqueue.h"

#include <utility>

namespace mc {

TxQueue::TxQueue(int capacity)
    : m_buf(size_t(capacity > 0 ? capacity : 1))
    , m_cap(capacity > 0 ? capacity : 1)
{
}

void TxQueue::push(const QByteArray &frame)
{
    if (m_count == m_cap) {
        // 丢最旧
        m_head = (m_head + 1) % m_cap;
        --m_count;
        ++m_dropped;
    }
    m_buf[size_t((m_head + m_count) % m_cap)] = frame;
    ++m_count;
}

bool TxQueue::pop(QByteArray *out)
{
    if (m_count == 0)
        return false;
    *out = std::move(m_buf[size_t(m_head)]);
    m_head = (m_head + 1) % m_cap;
    --m_count;
    return true;
}

void TxQueue::clear()
{
    m_head = 0;
    m_count = 0;
    for (auto &b : m_buf)
        b.clear();
}

} // namespace mc
