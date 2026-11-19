#pragma once

#include "common/config.h"

#include <QStringList>

namespace mc {

enum class InitResult { Ok, ExitOk, ExitError };

// 解析命令行、加载并校验配置、初始化日志、安装退出信号处理
InitResult initApp(const QStringList &args, Config::Role role, Config *cfg);

} // namespace mc
