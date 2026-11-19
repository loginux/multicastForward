#pragma once

#include <QByteArray>
#include <QString>

namespace mc {

// ---- 协议常量（与 docs/协议规格.md 保持一致）----
constexpr int kHelloLen     = 8;
constexpr int kChallengeLen = 16;
constexpr int kAuthLen      = 32;
constexpr int kHeaderLen    = 3;
constexpr int kMaxPayload   = 1024;
constexpr int kMaxFrameLen  = kHeaderLen + kMaxPayload; // 1027

// 远程更新分块：1KB 一包，与数据帧同量级，不触碰 MTU / 接收缓冲上限
constexpr int kUpdateChunkSize = 1024;
constexpr int kUpdateBeginLen  = 36;   // 4 字节总长 + 32 字节 SHA-256
constexpr int kUpdateResultMax = 256;  // 1 字节结果码 + 说明文字

constexpr int kRxBufferLimit = 4096;

constexpr quint8 kVersion  = 0x01;
constexpr quint8 kTypeData = 0x01;
constexpr quint8 kTypePing = 0x02;
constexpr quint8 kTypePong = 0x03;

// 远程更新（仅 GUI 形态使用）
constexpr quint8 kTypeUpdateBegin  = 0x10;
constexpr quint8 kTypeUpdateChunk  = 0x11;
constexpr quint8 kTypeUpdateEnd    = 0x12;
constexpr quint8 kTypeUpdateResult = 0x13;

constexpr quint8 kAuthOk     = 0x00;
constexpr quint8 kAuthFailed = 0x01;

// 更新结果码
constexpr quint8 kUpdateOk        = 0x00; // 校验通过，服务端即将重启
constexpr quint8 kUpdateHashFail  = 0x01; // 哈希不一致或长度不符
constexpr quint8 kUpdateWriteFail = 0x02; // 落盘失败
constexpr quint8 kUpdateBadReq    = 0x03; // 请求非法（长度/顺序）
constexpr quint8 kUpdateBusy      = 0x04; // 已有更新在传输

// UPDATE_BEGIN 载荷
struct UpdateBegin
{
    quint32 size = 0;
    QByteArray sha256; // 32 字节
};

QByteArray makeHello();
QByteArray encodeFrame(quint8 type, const QByteArray &payload);
QByteArray encodePing();
QByteArray encodePong();

QByteArray makeUpdateBegin(quint32 size, const QByteArray &sha256);
bool parseUpdateBegin(const QByteArray &payload, UpdateBegin *out, QString *why);

QByteArray makeUpdateChunk(quint32 offset, const QByteArray &data);
bool parseUpdateChunk(const QByteArray &payload, quint32 *offset, QByteArray *data, QString *why);

QByteArray makeUpdateEnd();
QByteArray makeUpdateResult(quint8 code, const QString &message = QString());
bool parseUpdateResult(const QByteArray &payload, quint8 *code, QString *message);

// HMAC-SHA256(key = password(UTF-8), data = "MCFW" || version || nonce)
QByteArray computeAuth(const QByteArray &password, const QByteArray &nonce);

// 16 字节密码学安全随机数
QByteArray makeNonce();

// 增量解帧，处理 TCP 半包/粘包
class FrameReader
{
public:
    enum Result { NeedMore, Ok, Illegal };

    void feed(const QByteArray &bytes) { m_buf.append(bytes); }
    Result next(quint8 *type, QByteArray *payload, QString *why);
    int buffered() const { return m_buf.size(); }
    void reset() { m_buf.clear(); }

private:
    QByteArray m_buf;
};

} // namespace mc
