#include "adriver.h"
/*
 *   ALSA driver for TEA5757/5759 Philips AM/FM radio tuner chips
 *
 *	Copyright (c) 2004 Jaroslav Kysela <perex@perex.cz>
 *
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307 USA
 *
 */

#include <asm/io.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/init.h>
#include <sound/core.h>
#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 29)
#define v4l2_file_operations	file_operations
#endif
#include <sound/tea575x-tuner.h>

MODULE_AUTHOR("Jaroslav Kysela <perex@perex.cz>");
MODULE_DESCRIPTION("Routines for control of TEA5757/5759 Philips AM/FM radio tuner chips");
MODULE_LICENSE("GPL");

/*
 * definitions
 */

#define TEA575X_BIT_SEARCH	(1<<24)		/* 1 = search action, 0 = tuned */
#define TEA575X_BIT_UPDOWN	(1<<23)		/* 0 = search down, 1 = search up */
#define TEA575X_BIT_MONO	(1<<22)		/* 0 = stereo, 1 = mono */
#define TEA575X_BIT_BAND_MASK	(3<<20)
#define TEA575X_BIT_BAND_FM	(0<<20)
#define TEA575X_BIT_BAND_MW	(1<<20)
#define TEA575X_BIT_BAND_LW	(1<<21)
#define TEA575X_BIT_BAND_SW	(1<<22)
#define TEA575X_BIT_PORT_0	(1<<19)		/* user bit */
#define TEA575X_BIT_PORT_1	(1<<18)		/* user bit */
#define TEA575X_BIT_SEARCH_MASK	(3<<16)		/* search level */
#define TEA575X_BIT_SEARCH_5_28	     (0<<16)	/* FM >5uV, AM >28uV */
#define TEA575X_BIT_SEARCH_10_40     (1<<16)	/* FM >10uV, AM > 40uV */
#define TEA575X_BIT_SEARCH_30_63     (2<<16)	/* FM >30uV, AM > 63uV */
#define TEA575X_BIT_SEARCH_150_1000  (3<<16)	/* FM > 150uV, AM > 1000uV */
#define TEA575X_BIT_DUMMY	(1<<15)		/* buffer */
#define TEA575X_BIT_FREQ_MASK	0x7fff

#ifndef CONFIG_HAVE_VIDEO_GET_DRVDATA
/* helper functions to access driver private data. */
static inline void *video_get_drvdata(struct video_device *dev)
{
	return dev->priv;
}

static inline void video_set_drvdata(struct video_device *dev, void *data)
{
	dev->priv = data;
}
#endif

/*
 * lowlevel part
 */

static void snd_tea575x_set_freq(struct snd_tea575x *tea)
{
	unsigned long freq;

	freq = tea->freq / 16;		/* to kHz */
	if (freq > 108000)
		freq = 108000;
	if (freq < 87000)
		freq = 87000;
	/* crystal fixup */
	if (tea->tea5759)
		freq -= tea->freq_fixup;
	else
		freq += tea->freq_fixup;
	/* freq /= 12.5 */
	freq *= 10;
	freq /= 125;

	tea->val &= ~TEA575X_BIT_FREQ_MASK;
	tea->val |= freq & TEA575X_BIT_FREQ_MASK;
	tea->ops->write(tea, tea->val);
}

/*
 * Linux Video interface
 */

#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 5, 0)
static int snd_tea575x_open(struct video_device *dev, int flags)
{
	return 0;
}

static void snd_tea575x_close(struct video_device *dev)
{
}
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 29)
static long snd_tea575x_ioctl(struct file *file,
			     unsigned int cmd, unsigned long data)
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(2, 5, 0)
static int snd_tea575x_ioctl(struct inode *inode, struct file *file,
			     unsigned int cmd, unsigned long data)
#else
static int snd_tea575x_ioctl(struct video_device *dev,
			     unsigned int cmd, void __user *arg)
#endif
{
#ifdef CONFIG_HAVE_VIDEO_DRVDATA
	struct snd_tea575x *tea = video_drvdata(file);
#else
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 5, 0)
	struct video_device *dev = video_devdata(file);
#endif
	struct snd_tea575x *tea = video_get_drvdata(dev);
#endif
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 5, 0)
	void __user *arg = (void __user *)data;
