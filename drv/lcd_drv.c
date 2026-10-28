#include <linux/clk.h>
#include <linux/completion.h>
#include <linux/dma-mapping.h>
#include <linux/errno.h>
#include <linux/fb.h>
#include <linux/gpio/consumer.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <video/display_timing.h>
#include <video/of_display_timing.h>

#define LCDIF_CTRL1_CUR_FRAME_DONE_IRQ_EN (1U << 13)
#define LCDIF_CTRL1_VSYNC_EDGE_IRQ_EN     (1U << 12)
#define LCDIF_CTRL1_CUR_FRAME_DONE_IRQ    (1U << 9)
#define LCDIF_CTRL1_VSYNC_EDGE_IRQ        (1U << 8)

struct imx6ull_lcdif {
	volatile unsigned int CTRL;
	volatile unsigned int CTRL_SET;
	volatile unsigned int CTRL_CLR;
	volatile unsigned int CTRL_TOG;
	volatile unsigned int CTRL1;
	volatile unsigned int CTRL1_SET;
	volatile unsigned int CTRL1_CLR;
	volatile unsigned int CTRL1_TOG;
	volatile unsigned int CTRL2;
	volatile unsigned int CTRL2_SET;
	volatile unsigned int CTRL2_CLR;
	volatile unsigned int CTRL2_TOG;
	volatile unsigned int TRANSFER_COUNT;
	unsigned char RESERVED_0[12];
	volatile unsigned int CUR_BUF;
	unsigned char RESERVED_1[12];
	volatile unsigned int NEXT_BUF;
	unsigned char RESERVED_2[12];
	volatile unsigned int TIMING;
	unsigned char RESERVED_3[12];
	volatile unsigned int VDCTRL0;
	volatile unsigned int VDCTRL0_SET;
	volatile unsigned int VDCTRL0_CLR;
	volatile unsigned int VDCTRL0_TOG;
	volatile unsigned int VDCTRL1;
	unsigned char RESERVED_4[12];
	volatile unsigned int VDCTRL2;
	unsigned char RESERVED_5[12];
	volatile unsigned int VDCTRL3;
	unsigned char RESERVED_6[12];
	volatile unsigned int VDCTRL4;
	unsigned char RESERVED_7[12];
	volatile unsigned int DVICTRL0;
	unsigned char RESERVED_8[12];
	volatile unsigned int DVICTRL1;
	unsigned char RESERVED_9[12];
	volatile unsigned int DVICTRL2;
	unsigned char RESERVED_10[12];
	volatile unsigned int DVICTRL3;
	unsigned char RESERVED_11[12];
	volatile unsigned int DVICTRL4;
	unsigned char RESERVED_12[12];
	volatile unsigned int CSC_COEFF0;
	unsigned char RESERVED_13[12];
	volatile unsigned int CSC_COEFF1;
	unsigned char RESERVED_14[12];
	volatile unsigned int CSC_COEFF2;
	unsigned char RESERVED_15[12];
	volatile unsigned int CSC_COEFF3;
	unsigned char RESERVED_16[12];
	volatile unsigned int CSC_COEFF4;
	unsigned char RESERVED_17[12];
	volatile unsigned int CSC_OFFSET;
	unsigned char RESERVED_18[12];
	volatile unsigned int CSC_LIMIT;
	unsigned char RESERVED_19[12];
	volatile unsigned int DATA;
	unsigned char RESERVED_20[12];
	volatile unsigned int BM_ERROR_STAT;
	unsigned char RESERVED_21[12];
	volatile unsigned int CRC_STAT;
	unsigned char RESERVED_22[12];
	volatile unsigned int STAT;
	unsigned char RESERVED_23[76];
	volatile unsigned int THRES;
	unsigned char RESERVED_24[12];
	volatile unsigned int AS_CTRL;
	unsigned char RESERVED_25[12];
	volatile unsigned int AS_BUF;
	unsigned char RESERVED_26[12];
	volatile unsigned int AS_NEXT_BUF;
	unsigned char RESERVED_27[12];
	volatile unsigned int AS_CLRKEYLOW;
	unsigned char RESERVED_28[12];
	volatile unsigned int AS_CLRKEYHIGH;
	unsigned char RESERVED_29[12];
	volatile unsigned int SYNC_DELAY;
};

