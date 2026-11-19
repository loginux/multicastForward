#include "common/config.h"

#include <QDir>
#include <QFileInfo>
#include <QSettings>
#include <QStringList>

namespace mc {

namespace {

QString availableInterfaceHint()
{
    QStringList items;
    const auto all = QNetworkInterface::allInterfaces();
    for (const auto &ni : all) {
        const auto flags = ni.flags();
        if (!(flags & QNetworkInterface::IsUp))
            continue;
        for (const auto &e : ni.addressEntries()) {
            if (e.ip().protocol() == QAbstractSocket::IPv4Protocol)
                items << QStringLiteral("%1(%2)").arg(e.ip().toString(), ni.humanReadableName());
        }
    }
    return items.isEmpty() ? QStringLiteral("(无)") : items.join(QStringLiteral(", "));
}

bool validateGroup(const QString &text, QString *err)
{
    QHostAddress addr;
    if (!addr.setAddress(text)) {
        *err = QStringLiteral("组播地址不是合法 IPv4: %1").arg(text);
        return false;
    }
    if (addr.protocol() != QAbstractSocket::IPv4Protocol) {
        *err = QStringLiteral("组播地址必须是 IPv4: %1").arg(text);
        return false;
    }
    const quint32 a = addr.toIPv4Address();
    // 合法范围 224.0.1.0 - 239.255.255.255，剔除 224.0.0.0/24 本地链路组
    if (a < 0xE0000100u || a > 0xEFFFFFFFu) {
        *err = QStringLiteral("组播地址超出可转发范围(224.0.1.0-239.255.255.255): %1").arg(text);
        return false;
    }
    return true;
}

void applyIniCodec(QSettings &s)
{
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    s.setIniCodec("UTF-8");
#else
    Q_UNUSED(s)
#endif
}

} // namespace

namespace {

// 只解析文件，不做业务校验，也不决定角色
bool parseIni(const QString &path, Config *out, QString *err)
{
    if (!QFileInfo::exists(path)) {
        *err = QStringLiteral("配置文件不存在: %1").arg(QDir::toNativeSeparators(path));
        return false;
    }

    QSettings s(path, QSettings::IniFormat);
    applyIniCodec(s);
    if (s.status() != QSettings::NoError) {
        *err = QStringLiteral("配置文件解析失败(需为 INI 格式): %1").arg(path);
        return false;
    }

    out->group     = s.value(QStringLiteral("multicast/group")).toString().trimmed();
    out->mcPort    = quint16(s.value(QStringLiteral("multicast/port")).toUInt());
    out->ifaceAddr = s.value(QStringLiteral("multicast/interface")).toString().trimmed();
    out->ttl       = quint8(s.value(QStringLiteral("multicast/ttl"), 1).toUInt());
    out->loop      = s.value(QStringLiteral("multicast/loop"), true).toBool();

    out->remoteHost = s.value(QStringLiteral("server/host")).toString().trimmed();
    out->remotePort = quint16(s.value(QStringLiteral("server/port")).toUInt());
    out->listenHost = s.value(QStringLiteral("listen/host"), QStringLiteral("0.0.0.0")).toString().trimmed();
    out->listenPort = quint16(s.value(QStringLiteral("listen/port")).toUInt());

    out->password = s.value(QStringLiteral("auth/password")).toString().toUtf8();

    out->queueSize            = s.value(QStringLiteral("runtime/queue_size"), 1024).toInt();
    out->reconnectMinSec      = s.value(QStringLiteral("runtime/reconnect_min"), 1).toInt();
    out->reconnectMaxSec      = s.value(QStringLiteral("runtime/reconnect_max"), 30).toInt();
    out->heartbeatIntervalSec = s.value(QStringLiteral("runtime/heartbeat_interval"), 10).toInt();
    out->heartbeatTimeoutSec  = s.value(QStringLiteral("runtime/heartbeat_timeout"), 30).toInt();
    out->statsIntervalSec     = s.value(QStringLiteral("runtime/stats_interval"), 60).toInt();
    out->logFile              = s.value(QStringLiteral("runtime/log_file")).toString().trimmed();

    const QString lvl = s.value(QStringLiteral("runtime/log_level"), QStringLiteral("info")).toString();
    if (!parseLogLevel(lvl, &out->logLevel)) {
        *err = QStringLiteral("runtime.log_level 非法(可选 error/warn/info/debug): %1").arg(lvl);
        return false;
    }
    return true;
}

} // namespace

bool Config::load(const QString &path, Role roleIn, Config *out, QString *err)
{
    if (!parseIni(path, out, err))
        return false;
    out->role = roleIn;
    return out->validate(err);
}

bool Config::loadAuto(const QString &path, Config *out, QString *err)
{
    Role r = Client;
    {
        if (!QFileInfo::exists(path)) {
            *err = QStringLiteral("配置文件不存在: %1").arg(QDir::toNativeSeparators(path));
            return false;
        }
        QSettings s(path, QSettings::IniFormat);
        applyIniCodec(s);
        // 注意: "general" 是 QSettings INI 的保留段名(映射到无分组的根键)，不能用
        QString t = s.value(QStringLiteral("app/role")).toString().trimmed().toLower();
        if (t.isEmpty())
            t = s.value(QStringLiteral("role")).toString().trimmed().toLower(); // 兼容根键写法
        r = (t == QStringLiteral("server")) ? Server : Client;
    }

    if (!parseIni(path, out, err))
        return false;

    // GUI 场景：只解析不校验，配置有问题时让用户在界面上改
    out->role = r;
    return true;
}

bool Config::save(const QString &path, QString *err) const
{
    const QString dir = QFileInfo(path).absolutePath();
    if (!dir.isEmpty() && !QDir(dir).exists() && !QDir().mkpath(dir)) {
        *err = QStringLiteral("无法创建目录: %1").arg(QDir::toNativeSeparators(dir));
        return false;
    }

    QSettings s(path, QSettings::IniFormat);
    applyIniCodec(s);

    s.setValue(QStringLiteral("app/role"), role == Client ? QStringLiteral("client")
                                                         : QStringLiteral("server"));

    s.setValue(QStringLiteral("multicast/group"), group);
    s.setValue(QStringLiteral("multicast/port"), mcPort);
    s.setValue(QStringLiteral("multicast/interface"), ifaceAddr);
    s.setValue(QStringLiteral("multicast/ttl"), ttl);
    s.setValue(QStringLiteral("multicast/loop"), loop);

    s.setValue(QStringLiteral("server/host"), remoteHost);
    s.setValue(QStringLiteral("server/port"), remotePort);
    s.setValue(QStringLiteral("listen/host"), listenHost);
    s.setValue(QStringLiteral("listen/port"), listenPort);

    s.setValue(QStringLiteral("auth/password"), QString::fromUtf8(password));

    s.setValue(QStringLiteral("runtime/queue_size"), queueSize);
    s.setValue(QStringLiteral("runtime/reconnect_min"), reconnectMinSec);
    s.setValue(QStringLiteral("runtime/reconnect_max"), reconnectMaxSec);
    s.setValue(QStringLiteral("runtime/heartbeat_interval"), heartbeatIntervalSec);
    s.setValue(QStringLiteral("runtime/heartbeat_timeout"), heartbeatTimeoutSec);
    s.setValue(QStringLiteral("runtime/stats_interval"), statsIntervalSec);
    s.setValue(QStringLiteral("runtime/log_level"), QString::fromLatin1(levelName(logLevel)));
    s.setValue(QStringLiteral("runtime/log_file"), logFile);

    s.sync();
    if (s.status() != QSettings::NoError) {
        *err = QStringLiteral("写入配置失败: %1").arg(QDir::toNativeSeparators(path));
        return false;
    }
    return true;
}

bool Config::validate(QString *err) const
{
    if (group.isEmpty()) {
        *err = QStringLiteral("缺少必填项：组播地址");
        return false;
    }
    if (!validateGroup(group, err))
        return false;

    if (mcPort == 0) {
        *err = QStringLiteral("缺少必填项：组播端口");
        return false;
    }
    if (ifaceAddr.isEmpty()) {
        *err = QStringLiteral("缺少必填项：网卡（填本机网卡的 IPv4 地址）");
        return false;
    }
    if (ttl == 0) {
        *err = QStringLiteral("TTL 必须在 1-255 之间");
        return false;
    }
    if (password.isEmpty()) {
        *err = QStringLiteral("缺少必填项：认证口令（不允许为空）");
        return false;
    }
    if (role == Client) {
        if (remoteHost.isEmpty()) {
            *err = QStringLiteral("缺少必填项：服务端地址");
            return false;
        }
        if (remotePort == 0) {
            *err = QStringLiteral("缺少必填项：服务端端口");
            return false;
        }
    } else {
        if (listenPort == 0) {
            *err = QStringLiteral("缺少必填项：监听端口");
            return false;
        }
    }
    if (queueSize < 1) {
        *err = QStringLiteral("队列容量必须大于 0");
        return false;
    }
    if (reconnectMinSec < 1 || reconnectMaxSec < reconnectMinSec) {
        *err = QStringLiteral("重连间隔非法（要求最小值>=1 且 最大值>=最小值）");
        return false;
    }
    if (heartbeatIntervalSec < 1 || heartbeatTimeoutSec < 1) {
        *err = QStringLiteral("心跳参数必须大于 0");
        return false;
    }

    QNetworkInterface ni;
    QHostAddress ip;
    if (!pickInterface(&ni, &ip, err))
        return false;

    if (password.size() < 8)
        logW(QStringLiteral("认证口令长度不足 8 个字符，建议加长"));
    for (char c : password) {
        if (static_cast<unsigned char>(c) > 0x7F) {
            logW(QStringLiteral("认证口令含非 ASCII 字符，两端配置必须同为 UTF-8 编码"));
            break;
        }
    }

    return true;
}

bool Config::pickInterface(QNetworkInterface *ni, QHostAddress *ip, QString *err) const
{
    QHostAddress want;
    if (!want.setAddress(ifaceAddr)) {
        *err = QStringLiteral("网卡地址不是合法 IPv4: %1（可用: %2）")
                   .arg(ifaceAddr, availableInterfaceHint());
        return false;
    }

    const auto all = QNetworkInterface::allInterfaces();
    for (const auto &cand : all) {
        if (!(cand.flags() & QNetworkInterface::IsUp))
            continue;
        for (const auto &e : cand.addressEntries()) {
            if (e.ip() == want) {
                *ni = cand;
                *ip = e.ip();
                return true;
            }
        }
    }

    *err = QStringLiteral("本机找不到地址为 %1 的网卡。可用: %2")
               .arg(ifaceAddr, availableInterfaceHint());
    return false;
}

} // namespace mc
