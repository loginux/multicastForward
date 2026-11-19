组播转发程序 V1.00 使用说明
（版本号后的括号是编译时的 git 提交号，便于现场核对是哪一版）
========================================

一、目录内容
  multicastForward.exe          主程序（图形界面，客户端/服务端同一个程序）
  multicastForwardLauncher.exe 启动器（后台守护，只在通知区域留图标）
  client.ini                   客户端（A 侧）配置模板
  server.ini                   服务端（B 侧）配置模板
  使用说明.txt                 本文件

二、第一步：选配置
  把要用的那份复制成 config.ini：
      copy client.ini config.ini      客户端：接收组播，连服务端
      copy server.ini config.ini      服务端：监听端口，组播发出
  也可以直接双击 multicastForward.exe，用【文件 → 导入配置】选择后自动保存。
  程序只认同目录的 config.ini；没有它就用内置默认值，界面里改完点【启动】会生成。

三、第二步：按现场网络改 config.ini
  [multicast] interface   必填本机网卡的 IPv4 地址（填网卡名称会启动失败）
  [multicast] group/port  要收（客户端）或要发（服务端）的组播组与端口
  [server]    host/port   客户端填服务端地址；两端 TCP 端口要一致
  [auth]      password    两端必须相同
  [runtime]   log_level   error / warn / info / debug

四、运行
  推荐：双击 multicastForwardLauncher.exe
        · 启动后不显示窗口，只在右下角通知区域留一个图标，不易被误关
        · 主程序被关闭后 3 秒自动拉起（连续启动失败会退避到 6/12/24/30 秒）
        · 托盘图标提示为「组播转发启动器 V1.00 (提交号) · 守护中」，右键可操作
  · 右键图标：【显示主程序窗口】【打开运行日志】【退出守护】
        · 只有右键退出才能结束启动器；退出不会关闭正在运行的主程序
  调试：直接双击 multicastForward.exe，界面点【启动】。此时没有守护，
        关掉主程序不会自动拉起。

五、开机自启（可选）
  任务计划程序 → 创建任务 → 触发器“登录时” → 操作指向
  multicastForwardLauncher.exe，起始位置填本目录。

六、客户端机器（A 侧）首次运行前
  以管理员身份运行仓库里的 allow_firewall.bat 放行入站 UDP，
  否则组播入组成功但收不到包。

七、找不到通知区域图标？
  Windows 默认把新加的图标放进任务栏 ^ （隐藏图标）里，不在时钟旁边。
  · 点任务栏的 ^ 展开，就能看到“组播转发启动器”，右键可操作
  · 想固定到可见区：设置 → 个性化 → 任务栏 → 选择哪些图标显示在任务栏上
    → 把 multicastForwardLauncher.exe 对应项打开（或从 ^ 里直接拖出来）
  · 图标实在找不到时，直接再双击一次 multicastForwardLauncher.exe：
    会弹提示框，可以【显示主程序窗口】或【退出守护】，不依赖图标

八、远程升级
  客户端菜单【工具 → 远程更新服务端…】选择新的 multicastForward.exe 推送。
  服务端校验 SHA-256 后由启动器替换并重启，新版本起不来会自动回滚旧版本。
  只换 exe，新版本不要引入新的 DLL 依赖。
