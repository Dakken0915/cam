## 2026-10-22：建立摄像头到 LCD 和 HTTP 的基础流水线

&emsp;&emsp;完成 V4L2、framebuffer、JPEG 和 HTTP 的基础串接。采集线程从 V4L2 MMAP 缓冲区复制帧到 `Frame::data`，处理线程完成图像转换、LCD 显示和 JPEG 编码，主线程发布最新 JPEG。

&emsp;&emsp;方案取舍：使用一次用户态复制，把驱动缓冲区与后续线程的生命周期解耦。替代方案是直接处理 MMAP 区域，但驱动重新排队后数据所有权不稳定，关闭流程和线程同步更复杂。`raw`、`jpeg` 队列固定容量为 3，满载丢弃旧帧，优先控制实时延迟和内存占用。

&emsp;&emsp;风险与边界：YUYV 的有效长度必须按 `bytesperline` 检查，宽度必须为偶数；队列关闭必须唤醒等待线程，退出时先关闭队列并等待线程，再释放设备和显存。

&emsp;&emsp;调试定位：早期 LCD 黑边和偏移来自忽略 framebuffer 的 `xoffset`、`yoffset` 和 `line_length`。通过带行填充和窗口偏移的保护区测试定位，统一使用带 stride 的显存地址计算，并补充 `test_video`。

&emsp;&emsp;性能观测：使用 `top`、`v4l2-ctl` 和程序统计日志分别观察 CPU、摄像头能力和处理帧率。早期主要成本是 YUYV 转 BGR、逐像素 LCD 写入和软件 JPEG 编码。

## 2026-10-24：加入 Haar 人脸检测和灰度比对

&emsp;&emsp;采用独立 `FaceAnalysisWorker` 线程处理抽样 BGR 帧，检测图宽度限制为 240 像素，检测框映射回原图；参考图归一化为 64x64 灰度图，用均衡化和平均绝对差计算相似度。

&emsp;&emsp;方案取舍：选择 Haar cascade 是因为模型小、依赖少，适合 i.MX6ULL；深度检测和特征向量识别更鲁棒，但模型、内存和算力成本更高。灰度差值只用于演示匹配，不能替代身份认证。

&emsp;&emsp;风险与边界：低分辨率会增加小脸、侧脸和遮挡场景的漏检；分析邮箱只保留一帧，新任务覆盖旧任务，避免检测延迟积累。模型加载失败只禁用人脸分析，不阻塞视频流水线。

&emsp;&emsp;调试定位：模型路径错误时区分“模型加载失败”和“没有检测到人脸”，使用 `test_face_analyzer` 验证空图像、无效模型和相似度边界。

&emsp;&emsp;性能观测：用 `--face-interval` 对比抽样频率和 CPU。人脸检测必须与 LCD/JPEG 主路径隔离，否则单帧检测耗时会直接增加视频延迟。

## 2026-10-24：完善触摸拍照和坐标兼容

&emsp;&emsp;触摸线程读取 evdev 释放事件，优先使用多点触摸坐标轴，按 `EVIOCGABS` 范围映射到 framebuffer；命中 LCD 圆形按钮后调用 `HttpServer::captureSnapshot()`。触摸拍照和 HTTP 截图共用最新 JPEG、目录、命名、配额和写盘锁。

&emsp;&emsp;方案取舍：采用事件能力扫描和坐标范围读取，替代硬编码某个 `eventN` 或固定坐标范围，以适应不同 BSP 的设备编号和触摸轴范围。显式 `--touch PATH` 保留严格失败语义，便于部署验收。

&emsp;&emsp;风险与边界：部分 BSP 的声明范围与实际上报值不一致，越界值需要校正；旋转和镜像仍由 BSP 负责。触摸线程只维护输入状态，快照写盘由互斥锁保护。

&emsp;&emsp;调试定位：按钮无响应时先确认设备能力和释放事件；发现坐标总落在右下角后，对比 `evtest` 原始值与 `EVIOCGABS` 范围，改为优先使用 `ABS_MT_POSITION_X/Y` 并增加越界校正日志。

&emsp;&emsp;性能观测：触摸使用 250 ms `poll` 等待，不忙等；写盘耗时不阻塞采集线程，但会占用快照锁。

