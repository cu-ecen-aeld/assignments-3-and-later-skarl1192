#include <linux/module.h>
#include <linux/init.h>
#include <linux/i2c.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include "aesd_lcd_ioctl.h"

#define DRIVER_NAME "aesdlcd_driver"
#define LCD_CLASS_NAME "aesdlcd_class"

/*
 * HD44780 LCD CONTROL VIA I2C
 * =========================================================================
 *
 * 1. I2C "BACKPACK" DATA BYTE MAPPING
 * -------------------------------------------------------------------------
 * The PCF8574 IO expander connects the 8-bit I2C byte to the LCD pins.
 * In 4-bit mode, only the upper nibble (D4-D7) is used for data.
 *
 * Bit 7 | Bit 6 | Bit 5 | Bit 4 | Bit 3 | Bit 2 | Bit 1 | Bit 0
 * ------+-------+-------+-------+-------+-------+-------+------
 *   D7  |   D6  |   D5  |   D4  |   BL  |   EN  |   RW  |   RS
 *
 * D7-D4 : LCD Data Bus (Sends High Nibble, then Low Nibble)
 * BL    : Backlight (1 = On, 0 = Off)
 * EN    : Enable (Pulse High->Low to latch data)
 * RW    : Read/Write (0 = Write)
 * RS    : Register Select (0 = Command, 1 = Data)
 *
 * 2. HD44780 INSTRUCTION SET FORMAT (Sent with RS=0)
 * -------------------------------------------------------------------------
 * Instruction               | D7  D6  D5  D4  D3  D2  D1  D0
 * --------------------------+-------------------------------
 * Clear Display             |  0   0   0   0   0   0   0   1
 * Return Home               |  0   0   0   0   0   0   1   -
 * Entry Mode Set            |  0   0   0   0   0   1  I/D  S
 * Display On/Off            |  0   0   0   0   1   D   C   B
 * Cursor/Display Shift      |  0   0   0   1  S/C R/L  -   -
 * Function Set              |  0   0   1   DL  N   F   -   -
 * Set CGRAM Address         |  0   1  ACG ACG ACG ACG ACG ACG
 * Set DDRAM Address         |  1  ADD ADD ADD ADD ADD ADD ADD
 *
 * Legend:
 * I/D=Inc/Dec, S=Shift, D=Disp On, C=Curs On, B=Blink,
 * DL=DataLen (0=4bit), N=Lines, F=Font, ACG=CGRAM, ADD=DDRAM
 *
 * 3. EXAMPLE 1: Sending Data (Letter 'A' -> 0x41)
 * -------------------------------------------------------------------------
 * Character 'A' binary: 0100 0001
 * Context: RS=1 (Data), RW=0 (Write), BL=1 (Backlight On)
 *
 * -- PASS 1: High Nibble (0100) --
 * 1. Setup: D7..4=0100, EN=1, RS=1. Byte: 0100 1101 (0x4D) -> Prepare Latch
 * 2. Latch: D7..4=0100, EN=0, RS=1. Byte: 0100 1001 (0x49) -> Execute
 *
 * -- PASS 2: Low Nibble (0001) --
 * 3. Setup: D7..4=0001, EN=1, RS=1. Byte: 0001 1101 (0x1D) -> Prepare Latch
 * 4. Latch: D7..4=0001, EN=0, RS=1. Byte: 0001 1001 (0x19) -> Execute
 *
 * I2C Stream: 0x4D, 0x49, 0x1D, 0x19
 *
 * 4. EXAMPLE 2: Sending Command (Clear Display -> 0x01)
 * -------------------------------------------------------------------------
 * Instruction Code: 0000 0001
 * Context: RS=0 (Command), RW=0 (Write), BL=1 (Backlight On)
 * IMPORTANT: "Clear Display" is slow! Must wait >1.52ms after sending.
 *
 * -- PASS 1: High Nibble (0000) --
 * 1. Setup: D7..4=0000, EN=1, RS=0. Byte: 0000 1100 (0x0C) -> Prepare Latch
 * 2. Latch: D7..4=0000, EN=0, RS=0. Byte: 0000 1000 (0x08) -> Execute
 *
 * -- PASS 2: Low Nibble (0001) --
 * 3. Setup: D7..4=0001, EN=1, RS=0. Byte: 0001 1100 (0x1C) -> Prepare Latch
 * 4. Latch: D7..4=0001, EN=0, RS=0. Byte: 0001 1000 (0x18) -> Execute
 *
 * I2C Stream: 0x0C, 0x08, 0x1C, 0x18 -> then Sleep(2)
 */