struct lcd_drvdata {
	struct device *dev;
	struct fb_info *info;
	struct imx6ull_lcdif *lcdif;
	struct clk *clk_pix;
	struct clk *clk_axi;
	dma_addr_t fb_dma;
	size_t fb_size;
	unsigned int page_size;
	int irq;
	bool irq_requested;
	unsigned int current_page;
	struct completion frame_done;
	struct completion vsync_done;
	spinlock_t lock;
};

static unsigned int pseudo_palette[16];

/**
 * @brief Enable the LCDIF controller after all registers are configured.
 * @param [in] lcdif Mapped LCDIF register block.
 * @retval None.
 */
static void lcd_controller_enable(struct imx6ull_lcdif *lcdif)
{
	writel(readl(&lcdif->CTRL) | 1U, &lcdif->CTRL);
}

/**
 * @brief Program LCDIF timing, pixel format, and initial framebuffer address.
 * @param [in] lcdif Mapped LCDIF register block.
 * @param [in] dt Display timing selected from the device tree.
 * @param [in] lcd_bpp Physical LCD bus width in bits per pixel.
 * @param [in] fb_bpp Framebuffer storage format in bits per pixel.
 * @param [in] fb_phy DMA address of the first framebuffer page.
 * @retval 0 Configuration succeeded.
 * @retval -EINVAL The requested pixel format is unsupported.
 */
static int lcd_controller_init(struct imx6ull_lcdif *lcdif,
				       struct display_timing *dt, int lcd_bpp,
				       int fb_bpp, dma_addr_t fb_phy)
{
	int lcd_width;
	int fb_width;
	int vsync_pol = 0;
	int hsync_pol = 0;
	int de_pol = 0;
	int clk_pol = 0;

	if (dt->flags & DISPLAY_FLAGS_HSYNC_HIGH)
		hsync_pol = 1;
	if (dt->flags & DISPLAY_FLAGS_VSYNC_HIGH)
		vsync_pol = 1;
	if (dt->flags & DISPLAY_FLAGS_DE_HIGH)
		de_pol = 1;
	if (dt->flags & DISPLAY_FLAGS_PIXDATA_POSEDGE)
		clk_pol = 1;

	switch (lcd_bpp) {
	case 24:
		lcd_width = 3;
		break;
	case 18:
		lcd_width = 2;
		break;
	case 8:
		lcd_width = 1;
		break;
	case 16:
		lcd_width = 0;
		break;
	default:
		return -EINVAL;
	}

	switch (fb_bpp) {
	case 24:
	case 32:
		fb_width = 3;
		break;
	case 18:
		fb_width = 2;
		break;
	case 8:
		fb_width = 1;
		break;
	case 16:
		fb_width = 0;
		break;
	default:
		return -EINVAL;
	}

	writel((1U << 19) | (1U << 17) | (lcd_width << 10) |
	       (fb_width << 8) | (1U << 5), &lcdif->CTRL);
	if (fb_bpp == 24 || fb_bpp == 32) {
		writel(readl(&lcdif->CTRL1) & ~(0xfU << 16), &lcdif->CTRL1);
		writel(readl(&lcdif->CTRL1) | (0x7U << 16), &lcdif->CTRL1);
	} else {
		writel(readl(&lcdif->CTRL1) | (0xfU << 16), &lcdif->CTRL1);
	}
	writel((dt->vactive.typ << 16) | dt->hactive.typ,
	       &lcdif->TRANSFER_COUNT);
	writel((1U << 28) | (vsync_pol << 27) | (hsync_pol << 26) |
	       (clk_pol << 25) | (de_pol << 24) | (1U << 21) |
	       (1U << 20) | dt->vsync_len.typ, &lcdif->VDCTRL0);
	writel(dt->vback_porch.typ + dt->vsync_len.typ + dt->vactive.typ +
	       dt->vfront_porch.typ, &lcdif->VDCTRL1);
	writel((dt->hsync_len.typ << 18) |
	       (dt->hback_porch.typ + dt->hsync_len.typ + dt->hactive.typ +
		dt->hfront_porch.typ),
	       &lcdif->VDCTRL2);
	writel(((dt->hback_porch.typ + dt->hsync_len.typ) << 16) |
	       (dt->vback_porch.typ + dt->vsync_len.typ), &lcdif->VDCTRL3);
	writel((1U << 18) | dt->hactive.typ, &lcdif->VDCTRL4);
	writel(fb_phy, &lcdif->CUR_BUF);
	writel(fb_phy, &lcdif->NEXT_BUF);

	return 0;
}

