## 2026-10-22：建立摄像头到 LCD 的基础流水线

&emsp;&emsp;这一阶段完成 V4L2、framebuffer、JPEG 和 HTTP 模块的基础串接。采集线程从摄像头取得 YUYV 帧，处理线程完成 YUYV 到 BGR 的转换、LCD 缩放显示和 JPEG 编码，主线程把最新 JPEG 发布给 HTTP 客户端。

&emsp;&emsp;方案取舍：采用 V4L2 MMAP 加一次用户态复制。直接在驱动映射区上处理可以减少一次复制，但驱动归还缓冲区后数据所有权不再稳定，会把采集、编码和队列生命周期绑定在一起；复制到 `Frame` 后，工作线程可以独立持有数据，关闭流程也更容易验证。队列采用固定容量并丢弃最旧帧，替代无限队列，因为视频应用更看重当前画面延迟，无限积压会让网页看到越来越旧的帧并消耗内存。

&emsp;&emsp;风险与边界：`bytesperline` 可能大于 `width * 2`，所以有效长度按 `(height - 1) * bytesperline + width * 2` 检查；只使用偶数宽度，避免 YUYV 的两个像素一组被截断。采集、编码和 HTTP 由不同线程执行，队列关闭必须唤醒等待者，退出时先关闭队列并 `join`，再释放摄像头和显存，避免工作线程访问已释放资源。

&emsp;&emsp;调试定位：早期黑边和画面偏移来自把可见坐标直接乘以 `width * bytes_per_pixel`，忽略了 framebuffer 的 `xoffset`、`yoffset` 和 `line_length`。通过构造带行填充、窗口偏移和保护区的测试显存，检查保护区是否被改写，最终统一使用 framebuffer 地址公式并补充 `test_video` 覆盖。

&emsp;&emsp;性能观测：首先用 `top` 观察进程 CPU，再用 `v4l2-ctl` 检查格式和帧率声明，用日志记录处理帧率。主要成本是 BGR 转换、逐像素 LCD 写入和软件 JPEG；后续优化方向是复用转换结果、限制队列长度和减少不必要的帧处理。

## 2026-10-24 00:07：加入 Haar 人脸检测和灰度比对

&emsp;&emsp;人脸分析使用 OpenCV Haar cascade，在独立 `FaceAnalysisWorker` 线程中处理抽样 BGR 帧；检测图宽度限制为 240 像素，检测框再映射回原图。可选参考图会被归一化为 64x64 灰度图，通过均衡化和平均绝对差计算 0..1 相似度。

&emsp;&emsp;方案取舍：选择 Haar cascade 是因为模型文件小、依赖简单，适合 i.MX6ULL 的部署和学习；替代方案是深度人脸检测或特征向量识别，鲁棒性更强但模型、内存和算力成本更高。这里的灰度差值只作为演示匹配，不能替代身份认证。

&emsp;&emsp;风险与边界：检测分辨率降低后，小脸、侧脸和遮挡场景更容易漏检；参考图为空时仍可检测，但不产生有效相似度。检测任务邮箱只保留一帧，新帧覆盖旧任务，避免检测延迟无限增长；停止时先关闭邮箱并唤醒条件变量，再等待分析线程退出。

&emsp;&emsp;调试定位：模型路径错误曾导致分析初始化失败，但不应阻塞摄像头、LCD 和 HTTP。定位时区分“模型加载失败”和“没有检测到人脸”两类日志，并使用 `test_face_analyzer` 验证无效图片、空图像、相似度边界和 Haar 模型加载路径。

&emsp;&emsp;性能观测：用 `--face-interval` 控制抽样频率，比较 `8`、`12`、`16` 等值下的 CPU 和处理帧率。人脸检测必须与 LCD/JPEG 主路径隔离，否则单帧检测耗时会直接扩大视频延迟。

## 2026-10-24 10:04：完善触摸拍照和坐标兼容