/* Define the maximum IOCTL command number for validation checks.
 * Currently 10 commands are defined in aesdlcd_ioctl.h */
#define LCD_IOC_MAXNR 10

/* PCF8574 Pin Definitions */
#define LCD_RS_BIT      (1 << 0)
#define LCD_RW_BIT      (1 << 1)
#define LCD_EN_BIT      (1 << 2)
#define LCD_BL_BIT      (1 << 3)
#define LCD_D4_BIT      (1 << 4)
#define LCD_D5_BIT      (1 << 5)
#define LCD_D6_BIT      (1 << 6)
#define LCD_D7_BIT      (1 << 7)

/* LCD Commands */
#define LCD_CMD_CLEAR           0x01
#define LCD_CMD_RETURN_HOME     0x02
#define LCD_CMD_ENTRY_MODE      0x04
#define LCD_CMD_DISPLAY_CTRL    0x08
#define LCD_CMD_SHIFT           0x10
#define LCD_CMD_FUNCTION_SET    0x20
#define LCD_CMD_SET_CGRAM_ADDR  0x40
#define LCD_CMD_SET_DDRAM_ADDR  0x80

/* Flags for display entry mode */
#define LCD_ENTRY_RIGHT         0x00
#define LCD_ENTRY_LEFT          0x02
#define LCD_ENTRY_SHIFT_INC     0x01
#define LCD_ENTRY_SHIFT_DEC     0x00

/* Flags for display on/off control */
#define LCD_DISPLAY_ON          0x04
#define LCD_DISPLAY_OFF         0x00
#define LCD_CURSOR_ON           0x02
#define LCD_CURSOR_OFF          0x00
#define LCD_BLINK_ON            0x01
#define LCD_BLINK_OFF           0x00

/* Flags for display/cursor shift */
#define LCD_DISPLAY_MOVE        0x08
#define LCD_CURSOR_MOVE         0x00
#define LCD_MOVE_RIGHT          0x04
#define LCD_MOVE_LEFT           0x00

/* Flags for function set */
#define LCD_8BIT_MODE           0x10
#define LCD_4BIT_MODE           0x00
#define LCD_2LINE               0x08
#define LCD_1LINE               0x00
#define LCD_5x10DOTS            0x04
#define LCD_5x8DOTS             0x00


MODULE_LICENSE("Dual BSD/GPL");
MODULE_AUTHOR("Scott Karl");
MODULE_DESCRIPTION("AESD I2C LCD Driver for Raspberry Pi");

/**
 * struct lcd_dev - Internal device structure
 * @client: Pointer to the I2C client struct provided by the kernel
 * @cdev: Character device structure for kernel registration
 * @class: Class structure for sysfs registration
 * @dev_num: The major/minor device number
 * @backlight_state: The state of the backlight (ON/OFF bit)
 * @display_ctrl: The Display/Cursor/Blink command bits
 * @display_mode: The Entry Mode (text direction/autoscroll) bits
 */
struct lcd_dev {
    struct i2c_client *client;
    struct cdev cdev;
    struct class *class;
    dev_t dev_num;
    u8 backlight_state;
    u8 display_ctrl;
    u8 display_mode;
};

/*
 * Write a byte to the I2C device
 */
