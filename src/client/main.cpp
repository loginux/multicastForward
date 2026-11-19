#include "client/multicastreceiver.h"
#include "client/tunnelclient.h"
#include "client/txqueue.h"
#include "common/appinit.h"
#include "common/log.h"

#include <QCoreApplication>

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("mc_forward_a"));

    mc::Config cfg;
    const mc::InitResult r = mc::initApp(app.arguments(), mc::Config::Client, &cfg);
    if (r != mc::InitResult::Ok)
        return r == mc::InitResult::ExitOk ? 0 : 1;

    mc::TxQueue queue(cfg.queueSize);
    mc::MulticastReceiver receiver(cfg, &queue);
    mc::TunnelClient client(cfg, &queue);

    QString err;
    if (!receiver.open(&err)) {
        mc::logE(err);
        mc::Log::shutdown();
        return 2;
    }

    QObject::connect(&receiver, &mc::MulticastReceiver::packetQueued,
                     &client, &mc::TunnelClient::pump);
    QObject::connect(&app, &QCoreApplication::aboutToQuit, [&client]() { client.stop(); });

    client.start();
    mc::logI(QStringLiteral("mc_forward_a 启动完成"));

    const int rc = app.exec();
    {
        const mc::Stats st = client.stats();
        mc::logI(QStringLiteral("退出。进口 %1 包 / %2，出口 %3 帧 / %4")
                     .arg(st.rxPackets).arg(mc::formatBytes(st.rxBytes))
                     .arg(st.txFrames).arg(mc::formatBytes(st.txBytes)));
    }
    mc::Log::shutdown();
    return rc;
}