&emsp;&emsp;触摸线程读取 Linux evdev 的释放事件，优先使用多点触摸位置轴，按 `EVIOCGABS` 的范围归一化到 framebuffer 可见尺寸，并只在释放位置命中 LCD 圆形按钮时保存最新 JPEG。触摸拍照和 HTTP `/snapshot` 共用保存、命名、目录创建、配额和写盘锁。

&emsp;&emsp;方案取舍：采用事件设备扫描和能力检测，替代硬编码某一个 `eventN` 的做法。默认仍尝试 `/dev/input/event1`，但自动扫描可以适应板卡设备编号变化；显式 `--touch PATH` 时保留严格失败语义，便于部署验收。

&emsp;&emsp;风险与边界：部分 BSP 声明的 `ABS_X/ABS_Y` 范围与实际上报值不一致，超出范围时按屏幕最大坐标校正；坐标旋转和镜像仍由 BSP 负责。触摸线程只修改自己的输入状态，快照保存由 `HttpServer` 的互斥锁保护，因此并发触摸和 HTTP 截图不会同时通过配额检查写文件。

&emsp;&emsp;调试定位：遇到按钮点击无响应时，先看 `[Touch] Opened` 是否确认了按键和绝对轴，再看释放坐标是否落在按钮矩形内。发现坐标总落到右下角后，对比 `evtest` 原始值与 `EVIOCGABS` 范围，确认需要优先使用 `ABS_MT_POSITION_X/Y`，并增加越界校正日志。按钮只绘制到 LCD，不写入 JPEG，因此网页画面不应出现快门图标。

&emsp;&emsp;性能观测：触摸等待使用 250 ms 的 `poll` 超时，不创建忙等循环；拍照写盘耗时不阻塞采集线程，但会占用快照互斥锁。可用 `ls -lh`、`df -h` 和日志中的保存路径确认文件是否生成以及存储是否接近配额。

## 2026-10-24 16:10：增加采集帧率、处理帧率和诊断日志

&emsp;&emsp;在共享状态中增加原子采集帧计数，在处理线程中按一秒窗口计算 `CAP`、`FPS`、LCD 平均耗时和 JPEG 平均耗时；LCD 诊断面板同步显示这些数据。`CAM_DEBUG` 根据前台终端和环境变量控制高频诊断日志，启动、错误和照片结果日志保持可见。

&emsp;&emsp;方案取舍：将 `CAP` 与 `FPS` 分开统计，替代只打印处理帧率。两者的差值可以判断瓶颈是在摄像头/驱动交付，还是在应用处理和队列积压；统计只读原子计数，不给采集线程增加互斥锁。日志默认在后台隐藏，替代无条件刷屏，避免日志 I/O 进一步影响 i.MX6ULL 的实时处理。

&emsp;&emsp;风险与边界：一秒窗口的首个样本受初始化时序影响，不能单独作为结论；`statsFrames` 非零时才计算平均耗时。原子计数只表示成功返回到应用的帧，不能证明摄像头传感器实际输出了声明的帧率。后台强制诊断可用 `CAM_DEBUG=1`，但高频日志仍会增加 CPU 和存储压力。

&emsp;&emsp;调试定位：板端格式枚举和 `--set-parm=30` 都显示 30 FPS，但完整采集和 `v4l2-ctl` 实测约 14 FPS；这排除了仅由应用 LCD/JPEG 造成的解释。进一步观察到运行程序时 CPU 接近满载，`cam_server_arm` 约占 77% CPU；交互式 `top` 自身也会增加负载，因此性能测试需退出持续刷新的 `top` 后再测量。

&emsp;&emsp;性能观测与优化：当前瓶颈可能同时来自摄像头/USB/驱动实际吞吐、软件 JPEG、YUYV 转换、逐像素 LCD 写入和人脸线程。按 `--no-lcd`、`--no-touch`、`--face-interval 120`、降低 JPEG 质量和降低采集分辨率逐项隔离；若 `CAP` 本身低，优先查 USB 链路、驱动和供电；若 `CAP` 高而 `FPS` 低，优先减少编码、显示或检测开销。后续可评估摄像头 MJPEG 直通 HTTP/截图和独立解码显示，但 LCD 仍需要解码，不能直接保证同样帧率。