static void lcd_i2c_write_byte(struct lcd_dev *lcd, u8 data)
{
    /* Call the Linux Kernal API provided by the I2C subsystem.
     * Sends a single byte over the physical I2C bus to the specified client address */
    i2c_smbus_write_byte(lcd->client, data);
}

/*
 * Pulse the Enable bit to latch data
 */
static void lcd_pulse_enable(struct lcd_dev *lcd, u8 data)
{
    lcd_i2c_write_byte(lcd, data | LCD_EN_BIT);
    udelay(1);
    lcd_i2c_write_byte(lcd, data & ~LCD_EN_BIT);
    udelay(50);
}

/*
 * Send a nibble (4 bits) to the LCD.
 * 
 * The HD44780 datasheet states that in 4-bit mode, data is sent to the 
 * top 4 pins of the data bus (pins 4 through 7).
 * 
 * @nibble: The byte containing the data in the upper 4 bits (MSB).
 *          The lower 4 bits are masked out.
 * @rs:     Register Select (0=Command, 1=Data).
 * 
 * NOTE:
 * The kernel function i2c_smbus_write_byte() executes a COMPLETE I2C transaction
 * for every call: [START] [ADDR] [DATA] [STOP].
 * * To latch a single 4-bit nibble into the LCD, this driver performs THREE 
 * separate I2C writes. Crucially, the data nibble (bits 4-7) is sent 
 * IDENTICALLY three times to ensure the data pins are stable while the 
 * Enable pin (bit 2) is toggled:
 * 1. Setup: [Nibble | EN=0] -> Stabilizes D4-D7 pins (Nibble sent 1st time)
 * 2. Pulse: [Nibble | EN=1] -> Latch data           (Nibble sent 2nd time)
 * 3. Hold:  [Nibble | EN=0] -> Complete pulse       (Nibble sent 3rd time)
 */
static void lcd_send_nibble(struct lcd_dev *lcd, u8 nibble, u8 rs)
{
    u8 data = (nibble & 0xF0) | rs | lcd->backlight_state;
    lcd_i2c_write_byte(lcd, data);
    lcd_pulse_enable(lcd, data);
}

/*
 * Send a full byte to the LCD (split into two nibbles)
 */
static void lcd_send_byte(struct lcd_dev *lcd, u8 byte, u8 rs)
{
    u8 high_nibble = byte & 0xF0;
    u8 low_nibble = (byte << 4) & 0xF0;
    
    lcd_send_nibble(lcd, high_nibble, rs);
    lcd_send_nibble(lcd, low_nibble, rs);
}

/*
 * Send a command to the LCD
 */
static void lcd_command(struct lcd_dev *lcd, u8 cmd)
{
    lcd_send_byte(lcd, cmd, 0);
}

/*
 * Send data to the LCD
 */
static void lcd_data(struct lcd_dev *lcd, u8 data)
{
    lcd_send_byte(lcd, data, LCD_RS_BIT);
}

/**
 * lcd_init_sequence() - Runs the HD44780 initialization logic
 * @lcd: Pointer to the local device structure
 *
 * Implements the specific 4-bit initialization procedure required by the 
 * controller datasheet. Sets default state (Display On, Cursor Off, etc.).
 */
static void lcd_init_sequence(struct lcd_dev *lcd)
{
    /* Wait for more than 40ms after VCC rises to 2.7V. */
    msleep(50);

    /* Initialization sequence for 4-bit mode */
    lcd_send_nibble(lcd, 0x30, 0);
    mdelay(5); 
    
    lcd_send_nibble(lcd, 0x30, 0);
    mdelay(5); 
    
    lcd_send_nibble(lcd, 0x30, 0);
    udelay(150); 
    
    /* Finally, set to 4-bit interface */
    lcd_send_nibble(lcd, 0x20, 0); 

    /* Function set: 4-bit, 2 line, 5x8 dots */
    lcd_command(lcd, LCD_CMD_FUNCTION_SET | LCD_4BIT_MODE | LCD_2LINE | LCD_5x8DOTS);
    
    /* Default: Display on, Cursor off, Blink off */
    lcd->display_ctrl = LCD_DISPLAY_ON | LCD_CURSOR_OFF | LCD_BLINK_OFF;
    lcd_command(lcd, LCD_CMD_DISPLAY_CTRL | lcd->display_ctrl);
    
    /* Clear display */
    lcd_command(lcd, LCD_CMD_CLEAR);
    mdelay(2);
    
    /* Default Entry mode: Increment cursor, no shift */
    lcd->display_mode = LCD_ENTRY_LEFT | LCD_ENTRY_SHIFT_DEC;
    lcd_command(lcd, LCD_CMD_ENTRY_MODE | lcd->display_mode);
    
    /* Return home */
    lcd_command(lcd, LCD_CMD_RETURN_HOME);
    mdelay(2);
}