/**
 * @brief Acknowledge LCDIF frame and VSYNC interrupts and wake waiters.
 * @param [in] irq Linux IRQ number.
 * @param [in] data Driver state passed by request_irq.
 * @retval IRQ_HANDLED A supported LCDIF event was acknowledged.
 * @retval IRQ_NONE The interrupt did not belong to this LCDIF instance.
 */
static irqreturn_t lcdif_irq_handler(int irq, void *data)
{
	struct lcd_drvdata *drv = data;
	unsigned int status;

	(void)irq;
	status = readl(&drv->lcdif->CTRL1);
	status &= LCDIF_CTRL1_CUR_FRAME_DONE_IRQ |
		  LCDIF_CTRL1_VSYNC_EDGE_IRQ;
	if (!status)
		return IRQ_NONE;
	if (status & LCDIF_CTRL1_CUR_FRAME_DONE_IRQ) {
		writel(LCDIF_CTRL1_CUR_FRAME_DONE_IRQ,
		       &drv->lcdif->CTRL1_CLR);
		complete(&drv->frame_done);
	}
	if (status & LCDIF_CTRL1_VSYNC_EDGE_IRQ) {
		writel(LCDIF_CTRL1_VSYNC_EDGE_IRQ,
		       &drv->lcdif->CTRL1_CLR);
		complete(&drv->vsync_done);
	}
	return IRQ_HANDLED;
}

/**
 * @brief Map the DMA framebuffer into a userspace framebuffer client.
 * @param [in] info Framebuffer descriptor owning the DMA allocation.
 * @param [in,out] vma Userspace virtual-memory area to populate.
 * @retval 0 Mapping succeeded.
 * @retval Negative errno Mapping failed.
 */
static int mylcd_mmap(struct fb_info *info, struct vm_area_struct *vma)
{
	struct lcd_drvdata *drv = info->par;

	return dma_mmap_wc(drv->dev, vma, info->screen_base,
			   info->fix.smem_start, info->fix.smem_len);
}

/**
 * @brief Queue a framebuffer page for the next LCDIF frame and wait for completion.
 * @param [in] var Requested framebuffer offsets and mode.
 * @param [in,out] info Framebuffer descriptor receiving the new visible offset.
 * @retval 0 Page was queued and displayed, or no IRQ is available.
 * @retval -EINVAL Offset is not a complete valid virtual page.
 * @retval -ETIMEDOUT LCDIF did not report frame completion in time.
 */
static int mylcd_pan_display(struct fb_var_screeninfo *var,
				     struct fb_info *info)
{
	struct lcd_drvdata *drv = info->par;
	unsigned long flags;
	dma_addr_t next;
	unsigned int page;
	long waited;