## 2026-10-24：增加采集帧率、处理帧率和诊断统计

&emsp;&emsp;在共享状态中增加原子采集计数，在处理线程按一秒窗口计算 `CAP`、`FPS`、LCD 平均耗时和 JPEG 平均耗时。`CAP` 表示应用从 V4L2 成功取帧的速率，`FPS` 表示进入 HTTP 队列的处理速率。

&emsp;&emsp;方案取舍：分开统计 `CAP` 和 `FPS`，替代只打印处理帧率，便于区分摄像头/驱动瓶颈和应用处理瓶颈。高频日志默认受 `CAM_DEBUG` 控制，避免日志 I/O 影响实时处理。

&emsp;&emsp;风险与边界：首个统计窗口受启动时序影响；原子计数只代表应用成功收到的帧，不代表传感器实际输出能力。队列丢帧时 `CAP` 可以高于 `FPS`。

&emsp;&emsp;调试定位：板端曾出现格式声明 30 FPS、完整采集约 14 FPS 的情况；通过对比 `CAP/FPS`、`top` 和 `v4l2-ctl`，确认不能只根据格式列表判断实际吞吐。

&emsp;&emsp;性能观测：用 `--no-lcd`、降低人脸分析频率、降低分辨率和 JPEG 质量逐项隔离处理成本；测试时退出持续刷新的 `top`，避免把监控工具负载计入结论。

## 2026-10-25：加入 MJPEG 直通和 LCD 刷新间隔

&emsp;&emsp;新增 MJPEG 摄像头输入。MJPEG 帧直接移动到 HTTP 输出，只有 LCD 或人脸分析需要时才调用 `cv::imdecode()`；`--lcd-interval` 控制 LCD 解码和刷新频率，`--face-interval` 控制分析抽样。

&emsp;&emsp;方案取舍：摄像头 JPEG 直通可以去掉每帧软件 JPEG 编码，适合 i.MX6ULL。替代方案是硬件编码或 DMA 零拷贝，但依赖额外 BSP 能力。LCD 仍必须解码成 BGR 并写入 framebuffer。

&emsp;&emsp;风险与边界：摄像头必须支持指定的 MJPEG 尺寸和帧率；MJPEG 模式下 `--quality` 不影响摄像头内部编码。坏 JPEG 会终止处理流水线，有限队列仍会丢弃旧帧控制延迟。

&emsp;&emsp;调试定位：板端实测 MJPEG 直通后 `JPEG` 约 0.002~0.007ms，说明软件 JPEG 编码已移除；LCD 间隔从 2 调整到 3 后，处理帧率由约 13~22 FPS 提升到接近 30 FPS，瓶颈集中在 MJPEG 解码、LCD 缩放和逐像素显存写入。

&emsp;&emsp;性能观测：`--lcd-interval 3` 是网络输出和 LCD 刷新成本的折中，网页仍发布每个处理到的 JPEG，LCD 实际刷新约为处理帧率的三分之一。

## 2026-10-25：完成 menuconfig 到 C++ 头文件的配置链路

&emsp;&emsp;`config/Kconfig` 描述配置模型，`make menuconfig` 使用 `kconfig-mconf` 或 `whiptail`，结果写入 `.config`；`scripts/gen_config.sh` 将配置转换为 `build/generated_config.hpp`，主机和 ARM 构建共用该链路。

&emsp;&emsp;方案取舍：选择生成头文件而不是让程序运行时解析 `.config`，因为设备路径、分辨率、开关和端口可在编译期进入默认值，目标程序不需要携带配置解析器。替代方案是 Make 直接 `include .config`，但需要额外处理 Kconfig 字符串、布尔值和 C++ 转义。

&emsp;&emsp;风险与边界：`.config` 是文本文件，只有生成脚本理解其语义；配置修改后必须重新生成头文件并重新链接程序。分辨率、格式和间隔使用固定选项，手工写入不支持的值会被拒绝或回退。

&emsp;&emsp;调试定位：后备菜单第一次读取旧值失败，是调用方使用 `CAM_*` 名称而读取函数只匹配 `CONFIG_CAM_*`；统一补齐前缀后，用已有 `.config` 验证旧值继承，并用 `make -n` 检查依赖顺序。