static int lcd_open(struct inode *inode, struct file *file)
{
    struct lcd_dev *lcd = container_of(inode->i_cdev, struct lcd_dev, cdev);
    file->private_data = lcd;
    return 0;
}

static int lcd_release(struct inode *inode, struct file *file)
{
    return 0;
}

/**
 * lcd_write() - Handle write system calls to the device
 * @file: File pointer providing access to the device private data
 * @buf: User space buffer containing the text to write
 * @count: Number of bytes to write
 * @ppos: File offset (ignored for this character device)
 *
 * Copies data from user space to kernel space and sends it byte-by-byte
 * to the LCD. Note that we allocate a kernel buffer to ensure safe access.
 *
 * Return: Number of bytes written on success, or negative error code
 */
static ssize_t lcd_write(struct file *file, const char __user *buf, size_t count, loff_t *ppos)
{
    struct lcd_dev *lcd = file->private_data;
    char *kbuf;
    int i;
    
    /* Limit write size to prevent excessive kernel memory allocation */
    if (count > 4096)
        count = 4096;

    kbuf = kmalloc(count, GFP_KERNEL);
    if (!kbuf)
        return -ENOMEM;
        
    if (copy_from_user(kbuf, buf, count)) {
        kfree(kbuf);
        return -EFAULT;
    }
    
    for (i = 0; i < count; i++) {
        lcd_data(lcd, kbuf[i]);
    }
    
    kfree(kbuf);
    return count;
}

/**
 * lcd_ioctl() - Handle ioctl commands for the LCD device
 * @file: File pointer for the open device instance
 * @cmd: ioctl command number (encoded with direction, size, type, and number)
 * @arg: Argument passed from user space. For most commands in this driver,
 * the value is passed directly in 'arg'. For complex structures (like custom chars),
 * 'arg' is a pointer to user space memory.
 *
 * Supports commands for:
 * - Clearing screen / Returning Home
 * - Moving Cursor (row/col)
 * - Toggling Backlight / Display / Cursor / Blink
 * - Scrolling text
 * - Loading Custom Characters
 *
 * Return: 0 on success, negative error code on failure (e.g., -ENOTTY, -EFAULT)
 */