	if (!var->yres || var->xoffset || var->yres_virtual < var->yres ||
	    var->yoffset % var->yres ||
	    var->yoffset > var->yres_virtual - var->yres)
		return -EINVAL;
	page = var->yoffset / var->yres;
	next = info->fix.smem_start +
	       (dma_addr_t)info->fix.line_length * var->yoffset;
	reinit_completion(&drv->frame_done);
	spin_lock_irqsave(&drv->lock, flags);
	writel(next, &drv->lcdif->NEXT_BUF);
	wmb();
	drv->current_page = page;
	spin_unlock_irqrestore(&drv->lock, flags);
	if (!drv->irq_requested) {
		info->var.yoffset = var->yoffset;
		return 0;
	}
	writel(LCDIF_CTRL1_CUR_FRAME_DONE_IRQ_EN, &drv->lcdif->CTRL1_SET);
	waited = wait_for_completion_timeout(&drv->frame_done, HZ / 2);
	writel(LCDIF_CTRL1_CUR_FRAME_DONE_IRQ_EN, &drv->lcdif->CTRL1_CLR);
	if (!waited)
		return -ETIMEDOUT;
	info->var.yoffset = var->yoffset;
	return 0;
}

/**
 * @brief Wait for a LCDIF VSYNC event through the standard fbdev ioctl.
 * @param [in] info Framebuffer descriptor owning the LCDIF instance.
 * @param [in] cmd Ioctl command from the framebuffer client.
 * @param [in] arg Userspace argument, unused for FBIO_WAITFORVSYNC.
 * @retval 0 VSYNC was observed.
 * @retval -ENOTTY Command is unsupported or no LCDIF IRQ is available.
 * @retval -ETIMEDOUT VSYNC did not arrive within 500 ms.
 */
static int mylcd_ioctl(struct fb_info *info, unsigned int cmd,
			       unsigned long arg)
{
	struct lcd_drvdata *drv = info->par;
	long waited;

	(void)arg;
	if (cmd != FBIO_WAITFORVSYNC || !drv->irq_requested)
		return -ENOTTY;
	reinit_completion(&drv->vsync_done);
	writel(LCDIF_CTRL1_VSYNC_EDGE_IRQ_EN, &drv->lcdif->CTRL1_SET);
	waited = wait_for_completion_timeout(&drv->vsync_done, HZ / 2);
	writel(LCDIF_CTRL1_VSYNC_EDGE_IRQ_EN, &drv->lcdif->CTRL1_CLR);
	return waited ? 0 : -ETIMEDOUT;
}

static struct fb_ops myfb_ops = {
	.owner = THIS_MODULE,
	.fb_fillrect = cfb_fillrect,
	.fb_copyarea = cfb_copyarea,
	.fb_imageblit = cfb_imageblit,
	.fb_pan_display = mylcd_pan_display,
	.fb_mmap = mylcd_mmap,
	.fb_ioctl = mylcd_ioctl,
};

/**
 * @brief Probe the LCD platform device and register a two-page fbdev.
 * @param [in] pdev Platform device matched from the device tree.
 * @retval 0 Driver initialized successfully.
 * @retval Negative errno Hardware, timing, memory, or registration setup failed.
 */
