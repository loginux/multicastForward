#pragma once

#include <QByteArray>
#include <QString>

#include <functional>

namespace mc {

// 远程更新的同步上传器：连接 → 握手 → 分块推送 → 等结果
// 传 1KB 一块，与数据帧同量级，避免大帧带来的缓冲与 MTU 顾虑（TCP 本身不分片，这里只是保守）
struct UpdateResult
{
    bool ok = false;
    bool serverWillRestart = false;
    QString message;
};

class UpdateClient
{
public:
    // onProgress 收到 0..100，可为空；返回前会阻塞当前线程
    static UpdateResult push(const QString &host,
                             quint16 port,
                             const QByteArray &password,
                             const QString &localFile,
                             const std::function<void(int)> &onProgress);
};

} // namespace mc