## 2026-10-25：梳理数据流和共享边界

&emsp;&emsp;数据经过 `V4l2Capture::getFrame()`、`Frame::data`、`raw` 队列、`JpegEncoder::bgr_` 或 MJPEG `decoded`、`encoded.data`、`jpeg` 队列，最后进入 `HttpServer::latestFrame_`。HTTP `/stream`、`/snapshot` 和触摸拍照共享最新 JPEG；LCD 使用 BGR 图像写入显存，不与 HTTP 共用同一块像素内存。

&emsp;&emsp;方案取舍：YUYV 只做一次 YUYV 到 BGR 转换，LCD 和 JPEG 复用结果；MJPEG 直接复用压缩数据，按需解码显示。这样减少重复转换和编码，代价是 LCD 与网页更新时间可能不同。

&emsp;&emsp;风险与边界：`latestFrame_` 由 `frameMutex_` 保护，快照配额和写盘由 `snapshotMutex_` 保护；LCD 按钮和统计文字只写显存，不会进入 JPEG。队列丢帧和 LCD 刷新间隔会造成两个输出的帧序号不同步。

## 2026-10-25：修复帧率配置回归

&emsp;&emsp;问题现象：修改前约 30 FPS，增加帧率配置后稳定约 10 FPS。`v4l2-ctl` 仍显示设备声明 30 FPS，程序关闭 LCD、触摸、人脸后 `CAP` 与 `FPS` 仍约 10，说明问题不在图像处理。

&emsp;&emsp;调试定位：新增的 `VIDIOC_S_PARM` 是首个回归点。旧代码不设置 stream 参数，驱动保留默认 30；直接使用零初始化的 `v4l2_streamparm` 可能覆盖驱动需要保留的参数。现改为先 `VIDIOC_G_PARM`，当前值已等于请求值时跳过 `S_PARM`，只有切换 10/20/30 时才修改 `timeperframe`。

&emsp;&emsp;风险与边界：`driver selected: 30 FPS` 只是 ioctl 返回的名义值，实际帧率必须看完整采集窗口的 `CAP`。驱动可能接受参数但因曝光、USB 或传感器状态实际只交付 10 FPS。

&emsp;&emsp;验证方法：`test_v4l2` 覆盖默认 30 FPS 和 MJPEG 20 FPS 的请求；ARM 包重新生成后，启动日志需要同时记录 requested 和 driver selected，板端还需用独立 `v4l2-ctl` 连续采集验证。

## 2026-10-25：排查 USB OTG 和 UVC 实际帧率

&emsp;&emsp;板卡曾将 USB OTG 从 Host 切换为 Gadget Device，用于手机和 ADB。这可能改变 USB 控制器角色、供电和中断负载；若摄像头与 Gadget 共用同一物理口，两个功能无法同时作为独立链路工作。

&emsp;&emsp;板端实测 MJPEG 和 YUYV 都稳定约 10 FPS，即使关闭 LCD、触摸、人脸和软件 JPEG，因此应用处理路径不是瓶颈。`lsusb -t` 显示摄像头以 480M High-Speed 挂在 `ci_hdrc.1` 的 Hub 下，可以排除 Full-Speed 12M 带宽不足；`dmesg` 中曾出现摄像头断开、UVC 注销和重新枚举，说明 USB Hub、供电或 OTG 角色切换造成过设备复位。

&emsp;&emsp;下一步方案：恢复 OTG Host、停止 Gadget/ADB 或改用独立 Host 口，在相同分辨率和格式下重复连续采集；同时检查 `exposure_auto_priority`，低照度自动曝光也可能使实际帧率降到 10。只有在 Host/Gadget 对照后恢复到 30 FPS，才能确认 OTG 资源冲突；否则继续定位 UVC 曝光控制、供电和 USB 复位。

&emsp;&emsp;性能观测：使用 `v4l2-ctl --list-formats-ext` 查看能力，`--get-parm` 查看名义参数，使用 `--stream-mmap --stream-count` 测量实际帧率，并结合 `dmesg`、`top` 的 softirq 和程序 `CAP` 统计判断真实瓶颈。