## 2026-10-25：增加 MJPEG 摄像头输入以降低处理负载

&emsp;&emsp;最新板端观测中 `CAP` 约 25 FPS、处理 `FPS` 约 6 FPS，`cam_server_arm` 接近 99% CPU，user 与 softirq 合计约 96%。这表明摄像头已能提供较高帧率，主要矛盾转为单线程处理成本；旧链路每帧执行 YUYV 转 BGR、LCD 写入和软件 JPEG 编码。

&emsp;&emsp;方案取舍：新增 `--format mjpeg`，把相机压缩帧直接送入 HTTP/MJPEG 和截图，去掉软件 JPEG 编码；同时在 MJPEG 模式按需解码，`--lcd-interval` 控制 LCD 更新频率，`--face-interval` 控制人脸分析解码频率。相比提高线程数，这条路径直接移除每帧重编码工作，更适合 CPU 紧张的 i.MX6ULL；默认仍是 YUYV 以保持兼容。替代方案是硬件 JPEG 编码或零拷贝 DMA，需要板端驱动/编码器支持，暂不引入额外 BSP 依赖。

&emsp;&emsp;风险与边界：摄像头须支持指定 MJPEG 分辨率；MJPEG 模式中的 `--quality` 不生效，图像质量由摄像头控制。LCD 和人脸分析仍要把 JPEG 解码为 BGR，因此屏幕输出可能继续受解码、缩放和显存写入限制。坏 JPEG 会导致处理流水线报告错误；有限队列仍会丢弃旧帧以限制延迟和内存。

&emsp;&emsp;调试与验证：用 `v4l2-ctl --list-formats-ext` 确认 MJPEG 尺寸支持，再用程序启动日志确认协商结果为 `MJPEG`。对比同一设备、尺寸、LCD 和人脸参数下的 `CAP/FPS` 与 `top`；若 `FPS` 明显上升，说明软件 JPEG 编码是重要成本，若仍约 6 FPS，则继续测量 LCD 渲染与 MJPEG 解码。主机侧回归测试覆盖 MJPEG 格式协商、压缩短帧接收和 V4L2 缓冲区释放；板端 FPS 仍需实机测量。

&emsp;&emsp;性能观测：`CAP=25`、`FPS=6` 表示处理速率约为采集的四分之一，有限队列会主动丢弃旧帧而非持续增长延迟。MJPEG 直通预期降低 user CPU；若 softirq 仍高，USB/摄像头中断成本不会因去掉软件编码而消失。

## 2026-10-25：板端验证 MJPEG 直通和 LCD 刷新间隔

&emsp;&emsp;调试结果：`--format mjpeg --no-lcd --no-touch --face-interval 30` 下，稳定 `CAP/FPS` 约 14.3 FPS，`JPEG` 约 0.005~0.007ms；这证明输出路径已经直接复用摄像头 JPEG，软件 JPEG 编码不再是主要瓶颈。启用 `--fb /dev/fb0 --lcd-interval 2` 后，`CAP` 约 14~22 FPS，处理 `FPS` 约 13~22 FPS，LCD 单次刷新约 50~75ms；触摸启用后结果接近，触摸线程不是主要负载。

&emsp;&emsp;方案取舍：LCD 仍保留 BGR 解码和现有 framebuffer 写入，原因是 MJPEG 压缩数据不能直接写入 RGB565 显存。`--lcd-interval 2` 只降低显示刷新频率，网页仍发布每个相机 JPEG，兼顾实时网络流和板端显示成本。进一步提高间隔可以提升处理帧率，但 LCD 画面更新时间会变慢。

&emsp;&emsp;风险与边界：统计中的 `JPEG` 在 MJPEG 模式只包含 JPEG 数据移动时间，不代表摄像头压缩耗时；`LCD` 是实际显示帧的平均耗时，不能简单乘以输入帧率作为总 CPU 占用。LCD 每次刷新约 50~75ms 时，单线程显示路径仍可能限制可见画面帧率。人脸检测按 30 帧抽样，在 30 FPS 输入下理论反馈间隔约 1 秒，队列丢帧或检测线程繁忙时会更长。

