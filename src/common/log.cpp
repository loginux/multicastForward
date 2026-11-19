#include "common/log.h"

#include <QDateTime>

#include <cstdio>
#include <cstdlib>

namespace mc {

LogLevel Log::s_level = LogLevel::Info;
QFile Log::s_file;
QMutex Log::s_mutex;
qint64 Log::s_written = 0;
QString Log::s_path;
LogSink Log::s_sink;

static const qint64 kMaxFileBytes = 8 * 1024 * 1024; // 单文件 8MB 后轮转

bool parseLogLevel(const QString &text, LogLevel *out)
{
    const QString v = text.trimmed().toLower();
    if (v == "error") { *out = LogLevel::Error; return true; }
    if (v == "warn")  { *out = LogLevel::Warn;  return true; }
    if (v == "info")  { *out = LogLevel::Info;  return true; }
    if (v == "debug") { *out = LogLevel::Debug; return true; }
    return false;
}

const char *levelName(LogLevel level)
{
    switch (level) {
    case LogLevel::Error: return "error";
    case LogLevel::Warn:  return "warn";
    case LogLevel::Info:  return "info";
    case LogLevel::Debug: return "debug";
    }
    return "?";
}

void Log::init(const QString &filePath, LogLevel level)
{
    s_level = level;
    s_path = filePath;

    QMutexLocker lock(&s_mutex);
    if (!s_path.isEmpty()) {
        if (s_file.isOpen())
            s_file.close();
        s_file.setFileName(s_path);
        if (!s_file.open(QIODevice::WriteOnly | QIODevice::Append)) {
            fprintf(stderr, "[warn] 无法打开日志文件 %s，仅输出到控制台\n",
                    s_path.toUtf8().constData());
        } else {
            s_written = s_file.size();
        }
    }

    // 接管 Qt 自身的 qDebug/qWarning，保证格式与脱敏策略统一
    qInstallMessageHandler([](QtMsgType type, const QMessageLogContext &, const QString &msg) {
        LogLevel lv = LogLevel::Info;
        switch (type) {
        case QtDebugMsg:    lv = LogLevel::Debug; break;
        case QtInfoMsg:     lv = LogLevel::Info;  break;
        case QtWarningMsg:  lv = LogLevel::Warn;  break;
        case QtCriticalMsg: lv = LogLevel::Error; break;
        case QtFatalMsg:    lv = LogLevel::Error; break;
        }
        Log::write(lv, msg);
        if (type == QtFatalMsg)
            std::abort();
    });
}

void Log::shutdown()
{
    QMutexLocker lock(&s_mutex);
    if (s_file.isOpen())
        s_file.close();
    qInstallMessageHandler(nullptr);
}

void Log::setLevel(LogLevel level)
{
    s_level = level;
}

LogLevel Log::level()
{
    return s_level;
}

void Log::rotateIfNeeded()
{
    if (!s_file.isOpen() || s_written < kMaxFileBytes)
        return;

    s_file.close();
    const QString backup = s_path + ".1";
    QFile::remove(backup);
    QFile::rename(s_path, backup);
    s_file.setFileName(s_path);
    if (s_file.open(QIODevice::WriteOnly | QIODevice::Append))
        s_written = 0;
    else
        s_written = 0;
}

void Log::setSink(LogSink sink)
{
    s_sink = std::move(sink);
}

void Log::write(LogLevel level, const QString &message)
{
    if (int(level) > int(s_level))
        return;

    const QString stamp = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss.zzz");
    const QString body =
        QStringLiteral("%1 [%2] %3").arg(stamp, QString::fromLatin1(levelName(level)), message);
    const QByteArray utf8 = (body + QLatin1Char('\n')).toUtf8();

    {
        QMutexLocker lock(&s_mutex);

        // 直接写 stderr：不依赖控制台句柄，重定向到管道/文件时同样有效
        fwrite(utf8.constData(), 1, size_t(utf8.size()), stderr);
        fflush(stderr);

        if (s_file.isOpen()) {
            s_file.write(utf8);
            s_file.flush();
            s_written += utf8.size();
            rotateIfNeeded();
        }
    }

    // 回调在锁外触发，避免 GUI 回调里再次打日志造成死锁
    if (s_sink)
        s_sink(int(level), body);
}

} // namespace mc