static int mylcd_probe(struct platform_device *pdev)
{
	struct lcd_drvdata *drv;
	struct fb_info *info;
	struct device_node *display_np;
	struct display_timings *timings = NULL;
	struct display_timing *dt;
	struct resource *res;
	struct gpio_desc *bl_gpio;
	struct clk *clk_pix;
	struct clk *clk_axi;
	int bus_width;
	int bits_per_pixel;
	int ret;
	int irq;
	size_t page_size;

	display_np = of_parse_phandle(pdev->dev.of_node, "display", 0);
	if (!display_np)
		return -EINVAL;
	ret = of_property_read_u32(display_np, "bus-width", &bus_width);
	if (ret)
		goto put_display;
	ret = of_property_read_u32(display_np, "bits-per-pixel", &bits_per_pixel);
	if (ret)
		goto put_display;
	(void)bus_width;
	timings = of_get_display_timings(display_np);
	if (!timings || timings->native_mode >= timings->num_timings) {
		ret = -EINVAL;
		goto put_display;
	}
	dt = timings->timings[timings->native_mode];
	if (!dt || !dt->hactive.typ || !dt->vactive.typ ||
	    dt->vactive.typ > (~0U / 2)) {
		ret = -EINVAL;
		goto put_display;
	}

	bl_gpio = devm_gpiod_get_optional(&pdev->dev, "backlight", GPIOD_OUT_HIGH);
	if (IS_ERR(bl_gpio)) {
		ret = PTR_ERR(bl_gpio);
		goto put_display;
	}
	clk_pix = devm_clk_get(&pdev->dev, "pix");
	if (IS_ERR(clk_pix)) {
		ret = PTR_ERR(clk_pix);
		goto put_display;
	}
	clk_axi = devm_clk_get(&pdev->dev, "axi");
	if (IS_ERR(clk_axi)) {
		ret = PTR_ERR(clk_axi);
		goto put_display;
	}
	ret = clk_set_rate(clk_pix, dt->pixelclock.typ);
	if (ret)
		goto put_display;
	ret = clk_prepare_enable(clk_pix);
	if (ret)
		goto put_display;
	ret = clk_prepare_enable(clk_axi);
	if (ret)
		goto disable_pix;

	drv = devm_kzalloc(&pdev->dev, sizeof(*drv), GFP_KERNEL);
	if (!drv) {
		ret = -ENOMEM;
		goto disable_clocks;
	}
	drv->dev = &pdev->dev;
	drv->clk_pix = clk_pix;
	drv->clk_axi = clk_axi;
	spin_lock_init(&drv->lock);
	init_completion(&drv->frame_done);
	init_completion(&drv->vsync_done);

	info = framebuffer_alloc(0, &pdev->dev);
	if (!info) {
		ret = -ENOMEM;
		goto disable_clocks;
	}
	drv->info = info;
	info->par = drv;
	info->var.xres = dt->hactive.typ;
	info->var.yres = dt->vactive.typ;
	info->var.xres_virtual = info->var.xres;
	info->var.yres_virtual = info->var.yres * 2;
	info->var.bits_per_pixel = 16;
	info->var.activate = FB_ACTIVATE_NOW;
	info->var.red.offset = 11;
	info->var.red.length = 5;
	info->var.green.offset = 5;
	info->var.green.length = 6;
	info->var.blue.offset = 0;
	info->var.blue.length = 5;
	strlcpy(info->fix.id, "hxh_lcd", sizeof(info->fix.id));
	info->fix.type = FB_TYPE_PACKED_PIXELS;
	info->fix.visual = FB_VISUAL_TRUECOLOR;
	info->fix.accel = FB_ACCEL_NONE;
	info->fix.line_length = info->var.xres * 2;
	page_size = (size_t)info->fix.line_length * info->var.yres;
	drv->page_size = page_size;
	drv->fb_size = page_size * 2;
	info->fix.smem_len = drv->fb_size;
	drv->fb_dma = 0;
	info->screen_base = dma_alloc_wc(&pdev->dev, drv->fb_size,
					&drv->fb_dma, GFP_KERNEL);
	if (!info->screen_base) {
		ret = -ENOMEM;
		goto release_fb;
	}
	info->fix.smem_start = drv->fb_dma;
	info->fbops = &myfb_ops;
	info->pseudo_palette = pseudo_palette;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res) {
		ret = -ENODEV;
		goto free_dma;
	}
	drv->lcdif = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(drv->lcdif)) {
		ret = PTR_ERR(drv->lcdif);
		goto free_dma;
	}
	ret = lcd_controller_init(drv->lcdif, dt, bits_per_pixel, 16,
				  drv->fb_dma);
	if (ret)
		goto free_dma;

	irq = platform_get_irq(pdev, 0);
	if (irq == -EPROBE_DEFER) {
		ret = irq;
		goto free_dma;
	}
	if (irq >= 0) {
		ret = devm_request_irq(&pdev->dev, irq, lcdif_irq_handler, 0,
				       dev_name(&pdev->dev), drv);
		if (!ret) {
			drv->irq = irq;
			drv->irq_requested = true;
		} else {
			dev_warn(&pdev->dev,
				 "LCDIF IRQ unavailable, page flip will not wait: %d\n",
				 ret);
			drv->irq = -1;
		}
	} else {
		drv->irq = -1;
		dev_warn(&pdev->dev,
			 "LCDIF has no IRQ, page flip will use asynchronous fallback\n");
	}

	lcd_controller_enable(drv->lcdif);
	if (bl_gpio)
		gpiod_set_value(bl_gpio, 1);
	platform_set_drvdata(pdev, drv);
	ret = register_framebuffer(info);
	if (ret)
		goto free_dma;
	dev_info(&pdev->dev, "registered %ux%u RGB565 framebuffer, %s IRQ\n",
		 info->var.xres, info->var.yres,
		 drv->irq_requested ? "with" : "without");
	display_timings_release(timings);
	of_node_put(display_np);
	return 0;