&emsp;&emsp;后续优化：优先尝试 `--lcd-interval 3` 或 `4`，比较显示流畅度和 `FPS`；若仍需更高 LCD 帧率，应把显示缩放和像素打包改为行级批量写入，或使用 BSP 提供的硬件合成/双缓冲接口。继续增加采集队列数量不会解决当前 LCD 处理耗时，只会改变丢帧和延迟特征。

## 2026-10-25：LCD 每三帧刷新时接近 30 FPS

&emsp;&emsp;测试结果：在 640x480 MJPEG 模式下使用 `--lcd-interval 3 --face-interval 30`，稳定阶段 `CAP` 约 29~31 FPS、处理 `FPS` 约 26~29 FPS，`LCD` 单次刷新约 59~82ms，`JPEG` 数据复用约 0.002ms。打开触摸设备后的帧率与关闭触摸时接近。开头数秒曾出现约 10 FPS，之后提升并稳定在接近 30 FPS；因此旧的约 14 FPS 数据是一次条件相关测量，不能据此判断摄像头硬件固定上限。

&emsp;&emsp;性能原因：MJPEG 相机帧直接进入 HTTP 和快照，省去了每帧 YUYV 转换及软件 JPEG 编码。LCD 只在每三个处理帧中解码、缩放和写显存一次，显示开销约降为每帧刷新方案的三分之一；人脸分析每 30 帧抽样一次。余下两帧仍可直接作为 JPEG 发布，所以网页约 26~29 FPS，而 LCD 实际更新约 9~10 次/秒。

&emsp;&emsp;方案取舍：`--lcd-interval 3` 是网络流畅度与实体屏刷新率之间的折中。改为 `2` 时可见刷新更频繁，但此前处理帧率约 13~22 FPS；改为 `4` 有望进一步释放 CPU，但 LCD 动画会更不连贯。`JPEG≈0.002ms` 只是应用内复用压缩帧的开销，不是相机编码时间，也不能单独证明端到端延迟。

&emsp;&emsp;风险与后续定位：`CAP` 和 `FPS` 按独立的一秒窗口计数，启动时以及队列跨窗口时会短暂不一致；需以稳定多行和持续运行测量为准。若启动后长时间停在约 10 FPS，应同步检查摄像头自动曝光、V4L2 实际帧间隔、USB/软中断负载，并在不运行 `top` 的条件下重复测试。接下来可比较 LCD 间隔 3 和 4，记录屏幕可见流畅度、网页 FPS、CPU 和温度。

## 2026-10-25：完成 menuconfig 到 C++ 头文件的配置链路

&emsp;&emsp;后备 `whiptail` 界面随后改为分层菜单：主菜单进入摄像头、LCD/触摸或处理输出分类，分类菜单进入具体配置项，选择项使用上下键和回车确认，取消配置项不会覆盖旧值，返回上一级后可以继续修改，主菜单的 `Save and exit` 才会原子写入 `.config`。

&emsp;&emsp;界面配色使用 `NEWT_COLORS` 设置为蓝色背景、白色文字和青色选中项，模拟内核 `menuconfig` 的蓝色 ncurses 页面。配色只作用于后备 `whiptail` 界面；如果系统安装了 `mconf`，仍使用标准 Kconfig 界面。

&emsp;&emsp;随后发现系统安装的实际命令名是 `kconfig-mconf`，脚本只检测 `mconf` 导致始终误用 `whiptail`。修复后按 `MCONF`、`mconf`、`kconfig-mconf` 顺序探测，当前环境会进入真正的内核 Kconfig 界面；这解释了此前“全蓝但不像内核界面”的现象。

&emsp;&emsp;将启动参数对应关系补齐到 Kconfig：`CAM_TOUCH_DEVICE` 默认生成 `--touch /dev/input/event1`，LCD 间隔选择 `CAM_LCD_INTERVAL_3` 后生成 `CAM_CONFIG_LCD_INTERVAL 3`，人脸间隔选择 `CAM_FACE_INTERVAL_30` 后生成 `CAM_CONFIG_FACE_INTERVAL 30`。两个间隔使用 choice 固定选项，避免在内核式菜单中再次出现任意数字输入。