## 2026-10-26：实现 LCD 多进程与共享内存解耦

&emsp;&emsp;当前 `encodeLoop()` 原先在同一个线程中完成 MJPEG 解码、LCD 缩放与逐像素写入、JPEG 发布；即使已经把人脸检测放到独立线程，i.MX6ULL 的单核 CPU 仍会被这些工作共同占满。多线程只能改善等待和流水线重叠，不能增加单核总算力。本次新增 `LcdProcess`，由父进程创建三槽匿名共享内存并在启动应用线程前 `fork` LCD 子进程；父进程继续处理采集、HTTP 和人脸分析，子进程独占 framebuffer 完成 LCD 刷写。

&emsp;&emsp;方案取舍：本次选择共享内存传递连续 BGR 帧，槽位按 `FREE/WRITING/READY/READING` 状态变化，满载时丢弃最旧 READY 帧。相比管道或 Unix socket，它避免了额外序列化并能固定内存上限；相比父进程预先生成 RGB565，它暂时不把缩放和像素转换移回父进程，改造范围更小，但 LCD 子进程仍承担这些 CPU 工作。后续只有在确认共享内存复制和父进程 CPU 余量足够时，才考虑改成 RGB565 行缓冲。

&emsp;&emsp;风险与线程/进程安全：`fork` 在应用线程启动前执行，避免子进程继承其他线程持有的 C++ 或 OpenCV 锁。共享控制块使用 `PTHREAD_PROCESS_SHARED` mutex/condition variable；普通 `std::mutex` 不用于跨进程同步。父进程保留 framebuffer 对象只读取屏幕尺寸和触摸按钮几何，LCD 子进程负责实际写入。父进程通过 `waitpid(WNOHANG)` 发现子进程退出，停止发布并在退出时先唤醒、等待子进程，再销毁同步对象和解除映射。当前仍需在板端验证摄像头 fd 在 LCD 子进程退出前的生命周期，以及 LCD 驱动异常时的退出时延。

&emsp;&emsp;调试定位：实现阶段首先遇到共享控制块前置声明与源文件匿名类型冲突，编译器将 `LcdSharedState` 识别为两个不同类型；将控制块定义移到源文件全局作用域后恢复一致。随后发现 `fork` 必须早于 HTTP、采集和人脸线程，最终把 LCD 子进程启动放在摄像头格式协商之后、`capture.start()` 之前。主机侧没有真实 framebuffer，无法模拟子进程刷屏；因此目前用主程序编译和现有视频/V4L2/人脸测试验证接口，板端还需确认共享槽位序号、旧帧丢弃和子进程退出。

&emsp;&emsp;性能瓶颈与优化：LCD 的主要成本仍是缩放、BGR 到 framebuffer 像素打包和显存写入；MJPEG 直通已经移除了软件 JPEG 编码。板端实测在摄像头捕获约 30 FPS 时，采用多进程和共享内存后 LCD 刷新达到约 17~21 FPS，父子两个进程的 CPU 占用合计约 50%。这说明 LCD 刷新不再串行阻塞图像处理主流程，同时本次测量仍留有 CPU 余量；结果只适用于当前板卡、图像格式、分辨率和 LCD 配置。后续可按行批量写入 RGB565、减少显示缩放成本或调用 BSP 硬件合成，并继续记录各进程 CPU、`CAP/FPS/LCD`、共享槽位丢帧和端到端延迟。

## 2026-10-26：确认 v4l2-ctl 当前模式实际约 10 FPS

&emsp;&emsp;板端执行 `v4l2-ctl -d /dev/video1 --stream-mmap --stream-count=100`，测得 11.86 FPS 逐步稳定到 10.23 FPS。这个命令没有包含 `--set-fmt-video` 或 `--set-parm=30`，因此它测量的是设备当前已经配置的格式、分辨率和帧间隔，不能单独证明设备不支持 30 FPS。工程 `.config`、`config/cam_defconfig` 和生成头文件默认请求值均为 30 FPS。

