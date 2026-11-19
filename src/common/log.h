#pragma once

#include <QFile>
#include <QMutex>
#include <QString>

#include <functional>

namespace mc {

enum class LogLevel { Error = 0, Warn = 1, Info = 2, Debug = 3 };

bool parseLogLevel(const QString &text, LogLevel *out);
const char *levelName(LogLevel level);

// 日志回调：level 为 LogLevel 的整数值，line 不含换行符
using LogSink = std::function<void(int level, const QString &line)>;

class Log
{
public:
    // filePath 为空表示只输出到控制台
    static void init(const QString &filePath, LogLevel level);
    static void shutdown();
    static void setLevel(LogLevel level);
    static LogLevel level();

    // 供 GUI 把日志接到窗口；传入空函数对象可解除
    static void setSink(LogSink sink);

    static void write(LogLevel level, const QString &message);

private:
    static void rotateIfNeeded();

    static LogLevel s_level;
    static QFile s_file;
    static QMutex s_mutex;
    static qint64 s_written;
    static QString s_path;
    static LogSink s_sink;
};

inline void logE(const QString &m) { Log::write(LogLevel::Error, m); }
inline void logW(const QString &m) { Log::write(LogLevel::Warn, m); }
inline void logI(const QString &m) { Log::write(LogLevel::Info, m); }
inline void logD(const QString &m) { Log::write(LogLevel::Debug, m); }

} // namespace mc
