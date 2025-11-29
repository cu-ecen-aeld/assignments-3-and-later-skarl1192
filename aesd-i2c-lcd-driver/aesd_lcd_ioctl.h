#ifndef AESDLCD_IOCTL_H
#define AESDLCD_IOCTL_H

#include <linux/ioctl.h>

/* Pick an arbitrary unused value from https://github.com/torvalds/linux/blob/master/Documentation/userspace-api/ioctl/ioctl-number.rst */
#define LCD_IOC_MAGIC 0x17

/* Define user commands for LCD Driver IOCTL */
#define LCD_CLEAR           _IO(LCD_IOC_MAGIC, 1)
#define LCD_SET_CURSOR      _IOW(LCD_IOC_MAGIC, 2, int)  /* Pass position (row << 8 | col) */
#define LCD_BACKLIGHT       _IOW(LCD_IOC_MAGIC, 3, int)  /* 1 for on, 0 for off */
#define LCD_HOME            _IO(LCD_IOC_MAGIC, 4)
#define LCD_DISPLAY_SWITCH  _IOW(LCD_IOC_MAGIC, 5, int)  /* 1 for on, 0 for off */
#define LCD_CURSOR_SWITCH   _IOW(LCD_IOC_MAGIC, 6, int)  /* 1 for on, 0 for off */
#define LCD_BLINK_SWITCH    _IOW(LCD_IOC_MAGIC, 7, int)  /* 1 for on, 0 for off */
#define LCD_SCROLL          _IOW(LCD_IOC_MAGIC, 8, int)  /* 0 for left, 1 for right */
#define LCD_TEXT_DIR        _IOW(LCD_IOC_MAGIC, 9, int)  /* 0 for Right-to-Left, 1 for Left-to-Right */
#define LCD_AUTOSCROLL      _IOW(LCD_IOC_MAGIC, 10, int) /* 1 for on, 0 for off */

/* Scroll direction constants */
#define LCD_SCROLL_LEFT     0
#define LCD_SCROLL_RIGHT    1

/* Text direction constants */
#define LCD_TEXT_RTL        0
#define LCD_TEXT_LTR        1

#endif