&emsp;&emsp;方案取舍：先用 v4l2-ctl 独立于应用验证“显式请求后的实际采集”，再判断程序的 `VIDIOC_S_PARM` 是否生效；替代方案是直接根据 `v4l2-ctl --list-formats-ext` 的能力列表下结论，但能力列表只表示某个特定格式/分辨率组合可能支持的离散帧间隔，不代表当前模式已经使用该帧率。

&emsp;&emsp;风险与边界：如果 `--get-parm` 显示 30 FPS 但连续采集仍约 10 FPS，原因应转向自动曝光、USB 链路/带宽、UVC 驱动或供电复位；如果显式 `--set-parm=30` 后驱动回报 10 FPS，则是该格式和分辨率组合未提供 30 FPS。还要分别测试 MJPEG 与 YUYV，因为 640x480 YUYV 30 FPS 的带宽和摄像头模式约束明显高于 MJPEG。

&emsp;&emsp;调试定位：下一步固定执行 `--set-fmt-video=width=640,height=480,pixelformat=MJPG`、`--set-parm=30`、`--get-fmt-video --get-parm`，再连续采集 300 帧；随后对 YUYV 重复同样流程。若两种格式都实际 10 FPS，再检查 `exposure_auto_priority`、`lsusb -t` 和 `dmesg`，避免把应用 CPU 或 LCD 进程误判为摄像头帧率上限。

&emsp;&emsp;性能观测：独立 v4l2-ctl 测试时停止 `cam_server_arm`，记录协商格式、驱动回报帧间隔、实际采集耗时和 `dmesg`。应用运行时再对照启动日志中的 `Requested frame rate`、`driver selected` 和 `[Stats] CAP`；只有三者在相同格式、分辨率、曝光和 USB 条件下对照，才能确定 10 FPS 的来源。

## 2026-10-26：确认帧率随照度从 10 FPS 变化到 30 FPS

&emsp;&emsp;板端对照结果：普通光照下实际约 10 FPS，手电筒强光照射后达到约 30 FPS。该对照使摄像头低照度自动曝光成为最符合现有证据的原因：低照度时摄像头延长曝光以保持画面亮度，实际帧间隔随之增加；强光下曝光缩短后恢复到请求的 30 FPS。应用当前仅通过 V4L2 请求帧率，没有设置曝光或 `exposure_auto_priority`。

&emsp;&emsp;方案取舍：先用同一格式、分辨率和 USB 连接，在应用关闭时比较 `v4l2-ctl --stream-mmap` 的实际帧率，隔离摄像头输出与应用处理；检查设备是否提供 `exposure_auto_priority` 控件。若支持，可临时设为 0 对照低照度帧率，观察是否稳定在 30 FPS。关闭自动曝光优先可能导致暗处画面变暗，因此是否在程序中加入可配置曝光策略，应依据目标画质决定。

&emsp;&emsp;风险与边界：强光/普通光对照尚未同时记录 `[Stats] CAP`、曝光值和独立 v4l2-ctl 数据，所以自动曝光目前是高可信解释而非完全排除 UVC 驱动行为的证明。若应用 `CAP` 随光照变化，变化发生在摄像头/驱动采集侧；若 `CAP` 不变而处理 `FPS` 变化，才应回到应用 CPU、LCD 和处理队列分析。`driver selected: 30 FPS` 仍只是驱动参数回报，不代表暗处实际帧率。

&emsp;&emsp;调试定位：之前只看到稳定约 10 FPS，曾怀疑新增的 `VIDIOC_S_PARM` 或 USB/OTG；手电筒强光下恢复 30 FPS 后，应用固定处理负载和 USB 连接不变而实际帧率随照度改变，使曝光策略成为首要检查点。下一步应记录 `v4l2-ctl --list-ctrls`、`--get-ctrl=exposure_auto_priority` 和实际采集帧率，并在支持时对比 `--set-ctrl=exposure_auto_priority=0`。

&emsp;&emsp;性能观测：低照度帧率受限时，降低应用分辨率或关闭 LCD 不会缩短摄像头曝光；应结合 V4L2 控件、`CAP`、完整采集窗口和图像亮度观测。若关闭曝光优先后帧率上升但图像欠曝，需在帧率与低照度画质之间取舍，或补光/调整固定曝光，而不是继续优化 JPEG/LCD 路径。

## 2026-10-26：加入自动曝光帧率优先级配置

