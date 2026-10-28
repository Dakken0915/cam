## 改动范围

&emsp;&emsp;本次修改针对 `drv/lcd_drv.c` 的 i.MX6ULL LCDIF framebuffer 驱动。BSP 使用 Linux 4.9.88、`CONFIG_FB_MXS`/`CONFIG_FB_MXC` 和 LCDIF 控制器；板端当前设备树时序报告为 1024x600，应用仍以 `/dev/fb0` 实际返回的模式为准。

## 已完成的驱动改动

&emsp;&emsp;显存从单页改为两个连续页面。`yres_virtual` 设置为 `2 * yres`，`fix.smem_len` 同步扩大，显存通过 `dma_alloc_wc(&pdev->dev, ...)` 从 LCD 设备上下文分配，物理 DMA 地址同时写入 `CUR_BUF` 和 `NEXT_BUF`。写合并属性减少 CPU 向 framebuffer 的小写事务，两个页面为应用层页翻转提供了地址空间。

&emsp;&emsp;增加 `fb_mmap`，通过 `dma_mmap_wc` 映射完整的双页 DMA 缓冲区。用户态 `mmap` 得到的虚拟地址与 LCDIF 使用的 DMA 地址来自同一分配，避免了原驱动没有 mmap 接口时依赖通用 framebuffer 映射的隐含行为。

&emsp;&emsp;增加 `fb_pan_display`。应用提交第二页的 `yoffset` 后，驱动计算对应的物理地址并写入 `NEXT_BUF`，检查偏移必须是完整页面且不能越过 `yres_virtual`。有 LCDIF 中断时等待当前帧完成再返回；没有中断时保留异步翻页，保证不同设备树版本仍能显示。

&emsp;&emsp;增加 `FBIO_WAITFORVSYNC` 的 `fb_ioctl` 实现。驱动申请设备树提供的 LCDIF IRQ，分别处理帧完成和 VSYNC 状态，使用 completion 唤醒等待者。这样应用的 VSYNC 等待和页翻转能够使用硬件事件，而不是固定睡眠或忙轮询。

&emsp;&emsp;修复资源生命周期：检查设备树、时钟、GPIO、显存、I/O 资源和 framebuffer 注册返回值；使用设备管理接口管理 GPIO、时钟获取和寄存器映射；移除原来对未赋值 `mylcd_regs` 的错误 `iounmap`；移除驱动时释放 DMA 显存、关闭 LCDIF 和时钟。


## 改动带来的好处

&emsp;&emsp;双缓冲把“应用正在写入的页面”和“LCDIF 正在扫描的页面”分开，降低整帧显示被写到一半时产生撕裂的概率。VSYNC 和帧完成同步使页面切换有明确的硬件时机，应用统计的 LCD 完成帧也更接近真实的可见帧。

&emsp;&emsp;行级应用绘制可以直接写入后台页，完成后一次 `FBIOPAN_DISPLAY` 切换，避免在扫描页上反复修改。`dma_alloc_wc` 和 `dma_mmap_wc` 让 CPU 写入路径具备写合并属性，但不会把 LCDIF 的扫描 DMA 误认为应用可以直接提交的通用 DMA 传输。

&emsp;&emsp;错误检查和统一释放避免了 probe 失败、模块卸载或反复加载时的显存泄漏、时钟引用泄漏和无效寄存器访问。驱动没有 IRQ 时仍能显示，只是同步能力下降并在日志中明确提示。

## 使用与验证

&emsp;&emsp;使用 BSP 的交叉编译器构建模块：

```sh
cd /home/hxh1987370733/workspace/cam/drv
make
```

`drv/Makefile` 默认选择 BSP 自带的 Linaro 6.2.1；也可以显式执行内核模块命令：

```sh
make -C /home/hxh1987370733/toolchain/100ask_imx6ull-sdk/Linux-4.9.88 \
  M=$PWD/drv ARCH=arm \
  CROSS_COMPILE=/home/hxh1987370733/toolchain/100ask_imx6ull-sdk/ToolChain/gcc-linaro-6.2.1-2016.11-x86_64_arm-linux-gnueabihf/bin/arm-linux-gnueabihf- \
  modules
```

&emsp;&emsp;板端加载后检查 `fbset -fb /dev/fb0 -i`：`yres_virtual` 应为两倍有效高度，`Size` 应为两页显存大小；检查 `dmesg` 是否出现 `registered ... with IRQ`。应用启动日志应显示 `double-buffer=yes`，并且 `FBIO_WAITFORVSYNC` 与 `FBIOPAN_DISPLAY` 不再立即返回不支持错误。

&emsp;&emsp;卸载前应停止使用 `/dev/fb0` 的用户进程。先观察 `dmesg` 中是否有 LCDIF 总线错误，再卸载模块；如果设备树没有 LCDIF 中断，驱动会保留异步翻页，不能把这种模式下的结果当作无撕裂保证。

## 已知边界

&emsp;&emsp;本次改动使用的是 LCDIF 控制器自身的帧扫描 DMA。内核中虽然启用了 `CONFIG_IMX_SDMA` 和 `CONFIG_MXS_DMA`，但当前用户态没有可直接提交 framebuffer 拷贝的 dmaengine 文件描述符，也没有加入私有 DMA ioctl。要做应用层 DMA 拷贝，仍需确认 BSP 的 DMA-BUF、dmaengine 申请通道和缓存一致性接口。

&emsp;&emsp;双页显存会使显存占用翻倍；以 1024x600 RGB565 为例，显存从约 1.17 MiB 增加到约 2.34 MiB。板端实际显示尺寸由设备树时序决定，1280x640 时对应显存约 1.56 MiB 和 3.13 MiB。