static long lcd_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    struct lcd_dev *lcd = file->private_data;
    
    /* Verify that the ioctl command is valid for this driver.
     * The ioctl command number is encoded with several fields using _IOC() macro:
     * - Type (magic number): Identifies which driver this command belongs to
     * - Number: The specific command within this driver
     * * _IOC_TYPE extracts the magic number - must match LCD_IOC_MAGIC ('k').
     * _IOC_NR extracts the command number - must be <= LCD_IOC_MAXNR.
     * * Returning -ENOTTY (inappropriate ioctl for device) is the standard way
     * to indicate "this ioctl command doesn't belong to this driver". */
    if (_IOC_TYPE(cmd) != LCD_IOC_MAGIC) return -ENOTTY;
    if (_IOC_NR(cmd) > LCD_IOC_MAXNR) return -ENOTTY;
    
    switch (cmd) {
        case LCD_CLEAR:
            /* Clear display command: writes space code 0x20 to all DDRAM addresses */
            lcd_command(lcd, LCD_CMD_CLEAR);
            /* This command requires > 1.52ms execution time according to datasheet */
            mdelay(2);
            break;

        case LCD_HOME:
            /* Return Home command: Sets DDRAM address 0 in address counter.
             * Returns display from being shifted to original position. */
            lcd_command(lcd, LCD_CMD_RETURN_HOME);
            mdelay(2);
            break;
            
        case LCD_SET_CURSOR:
            {
                /* Decode the argument: High 8 bits = Row, Low 8 bits = Column.
                 * 'arg' contains the integer value directly. */
                int row = (arg >> 8) & 0xFF;
                int col = arg & 0xFF;
                
                /* Standard DDRAM offsets for common LCD sizes (16x2, 20x4).
                 * Row 0 starts at 0x00, Row 1 at 0x40, etc. */
                int row_offsets[] = { 0x00, 0x40, 0x14, 0x54 };
                
                if (row >= 4) row = 0; // Wrap around if invalid row
                
                /* Send Set DDRAM Address command with calculated offset */
                lcd_command(lcd, LCD_CMD_SET_DDRAM_ADDR | (col + row_offsets[row]));
            }
            break;
            
        case LCD_BACKLIGHT:
            /* Toggle the backlight bit in the internal state */
            if (arg)
                lcd->backlight_state = LCD_BL_BIT;
            else
                lcd->backlight_state = 0;
            
            /* Update the I2C expander immediately to reflect the change */
            lcd_i2c_write_byte(lcd, lcd->backlight_state);
            break;

        case LCD_DISPLAY_SWITCH:
            /* Modify the Display ON/OFF bit in the control register */
            if (arg)
                lcd->display_ctrl |= LCD_DISPLAY_ON;
            else
                lcd->display_ctrl &= ~LCD_DISPLAY_ON;
            
            lcd_command(lcd, LCD_CMD_DISPLAY_CTRL | lcd->display_ctrl);
            break;

        case LCD_CURSOR_SWITCH:
            /* Modify the Cursor ON/OFF bit (underline) */
            if (arg)
                lcd->display_ctrl |= LCD_CURSOR_ON;
            else
                lcd->display_ctrl &= ~LCD_CURSOR_ON;
                
            lcd_command(lcd, LCD_CMD_DISPLAY_CTRL | lcd->display_ctrl);
            break;

        case LCD_BLINK_SWITCH:
            /* Modify the Blink ON/OFF bit (blinking block) */
            if (arg)
                lcd->display_ctrl |= LCD_BLINK_ON;
            else
                lcd->display_ctrl &= ~LCD_BLINK_ON;
                
            lcd_command(lcd, LCD_CMD_DISPLAY_CTRL | lcd->display_ctrl);
            break;

        case LCD_SCROLL:
            /* Shift the display window left or right.
             * Note: This moves the viewport, not the data in RAM. */
            if (arg == LCD_SCROLL_LEFT)
                lcd_command(lcd, LCD_CMD_SHIFT | LCD_DISPLAY_MOVE | LCD_MOVE_LEFT);
            else
                lcd_command(lcd, LCD_CMD_SHIFT | LCD_DISPLAY_MOVE | LCD_MOVE_RIGHT);
            break;

        case LCD_TEXT_DIR:
            /* Set text entry mode: Left-to-Right or Right-to-Left */
            if (arg == LCD_TEXT_LTR)
                lcd->display_mode |= LCD_ENTRY_LEFT;
            else
                lcd->display_mode &= ~LCD_ENTRY_LEFT;
                
            lcd_command(lcd, LCD_CMD_ENTRY_MODE | lcd->display_mode);
            break;

        case LCD_AUTOSCROLL:
            /* Set text entry mode: Auto-shift (autoscroll) enable/disable */
            if (arg)
                lcd->display_mode |= LCD_ENTRY_SHIFT_INC;
            else
                lcd->display_mode &= ~LCD_ENTRY_SHIFT_INC;
                
            lcd_command(lcd, LCD_CMD_ENTRY_MODE | lcd->display_mode);
            break;
            
        default:
            /* Should be caught by the _IOC_NR check above, but good for safety */
            return -ENOTTY;
    }
    return 0;
}