#endif

	switch(cmd) {
		case VIDIOCGCAP:
		{
			struct video_capability v;
			v.type = VID_TYPE_TUNER;
			v.channels = 1;
			v.audios = 1;
			/* No we don't do pictures */
			v.maxwidth = 0;
			v.maxheight = 0;
			v.minwidth = 0;
			v.minheight = 0;
			strcpy(v.name, tea->tea5759 ? "TEA5759" : "TEA5757");
			if (copy_to_user(arg,&v,sizeof(v)))
				return -EFAULT;
			return 0;
		}
		case VIDIOCGTUNER:
		{
			struct video_tuner v;
			if (copy_from_user(&v, arg,sizeof(v))!=0)
				return -EFAULT;
			if (v.tuner)	/* Only 1 tuner */
				return -EINVAL;
			v.rangelow = (87*16000);
			v.rangehigh = (108*16000);
			v.flags = VIDEO_TUNER_LOW;
			v.mode = VIDEO_MODE_AUTO;
			strcpy(v.name, "FM");
			v.signal = 0xFFFF;
			if (copy_to_user(arg, &v, sizeof(v)))
				return -EFAULT;
			return 0;
		}
		case VIDIOCSTUNER:
		{
			struct video_tuner v;
			if(copy_from_user(&v, arg, sizeof(v)))
				return -EFAULT;
			if(v.tuner!=0)
				return -EINVAL;
			/* Only 1 tuner so no setting needed ! */
			return 0;
		}
		case VIDIOCGFREQ:
			if(copy_to_user(arg, &tea->freq, sizeof(tea->freq)))
				return -EFAULT;
			return 0;
		case VIDIOCSFREQ:
			if(copy_from_user(&tea->freq, arg, sizeof(tea->freq)))
				return -EFAULT;
			snd_tea575x_set_freq(tea);
			return 0;
		case VIDIOCGAUDIO:
		{
			struct video_audio v;
			memset(&v, 0, sizeof(v));
			strcpy(v.name, "Radio");
			if(copy_to_user(arg,&v, sizeof(v)))
				return -EFAULT;
			return 0;
		}
		case VIDIOCSAUDIO:
		{
			struct video_audio v;
			if(copy_from_user(&v, arg, sizeof(v)))
				return -EFAULT;
			if (tea->ops->mute)
				tea->ops->mute(tea,
					       (v.flags &
						VIDEO_AUDIO_MUTE) ? 1 : 0);
			if(v.audio)
				return -EINVAL;
			return 0;
		}
		default:
			return -ENOIOCTLCMD;
	}
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 8)
static void snd_tea575x_release(struct video_device *vfd)
{
}
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 5, 0)
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 29)
static int snd_tea575x_exclusive_open(struct file *file)
#else
static int snd_tea575x_exclusive_open(struct inode *inode, struct file *file)
#endif
{
#ifdef CONFIG_HAVE_VIDEO_DRVDATA
	struct snd_tea575x *tea = video_drvdata(file);
#else
	struct video_device *dev = video_devdata(file);
	struct snd_tea575x *tea = video_get_drvdata(dev);
#endif
	return test_and_set_bit(0, &tea->in_use) ? -EBUSY : 0;
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 29)
static int snd_tea575x_exclusive_release(struct file *file)
#else
static int snd_tea575x_exclusive_release(struct inode *inode, struct file *file)
#endif
{
#ifdef CONFIG_HAVE_VIDEO_DRVDATA
	struct snd_tea575x *tea = video_drvdata(file);
#else
	struct video_device *dev = video_devdata(file);
	struct snd_tea575x *tea = video_get_drvdata(dev);
#endif
	clear_bit(0, &tea->in_use);
	return 0;
}
#endif /* >= 2.5.0 */

/*
 * initialize all the tea575x chips
 */
void snd_tea575x_init(struct snd_tea575x *tea)
{
	unsigned int val;

	val = tea->ops->read(tea);
	if (val == 0x1ffffff || val == 0) {
		snd_printk(KERN_ERR "Cannot find TEA575x chip\n");
		return;
	}

	memset(&tea->vd, 0, sizeof(tea->vd));
#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 27)
	tea->vd.owner = tea->card->module;
#endif
	strcpy(tea->vd.name, tea->tea5759 ? "TEA5759 radio" : "TEA5757 radio");
#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 24)
	tea->vd.hardware = VID_HARDWARE_RTRACK;	/* FIXME: assign new number */
#endif
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 8)
	tea->vd.release = snd_tea575x_release;
#endif
#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 27)
	tea->vd.type = VID_TYPE_TUNER;
#endif
	video_set_drvdata(&tea->vd, tea);
	tea->in_use = 0;
#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 5, 0)
	tea->vd.open = snd_tea575x_open;
	tea->vd.close = snd_tea575x_close;
	tea->vd.ioctl = snd_tea575x_ioctl;
#else
	tea->vd.fops = &tea->fops;
	tea->fops.owner = tea->card->module;
	tea->fops.open = snd_tea575x_exclusive_open;
	tea->fops.release = snd_tea575x_exclusive_release;
	tea->fops.ioctl = snd_tea575x_ioctl;
#endif
	if (video_register_device(&tea->vd, VFL_TYPE_RADIO, tea->dev_nr - 1) < 0) {
		snd_printk(KERN_ERR "unable to register tea575x tuner\n");
		return;
	}
	tea->vd_registered = 1;

	tea->val = TEA575X_BIT_BAND_FM | TEA575X_BIT_SEARCH_10_40;
	tea->freq = 90500 * 16;		/* 90.5Mhz default */

	snd_tea575x_set_freq(tea);

	/* mute on init */
	if (tea->ops->mute)
		tea->ops->mute(tea, 1);
}

void snd_tea575x_exit(struct snd_tea575x *tea)
{
	if (tea->vd_registered) {
		video_unregister_device(&tea->vd);
		tea->vd_registered = 0;
	}
}

static int __init alsa_tea575x_module_init(void)
{
	return 0;
}

static void __exit alsa_tea575x_module_exit(void)
{
}

module_init(alsa_tea575x_module_init)
module_exit(alsa_tea575x_module_exit)

EXPORT_SYMBOL(snd_tea575x_init);
EXPORT_SYMBOL(snd_tea575x_exit);