&emsp;&emsp;本次继续完成配置入口：`config/Kconfig` 描述配置模型，`make menuconfig` 在有 `mconf` 时使用 Kconfig 界面，否则使用 `whiptail` 菜单后备界面；配置结果写入 `.config`，`scripts/gen_config.sh` 再生成 `build/generated_config.hpp`。格式、分辨率和数值参数改为固定选项，分辨率使用 `320x240`、`640x480`、`800x600`、`1280x720`、`1280x1024` 预设。Makefile 通过 `generated_config.hpp: .config scripts/gen_config.sh` 和主程序目标依赖保证配置先转换、再编译；ARM Makefile 复用同一链路，只替换编译器和输出目录。

&emsp;&emsp;方案取舍：选择生成头文件而不是让程序在运行时解析 `.config`，因为设备路径、尺寸、开关和端口可以在编译期进入默认值，板端程序不需要携带配置解析器；替代方案是 `include .config` 让 Make 变量直接传给编译器，但 Kconfig 字符串引号、`y/n` 布尔值和 C++ 字符串转义需要额外处理，且会让 Makefile 变量命名与 C++ 宏耦合。当前生成脚本集中完成类型转换和尺寸校验，便于主机、ARM 两种构建复用。

&emsp;&emsp;风险与边界：`.config` 是文本格式，Make 不会自动理解其中的 `CONFIG_*` 语义；只有生成规则读取它。分辨率只能使用五个预设，手工写入其他分辨率时脚本会拒绝生成头文件；其他配置项通过菜单选项或命令行参数校验。`mconf` 与 `whiptail` 的交互结果都必须完整写回 `.config`，中途取消时临时文件会被清理，避免半份配置覆盖旧配置。配置修改后若只运行旧二进制不会生效，必须让 `make` 重新生成头文件并链接程序。

&emsp;&emsp;调试定位：首次验证简化菜单时发现输入框显示的旧值没有被读取，原因是调用方传入 `CAM_CAMERA_WIDTH`，读取函数却只匹配 `CONFIG_CAM_CAMERA_WIDTH`。修复后在读取函数内部统一补齐 `CONFIG_` 前缀，并用已有 `.config` 重新进入菜单确认值能够继承；随后用 `make -n` 检查依赖顺序，用 `sh scripts/gen_config.sh` 检查宏输出。这个问题说明菜单能显示并保存不等于配置可持续编辑，必须验证“旧配置读取”和“新头文件生成”两个方向。

&emsp;&emsp;性能与观测：配置生成只在 `.config` 或脚本更新时间变化时执行，开销是一次小型 shell 文本转换，不在摄像头运行时路径上。用 `make -n cam_server` 可以观察是否会先调用生成脚本；用 `stat .config build/generated_config.hpp` 可以核对时间戳；用 `sed -n` 查看生成宏；最终用 `strings build/cam_server` 或 `./build/cam_server --help` 检查编译进去的默认值。若修改配置后目标没有重编译，应先检查目标是否依赖 `$(CONFIG_HEADER)`，以及是否误用了旧的 `OBJ_DIR`。

## 复现与验收记录

&emsp;&emsp;主机侧使用 `make -B cam_server build/test_face_analyzer test_video test_v4l2` 编译，已验证人脸分析、视频显存保护和模拟 V4L2 故障路径；`./build/cam_server --help` 验证启动参数。ARM 版本使用 `make -B -f Makefile.arm package_arm` 生成程序、模型和 OpenCV 动态库目录。

&emsp;&emsp;板端验收应记录摄像头协商分辨率、`CAP/FPS`、LCD/JPEG 耗时、CPU、内存、温度和 `dmesg` 中的 USB/摄像头错误。还需验证触摸按钮坐标、重复拍照序号、HTTP `/snapshot` 与触摸拍照的配额一致性，以及 Ctrl-C/SIGTERM 后设备可以再次打开。