&emsp;&emsp;为适配普通光约 10 FPS、手电筒强光约 30 FPS 的板端现象，在 `Camera input` 配置中加入 `CAM_EXPOSURE_AUTO_PRIORITY`，并通过生成头文件传入 V4L2 采集初始化。选项关闭时向摄像头请求保持帧率；开启时允许自动曝光在低照度下动态降低帧率。此设置控制曝光算法能否改变帧率，不会直接指定曝光时间或模拟增益。

&emsp;&emsp;方案取舍：使用内核标准 `V4L2_CID_EXPOSURE_AUTO_PRIORITY` 而不是硬编码曝光绝对值，因为不同摄像头的传感器、镜头和增益范围不同，自动曝光仍可随环境亮度调节。默认关闭以优先兑现用户请求的 30 FPS；需要暗处画面亮度时可在菜单开启，代价是帧率可降。若设备驱动不支持该控件，应用继续工作并输出警告。

&emsp;&emsp;风险与边界：V4L2 控件支持情况取决于摄像头和驱动；开关关闭也不能保证所有设备一定能达到请求帧率。低照度下固定帧率可能导致曝光不足或噪声上升。之后如需手动曝光时间和增益，需要分别查询控件范围、维护自动/手动模式切换，并保证曝光时间小于目标帧周期。

&emsp;&emsp;调试定位：新增模拟 V4L2 测试，验证默认关闭时写入 0、开启时写入 1，以及驱动拒绝 `VIDIOC_S_CTRL` 时采集仍能打开。板端需用 `v4l2-ctl --list-ctrls` 确认控件存在，并在相同照度下比较应用 `CAP` 和画面亮度；主机模拟测试不能代替真实摄像头验证。

&emsp;&emsp;性能观测：菜单选项只在摄像头初始化时设置一次，不增加逐帧处理开销。对照测试应停止应用后确认当前控制值，再启动应用并记录启动日志、`CAP/FPS`、CPU 和画面亮度；若开关状态无法读取或设置失败，以驱动日志和帧间隔为准。

## 2026-10-26：板端验证多进程 LCD 刷新性能

&emsp;&emsp;测试结果：摄像头捕获保持约 30 FPS 时，多进程加共享内存的 LCD 刷新率达到约 17~21 FPS，父子两个进程 CPU 占用合计约 50%。本次数据表明 LCD 子进程能在不阻塞主处理流程的情况下持续刷新，仍有部分输入帧未显示。

&emsp;&emsp;方案取舍：保留共享内存三槽和只消费最新帧的策略，使 LCD 负载跟不上时丢弃过期显示帧，而不是积压帧队列并增加画面延迟。该方案提升了 LCD 刷新率并隔离刷新阻塞，但不会让 LCD 追平摄像头的 30 FPS，也不会消除缩放和 framebuffer 写入成本。

&emsp;&emsp;风险与边界：17~21 FPS 和约 50% CPU 是当前板卡、输入格式/尺寸、LCD 模式及其他运行负载下的实测值，不代表所有配置均有相同表现。CPU 百分比需区分单个进程与父子进程合计，并避免将 `top` 自身开销计入结果。共享槽位丢帧属于实时显示取舍，HTTP 的处理帧率与 LCD 刷新率也不是同一指标。

&emsp;&emsp;调试定位：此前担心单核 i.MX6ULL 上增加进程不能提高处理能力；本次运行数据确认它的作用是隔离 LCD 刷新与主处理流程的调度，LCD 更新达到 17~21 FPS，而捕获仍约 30 FPS。当前没有发现因共享内存导致的采集帧率下降；后续需结合共享槽位丢帧统计、`CAP/FPS/LCD` 和端到端延迟持续观察。

&emsp;&emsp;性能观测与优化：本次父子进程 CPU 合计约 50%，后续应分别记录两进程 CPU 和系统总 CPU，并固定摄像头格式、分辨率、LCD 间隔及人脸检测频率做对照。可进一步按行批量写入 RGB565、降低显示缩放成本或使用 BSP 硬件合成；优化后同时比较 LCD 刷新率、丢弃帧数、画面延迟和 CPU，确认没有把负载转移到主进程。