free_dma:
	dma_free_wc(&pdev->dev, drv->fb_size, info->screen_base, drv->fb_dma);
release_fb:
	framebuffer_release(info);
disable_clocks:
	clk_disable_unprepare(clk_axi);
disable_pix:
	clk_disable_unprepare(clk_pix);
put_display:
	if (timings)
		display_timings_release(timings);
	of_node_put(display_np);
	return ret;
}

/**
 * @brief Stop the LCDIF device and release framebuffer resources.
 * @param [in] pdev Platform device being removed.
 * @retval 0 Resources were released.
 */
static int mylcd_remove(struct platform_device *pdev)
{
	struct lcd_drvdata *drv = platform_get_drvdata(pdev);
	struct fb_info *info;

	if (!drv)
		return 0;
	info = drv->info;
	writel(LCDIF_CTRL1_CUR_FRAME_DONE_IRQ_EN |
	       LCDIF_CTRL1_VSYNC_EDGE_IRQ_EN, &drv->lcdif->CTRL1_CLR);
	writel(readl(&drv->lcdif->CTRL) & ~1U, &drv->lcdif->CTRL);
	if (info) {
		unregister_framebuffer(info);
		dma_free_wc(&pdev->dev, drv->fb_size, info->screen_base,
			     drv->fb_dma);
		framebuffer_release(info);
	}
	clk_disable_unprepare(drv->clk_axi);
	clk_disable_unprepare(drv->clk_pix);
	return 0;
}

static const struct of_device_id mylcd_of_match[] = {
	{ .compatible = "hxh,lcd_drv" },
	{ }
};
MODULE_DEVICE_TABLE(of, mylcd_of_match);

static struct platform_driver mylcd_driver = {
	.driver = {
		.name = "mylcd",
		.of_match_table = mylcd_of_match,
	},
	.probe = mylcd_probe,
	.remove = mylcd_remove,
};

/**
 * @brief Register the LCD platform driver with the kernel.
 * @param [in] None.
 * @retval 0 Registration succeeded.
 * @retval Negative errno Registration failed.
 */
static int __init lcd_drv_init(void)
{
	return platform_driver_register(&mylcd_driver);
}

/**
 * @brief Unregister the LCD platform driver during module removal.
 * @param [in] None.
 * @retval None.
 */
static void __exit lcd_drv_exit(void)
{
	platform_driver_unregister(&mylcd_driver);
}

module_init(lcd_drv_init);
module_exit(lcd_drv_exit);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("hxh");
MODULE_DESCRIPTION("i.MX6ULL LCDIF framebuffer driver");
