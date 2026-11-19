#include "common/appinit.h"
#include "common/log.h"
#include "server/multicastsender.h"
#include "server/tunnelserver.h"

#include <QCoreApplication>

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("mc_forward_b"));

    mc::Config cfg;
    const mc::InitResult r = mc::initApp(app.arguments(), mc::Config::Server, &cfg);
    if (r != mc::InitResult::Ok)
        return r == mc::InitResult::ExitOk ? 0 : 1;

    mc::MulticastSender sender(cfg);
    QString err;
    if (!sender.open(&err)) {
        mc::logE(err);
        mc::Log::shutdown();
        return 2;
    }

    mc::TunnelServer server(cfg, &sender);
    if (!server.listen(&err)) {
        mc::logE(err);
        mc::Log::shutdown();
        return 2;
    }

    mc::logI(QStringLiteral("mc_forward_b 启动完成"));

    const int rc = app.exec();
    {
        const mc::Stats st = server.stats();
        mc::logI(QStringLiteral("退出。进口 %1 帧 / %2，出口 %3 包 / %4")
                     .arg(st.framesIn).arg(mc::formatBytes(st.framesInBytes))
                     .arg(st.mcSent).arg(mc::formatBytes(st.mcSentBytes)));
    }
    mc::Log::shutdown();
    return rc;
}
