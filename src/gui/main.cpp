#include "common/log.h"
#include "gui/mainwindow.h"
#include "gitinfo.h"
#include "gui/updateclient.h"

#include <QApplication>
#include <QFile>
#include <QFont>
#include <QIcon>
#include <QTimer>

namespace {

// 命令行推送更新（供自动化验证与脚本化运维使用）：
//   multicastForward.exe --push-update <host:port> --password <口令> --file <本地 exe>
// 结果同时写入 exe 同目录的 update-result.txt，退出码 0=成功
int runPushUpdate(const QStringList &args)
{
    const int uIdx = args.indexOf(QStringLiteral("--push-update"));
    const QString target = args.value(uIdx + 1);
    const int pIdx = args.indexOf(QStringLiteral("--password"));
    const int fIdx = args.indexOf(QStringLiteral("--file"));

    QString host;
    quint16 port = 0;
    const int colon = target.lastIndexOf(QLatin1Char(':'));
    if (colon > 0) {
        host = target.left(colon);
        port = quint16(target.mid(colon + 1).toUInt());
    }
    const QString file = (fIdx >= 0) ? args.value(fIdx + 1) : QString();
    const QString pw = (pIdx >= 0) ? args.value(pIdx + 1) : QString();

    mc::UpdateResult r;
    if (host.isEmpty() || port == 0 || file.isEmpty()) {
        r.message = QStringLiteral(
            "参数不完整：--push-update host:port --password <口令> --file <本地 exe>");
    } else {
        r = mc::UpdateClient::push(host, port, pw.toUtf8(), file, nullptr);
    }

    QFile out(QCoreApplication::applicationDirPath() + QStringLiteral("/update-result.txt"));
    if (out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        out.write(QStringLiteral("%1\n%2\n")
                      .arg(r.ok ? QStringLiteral("OK") : QStringLiteral("FAIL"), r.message)
                      .toUtf8());
        out.close();
    }
    return r.ok ? 0 : 1;
}

} // namespace

int main(int argc, char *argv[])
{
    // 高分辨率屏幕必须开这个：否则 4K 上整窗按 96 DPI 画，字号显得极小
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    QCoreApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
    QCoreApplication::setAttribute(Qt::AA_UseHighDpiPixmaps);
#endif

    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("multicastForward"));
    QApplication::setApplicationVersion(QStringLiteral(MCFW_VERSION_FULL));
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/app.ico")));

    const QStringList args = app.arguments();

    // 命令行推送更新，不进入界面
    if (args.contains(QStringLiteral("--push-update")))
        return runPushUpdate(args);

    QFont f = app.font();
    f.setFamily(QStringLiteral("Microsoft YaHei"));
    f.setPointSize(9);
    app.setFont(f);

    mc::MainWindow w;
    w.show();

    // 便于生成界面截图：multicastForward.exe --screenshot out.png [--screenshot-delay 毫秒]
    const int idx = args.indexOf(QStringLiteral("--screenshot"));
    if (idx >= 0 && idx + 1 < args.size()) {
        const QString path = args.at(idx + 1);
        int delayMs = 900;
        const int dIdx = args.indexOf(QStringLiteral("--screenshot-delay"));
        if (dIdx >= 0 && dIdx + 1 < args.size())
            delayMs = qMax(200, args.at(dIdx + 1).toInt());
        QTimer::singleShot(delayMs, &w, [&w, path]() {
            w.grab().save(path);
            QCoreApplication::quit();
        });
    }

    // 启动后自动开始转发（用于自检/无人值守）
    if (args.contains(QStringLiteral("--autostart")))
        QTimer::singleShot(200, &w, [&w]() { QMetaObject::invokeMethod(&w, "onStartClicked"); });

    const int rc = app.exec();
    mc::Log::shutdown();
    return rc;
}
