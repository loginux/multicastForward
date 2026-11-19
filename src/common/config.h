#pragma once

#include "common/log.h"

#include <QByteArray>
#include <QHostAddress>
#include <QNetworkInterface>
#include <QString>

namespace mc {

struct Config
{
    enum Role { Client, Server };

    Role role = Client;

    // [multicast]
    QString group;
    quint16 mcPort = 0;
    QString ifaceAddr;
    quint8  ttl = 1;
    bool    loop = true;

    // [server] 仅主机 A 使用
    QString remoteHost;
    quint16 remotePort = 0;

    // [listen] 仅主机 B 使用
    QString listenHost = QStringLiteral("0.0.0.0");
    quint16 listenPort = 0;

    // [auth]
    QByteArray password;

    // [runtime]
    int queueSize = 1024;
    int reconnectMinSec = 1;
    int reconnectMaxSec = 30;
    int heartbeatIntervalSec = 10;   // A 发 PING 的周期
    int heartbeatTimeoutSec = 30;    // B 判定失联的超时
    int statsIntervalSec = 60;
    LogLevel logLevel = LogLevel::Info;
    QString logFile;

    // 加载并强制指定角色（命令行程序用）
    static bool load(const QString &path, Role role, Config *out, QString *err);

    // 从配置文件里的 [general] role 推断角色（GUI 用）
    static bool loadAuto(const QString &path, Config *out, QString *err);

    // 写出为 INI；不要求配置先通过校验
    bool save(const QString &path, QString *err) const;

    // 启动前校验，含网卡存在性检查
    bool validate(QString *err) const;

    // 按 ifaceAddr 反查网卡，失败时给出可用地址列表
    bool pickInterface(QNetworkInterface *ni, QHostAddress *ip, QString *err) const;
};

} // namespace mc