static const struct file_operations lcd_fops = {
    .owner = THIS_MODULE,
    .open = lcd_open,
    .release = lcd_release,
    .write = lcd_write,
    .unlocked_ioctl = lcd_ioctl,
};

/**
 * lcd_probe() - Initialize the device when the I2C client is detected
 * @client: The I2C client structure provided by the kernel
 * @id: The I2C device ID structure
 *
 * Return: 0 on success, negative error code on failure
 */
static int lcd_probe(struct i2c_client *client)
{
    int ret;
    struct lcd_dev *lcd;
    
    /* 0. Allocate memory for device state */
    lcd = kzalloc(sizeof(struct lcd_dev), GFP_KERNEL);
    if (!lcd)
        return -ENOMEM;
        
    lcd->client = client;
    lcd->backlight_state = LCD_BL_BIT; /* Default On */
    i2c_set_clientdata(client, lcd);
    
    /* Initialize LCD hardware */
    lcd_init_sequence(lcd);
    
    /* 1. Allocate a range of device numbers
     * int alloc_chrdev_region(dev_t *dev, unsigned baseminor, unsigned count, const char *name);
     * @param dev: Pointer to dev_t variable to store the first allocated device number
     * @param baseminor: First minor number to allocate (0 in our case)
     * @param count: Number of contiguous minor numbers to allocate (1 in our case)
     * @param name: Name of the device as it will appear in /proc/devices */
    ret = alloc_chrdev_region(&lcd->dev_num, 0, 1, DRIVER_NAME);
    if (ret < 0) {
        dev_err(&client->dev, "Failed to allocate chrdev region\n");
        goto err_free;
    }
    
    /* 2. Create the device class
     * struct class *class_create(const char *name);
     * @param name: The name of the class (appears in /sys/class/)
     * This creates the high-level class under which our device will appear. */
    lcd->class = class_create(LCD_CLASS_NAME);
    if (IS_ERR(lcd->class)) {
        dev_err(&client->dev, "Failed to create class\n");
        ret = PTR_ERR(lcd->class);
        goto err_unregister;
    }
    
    /* 3. Initialize the character device structure (cdev)
     * void cdev_init(struct cdev *cdev, const struct file_operations *fops);
     * @param cdev: The cdev structure to initialize
     * @param fops: The file_operations structure defining our read/write/ioctl callbacks */
    cdev_init(&lcd->cdev, &lcd_fops);

    /* 4. Add the character device to the system
     * int cdev_add(struct cdev *p, dev_t dev, unsigned count);
     * @param p: The initialized cdev structure
     * @param dev: The device number (major+minor)
     * @param count: Number of devices
     * After this call, the kernel knows to route /dev/aesdlcd operations to our functions. */
    ret = cdev_add(&lcd->cdev, lcd->dev_num, 1);
    if (ret < 0) {
        dev_err(&client->dev, "Failed to add cdev\n");
        goto err_destroy_class;
    }
    
    /* 5. Create the device node
     * struct device *device_create(struct class *class, struct device *parent, 
     * dev_t devt, void *drvdata, const char *fmt, ...);
     * @param class: The class the device belongs to
     * @param parent: The parent device (client->dev) for sysfs hierarchy
     * @param devt: The device number
     * @param drvdata: Driver private data (NULL here)
     * @param fmt: The name of the device node (appears in /dev/aesdlcd)
     * This triggers udev to create the actual /dev/aesdlcd file. */
    device_create(lcd->class, &client->dev, lcd->dev_num, NULL, "aesdlcd");
    
    dev_info(&client->dev, "AESD LCD driver probed at addr 0x%x\n", client->addr);
    return 0;

err_destroy_class:
    class_destroy(lcd->class);
err_unregister:
    unregister_chrdev_region(lcd->dev_num, 1);
err_free:
    kfree(lcd);
    return ret;
}

