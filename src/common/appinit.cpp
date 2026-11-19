#include "common/appinit.h"

#include "common/log.h"

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>

#include <csignal>
#include <cstdio>

#ifdef Q_OS_WIN
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace mc {

static const char *kAppVersion = "1.0.0";

#ifdef Q_OS_WIN
static BOOL WINAPI consoleCtrlHandler(DWORD type)
{
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT) {
        QCoreApplication::quit();
        // 给事件循环一点时间做优雅退出；CLOSE 事件必须尽快返回
        if (type == CTRL_CLOSE_EVENT)
            Sleep(200);
        return TRUE;
    }
    return FALSE;
}
#else
static void posixSignalHandler(int)
{
    QCoreApplication::quit();
}
#endif

InitResult initApp(const QStringList &args, Config::Role role, Config *cfg)
{
#ifdef Q_OS_WIN
    // 中文日志必须显式切到 UTF-8 代码页，否则控制台乱码
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCtrlHandler(consoleCtrlHandler, TRUE);
#else
    std::signal(SIGINT, posixSignalHandler);
    std::signal(SIGTERM, posixSignalHandler);
#endif

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("组播隧道（单向 TCP 组播转发）"));
    const QString defaultCfg = (role == Config::Client) ? QStringLiteral("client.ini")
                                                        : QStringLiteral("server.ini");
    const QCommandLineOption optConfig(
        {QStringLiteral("c"), QStringLiteral("config")},
        QStringLiteral("配置文件路径（默认 %1，相对当前目录，可用 conf/%1）").arg(defaultCfg),
        QStringLiteral("path"), defaultCfg);
    const QCommandLineOption optLogLevel(
        QStringLiteral("log-level"),
        QStringLiteral("覆盖日志级别: error|warn|info|debug"), QStringLiteral("level"));
    const QCommandLineOption optVersion(
        {QStringLiteral("v"), QStringLiteral("version")}, QStringLiteral("显示版本号"));
    parser.addOption(optConfig);
    parser.addOption(optLogLevel);
    parser.addOption(optVersion);
    parser.addHelpOption();

    if (!parser.parse(args)) {
        fprintf(stderr, "%s\n", parser.errorText().toUtf8().constData());
        return InitResult::ExitError;
    }
    if (parser.isSet(QStringLiteral("help"))) {
        printf("%s", parser.helpText().toUtf8().constData());
        return InitResult::ExitOk;
    }
    if (parser.isSet(optVersion)) {
        printf("%s %s\n", QCoreApplication::applicationName().toUtf8().constData(), kAppVersion);
        return InitResult::ExitOk;
    }

    QString err;
    if (!Config::load(parser.value(optConfig), role, cfg, &err)) {
        fprintf(stderr, "[error] %s\n", err.toUtf8().constData());
        return InitResult::ExitError;
    }

    if (parser.isSet(optLogLevel)) {
        LogLevel lv;
        const QString text = parser.value(optLogLevel);
        if (!parseLogLevel(text, &lv)) {
            fprintf(stderr, "[error] --log-level 非法: %s\n", text.toUtf8().constData());
            return InitResult::ExitError;
        }
        cfg->logLevel = lv;
    }

    Log::init(cfg->logFile, cfg->logLevel);
    return InitResult::Ok;
}

} // namespace mc
