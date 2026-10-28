## 优化目标

&emsp;&emsp;当前观测中摄像头采集约 30 FPS，LCD 单次绘制约 90~110 ms，实际可见刷新约 8~11 FPS。驱动优化目标是减少 framebuffer 写入和等待造成的阻塞，降低撕裂和无效轮询，同时保持 `/dev/fb0` 的 fbdev 接口兼容性。

## 已落地的优化路径

&emsp;&emsp;第一步是双页 DMA 显存和页翻转。应用把一帧画到后台页，驱动把后台页物理地址写入 LCDIF `NEXT_BUF`，在帧完成后让硬件接管切换。该路径把显示扫描和 CPU 写入解耦，收益主要是减少撕裂和等待冲突；它不会自动降低 OpenCV 缩放或逐像素打包的 CPU 时间。

&emsp;&emsp;第二步是 `dma_alloc_wc` 与 `dma_mmap_wc`。写合并映射适合连续的行缓冲复制，CPU 可以把多次小写合并成更少的总线事务。应用仍应按行或按块生成 RGB565 后批量写入，不能恢复成每个像素一次 `memcpy`。

&emsp;&emsp;第三步是 LCDIF 中断和 completion。`FBIO_WAITFORVSYNC` 只在有新帧时等待一次硬件事件，`FBIOPAN_DISPLAY` 在提交地址后等待帧完成；没有 IRQ 时明确降级到异步路径，避免因错误的设备树中断号阻塞整个显示进程。

## 后续优化顺序

&emsp;&emsp;先在板端对比单页和双页的 `LCD`、`LCD_FPS`、CPU、撕裂和端到端延迟。若 `LCD` 时间几乎不变而撕裂减少，瓶颈在应用渲染或显存带宽；若 `LCD` 时间下降，应继续确认是否来自写合并和减少页等待，而不是统计窗口偶然波动。

&emsp;&emsp;再测量缩放、像素打包和 framebuffer 拷贝三个阶段。保持摄像头分辨率、曝光、`--lcd-interval` 和人脸分析参数一致，使用连续多行统计；不要只用一行 `CAP/FPS` 判断优化效果。用 `top` 或 `/proc/<pid>/stat` 观察 CPU，用 `dmesg` 检查 LCDIF 总线错误，用 `fbset` 检查两页布局。

&emsp;&emsp;如果显存拷贝仍是主要耗时，优先确认 BSP 是否提供硬件缩放、IPU/VPU 合成或 DMA-BUF 导入。只有确认接口、缓存同步和缓冲区所有权后，才增加 dmaengine 或私有 ioctl；否则通用 DMA 方案可能比当前连续行复制更慢，并带来缓存脏数据和页面正在扫描时被覆盖的风险。

## 方案取舍

&emsp;&emsp;选择 fbdev 原生双缓冲和 LCDIF `NEXT_BUF`，是因为现有应用已经通过 `FBIOPAN_DISPLAY` 工作，改动集中在驱动且不增加新的用户态设备协议。替代方案是直接使用 DRM/KMS atomic commit，理论上同步模型更完整，但当前 BSP 使用旧版 Linux 4.9、应用接口已经是 fbdev，迁移会扩大验证范围。

&emsp;&emsp;选择 LCDIF 内置扫描 DMA，而不是在驱动中另起一条 memcpy DMA 通道，是因为 LCD 控制器本来就通过 `CUR_BUF`/`NEXT_BUF` 作为总线主设备读取显存。额外 DMA 只负责把一块内存复制到另一块内存，仍然要等待复制完成、处理 cache sync，并不能替代 LCDIF 的扫描时序。

## 风险和回退

&emsp;&emsp;双缓冲增加内存压力，低内存时 probe 会失败并在日志中返回错误；`dma_alloc_wc` 失败时不能继续使用部分初始化的 framebuffer。若设备树没有 IRQ，驱动会报告异步回退，应用仍可显示但不能宣称已经完成 VSYNC 同步。

&emsp;&emsp;页面切换要求 `yoffset` 按整页对齐，非法偏移返回 `-EINVAL`；帧完成等待超过 500 ms 返回 `-ETIMEDOUT`，应用应记录并进入单缓冲或重试策略。模块卸载前必须停止用户态 mmap 访问，否则用户进程继续访问已释放的 DMA 显存会导致崩溃。

## 预期收益

&emsp;&emsp;在相同图像处理负载下，预期收益顺序是减少撕裂、减少显示等待、降低 framebuffer 小写事务和降低显示进程空转 CPU。能否把可见刷新率从当前约 8~11 FPS 提高到更高水平，取决于行级批量写入、缩放耗时和 LCD 总线带宽，必须以上板统计为准，不能仅凭驱动加载成功推断。