static void lcd_remove(struct i2c_client *client)
{
    struct lcd_dev *lcd = i2c_get_clientdata(client);
    
    /* 1. Destroy the device node 
     * void device_destroy(struct class *class, dev_t devt);
     * Removes /dev/aesdlcd */
    device_destroy(lcd->class, lcd->dev_num);

    /* 2. Delete the cdev
     * void cdev_del(struct cdev *p);
     * Removes the char device from the kernel system */
    cdev_del(&lcd->cdev);

    /* 3. Destroy the class
     * void class_destroy(struct class *cls);
     * Cleans up /sys/class entries */
    class_destroy(lcd->class);

    /* 4. Unregister the device numbers
     * void unregister_chrdev_region(dev_t from, unsigned count);
     * Returns the major/minor numbers to the kernel pool */
    unregister_chrdev_region(lcd->dev_num, 1);

    /* 5. Free memory */
    kfree(lcd);
}

/*
 * I2C Driver Registration & Startup Sequence
 * ----------------------------------------
 * * 1. DRIVER LOAD (insmod/modprobe): 
 * The kernel runs `module_i2c_driver()`. This registers `lcd_driver` with the 
 * I2C core, but DOES NOT create the character device yet.
 * * 2. MATCHING: 
 * The I2C core checks the Device Tree (DTS). If it finds a node compatible 
 * with "freenove,lcd" or "hitachi,hd44780", it triggers the probe.
 * * 3. PROBE (lcd_probe):
 * The kernel calls `lcd_probe()`. This function:
 * a. Allocates a Dynamic Major Number via `alloc_chrdev_region()`. 
 * -> This registers "aesdlcd_driver" in /proc/devices.
 * b. Creates the sysfs class.
 * c. Calls `device_create()`.
 * -> This triggers the kernel to request udev to create /dev/aesdlcd 
 * (usually with root:root 600 permissions).
 * * 4.LOAD SCRIPT (load_aesdlcd.sh):
 * Our shell script runs after insmod. It:
 * a. Greps /proc/devices for "aesdlcd_driver" to find the Major Number 
 * allocated in Step 3a.
 * b. Deletes the /dev/aesdlcd node (potentially created by udev in Step 3c).
 * c. Manually runs `mknod` to recreate the device with '666' permissions,
 * allowing non-root users to write to the display.
 */
static const struct i2c_device_id lcd_id[] = {
    { "lcd_i2c", 0 },
    { }
};

/* Exports the I2C device ID table to userspace so the module can be auto-loaded */
MODULE_DEVICE_TABLE(i2c, lcd_id);

/*
 * Device Tree Match Table
 * Used by the kernel to scan the device tree for a node with a "compatible"
 * property matching one of the strings listed here.
 * If a match is found, the probe function of this driver will be called.
 */
static const struct of_device_id lcd_of_match[] = {
    { .compatible = "freenove,lcd" },
    { .compatible = "hitachi,hd44780" },
    { }
};

/* Exports the OF match table for auto-loading based on Device Tree */
MODULE_DEVICE_TABLE(of, lcd_of_match);

/*
 * I2C Driver Structure
 * Registers the driver with the I2C subsystem.
 * - .driver.name: Name of the driver
 * - .driver.of_match_table: Pointer to the Device Tree match table
 * - .probe: Function called when a matching device is found
 * - .remove: Function called when the device is removed
 * - .id_table: Pointer to the I2C ID table
 */
static struct i2c_driver lcd_driver = {
    .driver = {
        .name = DRIVER_NAME,
        .of_match_table = lcd_of_match,
    },
    .probe = lcd_probe,
    .remove = lcd_remove,
    .id_table = lcd_id,
};

/* Macro that registers the I2C driver with the kernel */
module_i2c_driver(lcd_driver);