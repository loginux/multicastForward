#include "common/protocol.h"

#include <QCryptographicHash>
#include <QMessageAuthenticationCode>
#include <QRandomGenerator>

namespace mc {

namespace {
const char kMagicStr[4] = {'M', 'C', 'F', 'W'};

inline bool isKnownType(quint8 t)
{
    return t == kTypeData || t == kTypePing || t == kTypePong
        || t == kTypeUpdateBegin || t == kTypeUpdateChunk
        || t == kTypeUpdateEnd || t == kTypeUpdateResult;
}
} // namespace

QByteArray makeHello()
{
    QByteArray b;
    b.reserve(kHelloLen);
    b.append(kMagicStr, 4);
    b.append(char(kVersion));
    b.append(char(0x00)); // flags
    b.append(char(0x00)); // reserved hi
    b.append(char(0x00)); // reserved lo
    return b;
}

QByteArray encodeFrame(quint8 type, const QByteArray &payload)
{
    QByteArray b;
    b.reserve(kHeaderLen + payload.size());
    b.append(char(type));
    b.append(char((payload.size() >> 8) & 0xFF));
    b.append(char(payload.size() & 0xFF));
    b.append(payload);
    return b;
}

QByteArray encodePing()
{
    return encodeFrame(kTypePing, QByteArray());
}

QByteArray encodePong()
{
    return encodeFrame(kTypePong, QByteArray());
}

QByteArray computeAuth(const QByteArray &password, const QByteArray &nonce)
{
    QByteArray msg;
    msg.reserve(5 + nonce.size());
    msg.append(kMagicStr, 4);
    msg.append(char(kVersion));
    msg.append(nonce);
    return QMessageAuthenticationCode::hash(msg, password, QCryptographicHash::Sha256);
}

QByteArray makeNonce()
{
    QByteArray nonce(kChallengeLen, Qt::Uninitialized);
    auto *rng = QRandomGenerator::system();
    for (int i = 0; i < kChallengeLen; ++i)
        nonce[i] = char(rng->generate() & 0xFF);
    return nonce;
}

QByteArray makeUpdateBegin(quint32 size, const QByteArray &sha256)
{
    QByteArray p;
    p.reserve(kUpdateBeginLen);
    p.append(char((size >> 24) & 0xFF));
    p.append(char((size >> 16) & 0xFF));
    p.append(char((size >> 8) & 0xFF));
    p.append(char(size & 0xFF));
    p.append(sha256.left(kAuthLen));
    return encodeFrame(kTypeUpdateBegin, p);
}

bool parseUpdateBegin(const QByteArray &payload, UpdateBegin *out, QString *why)
{
    if (payload.size() != kUpdateBeginLen) {
        *why = QStringLiteral("UPDATE_BEGIN 长度非法: %1").arg(payload.size());
        return false;
    }
    const auto *p = reinterpret_cast<const quint8 *>(payload.constData());
    out->size = (quint32(p[0]) << 24) | (quint32(p[1]) << 16)
              | (quint32(p[2]) << 8) | quint32(p[3]);
    out->sha256 = payload.mid(4, kAuthLen);
    if (out->size == 0) {
        *why = QStringLiteral("UPDATE_BEGIN 声明的文件长度为 0");
        return false;
    }
    return true;
}

QByteArray makeUpdateChunk(quint32 offset, const QByteArray &data)
{
    QByteArray p;
    p.reserve(4 + data.size());
    p.append(char((offset >> 24) & 0xFF));
    p.append(char((offset >> 16) & 0xFF));
    p.append(char((offset >> 8) & 0xFF));
    p.append(char(offset & 0xFF));
    p.append(data);
    return encodeFrame(kTypeUpdateChunk, p);
}

bool parseUpdateChunk(const QByteArray &payload, quint32 *offset, QByteArray *data, QString *why)
{
    if (payload.size() < 5) {
        *why = QStringLiteral("UPDATE_CHUNK 长度非法: %1").arg(payload.size());
        return false;
    }
    const auto *p = reinterpret_cast<const quint8 *>(payload.constData());
    *offset = (quint32(p[0]) << 24) | (quint32(p[1]) << 16)
            | (quint32(p[2]) << 8) | quint32(p[3]);
    *data = payload.mid(4);
    if (data->size() > kUpdateChunkSize) {
        *why = QStringLiteral("UPDATE_CHUNK 数据超过 %1 字节").arg(kUpdateChunkSize);
        return false;
    }
    return true;
}

QByteArray makeUpdateEnd()
{
    return encodeFrame(kTypeUpdateEnd, QByteArray());
}

QByteArray makeUpdateResult(quint8 code, const QString &message)
{
    QByteArray p;
    p.append(char(code));
    if (!message.isEmpty())
        p.append(message.toUtf8().left(kUpdateResultMax - 1));
    return encodeFrame(kTypeUpdateResult, p);
}

bool parseUpdateResult(const QByteArray &payload, quint8 *code, QString *message)
{
    if (payload.isEmpty())
        return false;
    *code = quint8(payload.at(0));
    *message = QString::fromUtf8(payload.mid(1));
    return true;
}

FrameReader::Result FrameReader::next(quint8 *type, QByteArray *payload, QString *why)
{
    if (m_buf.size() < kHeaderLen)
        return NeedMore;

    const quint8 t = quint8(m_buf.at(0));
    if (!isKnownType(t)) {
        *why = QStringLiteral("未知帧类型 0x%1").arg(t, 2, 16, QLatin1Char('0'));
        return Illegal;
    }

    const int len = (quint8(m_buf.at(1)) << 8) | quint8(m_buf.at(2));
    switch (t) {
    case kTypeData:
        if (len < 1 || len > kMaxPayload) {
            *why = QStringLiteral("数据帧长度非法: %1").arg(len);
            return Illegal;
        }
        break;
    case kTypePing:
    case kTypePong:
    case kTypeUpdateEnd:
        if (len != 0) {
            *why = QStringLiteral("控制帧长度必须为 0，实际 %1").arg(len);
            return Illegal;
        }
        break;
    case kTypeUpdateBegin:
        if (len != kUpdateBeginLen) {
            *why = QStringLiteral("UPDATE_BEGIN 长度非法: %1").arg(len);
            return Illegal;
        }
        break;
    case kTypeUpdateChunk:
        if (len < 5 || len > 4 + kUpdateChunkSize) {
            *why = QStringLiteral("UPDATE_CHUNK 长度非法: %1").arg(len);
            return Illegal;
        }
        break;
    case kTypeUpdateResult:
        if (len < 1 || len > kUpdateResultMax) {
            *why = QStringLiteral("UPDATE_RESULT 长度非法: %1").arg(len);
            return Illegal;
        }
        break;
    default:
        break;
    }

    if (m_buf.size() < kHeaderLen + len) {
        if (m_buf.size() > kRxBufferLimit) {
            *why = QStringLiteral("接收缓冲超过上限且无法解出完整帧");
            return Illegal;
        }
        return NeedMore;
    }

    *type = t;
    *payload = m_buf.mid(kHeaderLen, len);
    m_buf.remove(0, kHeaderLen + len);
    return Ok;
}

} // namespace mc
