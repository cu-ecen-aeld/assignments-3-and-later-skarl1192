/**
 * @file aesdchar.c
 * @brief Functions and data related to the AESD char driver implementation
 *
 * Based on the implementation of the "scull" device driver, found in
 * Linux Device Drivers example code.
 *
 * @author Dan Walkes
 * @date 2019-10-22
 * @copyright Copyright (c) 2019
 *
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/printk.h>
#include <linux/types.h>
#include <linux/cdev.h>
#include <linux/fs.h> // file_operations
#include <linux/slab.h>
#include <linux/uaccess.h> // copy_to_user, copy_from_user
#include <linux/mutex.h>
#include "aesdchar.h"
int aesd_major =   0; // use dynamic major
int aesd_minor =   0;

MODULE_AUTHOR("Scott Karl");
MODULE_LICENSE("Dual BSD/GPL");

struct aesd_dev aesd_device;

/**
 * aesd_open() - Device open operation handler
 * @inode: Pointer to inode structure representing the device file
 *         The inode (index node) represents the FILE ITSELF on the filesystem.
 *         It contains metadata about the file (permissions, owner, type, device numbers)
 *         and remains constant regardless of how many processes open the file.
 *         For our character device, inode->i_cdev points to our cdev structure.
 *         Think of it as: ONE inode per /dev/aesdchar file in the filesystem.
 *
 * @filp: Pointer to file structure representing the open file instance
 *        The file struct represents AN OPEN INSTANCE of the file - one per open() call.
 *        It tracks the current file position (f_pos), access mode (read/write), and
 *        other per-open state. Multiple opens create multiple file structs, all pointing
 *        to the same inode. We use filp->private_data to store our device pointer.
 *        Think of it as: MULTIPLE file structs (one per open) → ONE inode (the file).
 *
 * This function is invoked by the kernel whenever a process opens the /dev/aesdchar
 * device file. It is called for each open() system call, including multiple opens
 * by the same or different processes. The function retrieves the device-specific
 * structure and stores it in the file's private_data field for efficient access
 * during subsequent read/write operations. Multiple processes can open the device
 * simultaneously, each receiving their own file pointer but sharing the underlying
 * device structure protected by a mutex.
 */
int aesd_open(struct inode *inode, struct file *filp)
{
    struct aesd_dev *dev;
    
    PDEBUG("open");

    /* Get our device structure from the inode's cdev using container_of macro.
     * inode->i_cdev points to the cdev we registered in aesd_setup_cdev().
     * container_of() navigates from the cdev member back to the containing aesd_dev.
     * This is best practice even though aesd_device is global. */
    dev = container_of(inode->i_cdev, struct aesd_dev, cdev);
    
    /* Save device pointer in filp->private_data for quick access in read/write.
     * Each open file instance (filp) gets its own private_data, but they all
     * point to the same shared device structure (dev). */
    filp->private_data = dev;
    
    return 0;
}

/**
 * aesd_release() - Device close operation handler
 * @inode: Pointer to inode structure representing the device file
 * @filp: Pointer to file structure representing the open file instance
 *
 * This function is invoked by the kernel when the last reference to an open file
 * is closed. It is called when a process closes the file descriptor via close(),
 * or when a process terminates and the kernel closes all open descriptors. Note
 * that if a file descriptor is duplicated (via dup(), fork(), etc.), release() is
 * only called when all duplicates are closed. For this driver, no cleanup is
 * required as the device and its circular buffer persist across open/close cycles.
 */
int aesd_release(struct inode *inode, struct file *filp)
{
    PDEBUG("release");
    
    /* No cleanup needed for our driver - the device and its buffers
     * persist across open/close and are only freed during module unload */
    
    return 0;
}

/**
 * aesd_read() - Read data from the device
 * @filp: File pointer for the open device instance
 * @buf: User-space buffer to copy data into
 * @count: Maximum number of bytes to read
 * @f_pos: Pointer to current file position (USED and updated to track read position)
 *         Unlike write operations which ignore f_pos, read operations MUST honor it
 *         to support sequential reads and operations like cat, tail, etc.
 *
 * Reads data from the circular buffer containing the most recent 10 write commands.
 * The function uses the file position to determine which entry and offset within
 * that entry to read from, honoring the count parameter by returning at most count
 * bytes. After each successful read, f_pos is advanced by the number of bytes read,
 * allowing subsequent reads to continue from where the previous read left off.
 * Multiple reads may be required to retrieve all data. The function is thread-safe,
 * using a mutex to protect the circular buffer during read operations.
 *
 * The circular buffer stores write commands as complete entries (terminated by newline).
 * This function allows reading across multiple entries, treating them as a contiguous
 * stream of data. When the file position reaches the end of available data, the
 * function returns 0 to indicate EOF.
 *
 * Example: cat /dev/aesdchar will call read() repeatedly with increasing f_pos until
 * it receives 0 (EOF), allowing it to retrieve all buffered data sequentially.
 *
 * Return: Number of bytes read on success, 0 on EOF, -ERESTARTSYS if interrupted,
 *         -EFAULT if copy_to_user fails
 */
ssize_t aesd_read(struct file *filp, char __user *buf, size_t count,
                loff_t *f_pos)
{
    ssize_t retval = 0;
    struct aesd_dev *dev = filp->private_data;
    struct aesd_buffer_entry *entry;
    size_t entry_offset;
    size_t bytes_to_copy;
    
    PDEBUG("read %zu bytes with offset %lld",count,*f_pos);
    
    /* Acquire mutex lock for thread-safe access to circular buffer */
    if (mutex_lock_interruptible(&dev->lock)) {
        return -ERESTARTSYS;  /* Interrupted by signal - kernel will retry */
    }
    
    /* Find which circular buffer entry contains data at the current file position.
     * This function treats all entries as a contiguous stream of bytes and returns:
     * - entry: pointer to the buffer entry containing data at f_pos
     * - entry_offset: byte offset within that entry where f_pos points
     * Example: If entry[0] has 10 bytes and f_pos=15, this returns entry[1] with offset=5
     * Returns NULL if f_pos is beyond all available data. */
    entry = aesd_circular_buffer_find_entry_offset_for_fpos(&dev->circular_buffer, *f_pos, &entry_offset);
    
    if (entry == NULL) {
        /* No data available at current file position - we've read everything.
         * Returning 0 signals EOF to userspace (standard Unix convention).
         * Tools like cat recognize this and stop reading. */
        mutex_unlock(&dev->lock);
        return 0;
    }
    
    /* Calculate how many bytes we can copy from this entry.
     * We can only read up to the end of the current entry (entry->size - entry_offset).
     * If userspace requested more bytes (count) than available in this entry,
     * they'll need to call read() again to get the rest from the next entry.
     * This is how sequential reads work - one entry (or partial entry) at a time. */
    bytes_to_copy = entry->size - entry_offset;
    if (bytes_to_copy > count) {
        bytes_to_copy = count;  /* Honor userspace's requested limit */
    }
    
    /* Copy data from kernel space (entry->buffptr) to userspace buffer (buf).
     * copy_to_user() is required because kernel cannot directly write to userspace memory.
     * It performs necessary access checks and handles page faults if userspace buffer
     * is swapped out. Returns non-zero on failure (bad userspace address).
     * The offset (entry_offset) ensures we start reading from the correct position
     * within the entry, not always from the beginning. */
    if (copy_to_user(buf, entry->buffptr + entry_offset, bytes_to_copy)) {
        retval = -EFAULT;  /* Bad userspace address */
        mutex_unlock(&dev->lock);
        return retval;
    }
    
    /* Update file position to reflect bytes we just read.
     * Next read() call will start from this new position, allowing sequential reads.
     * Example: Read 10 bytes at f_pos=0 → f_pos becomes 10 → next read starts at 10. */
    *f_pos += bytes_to_copy;
    retval = bytes_to_copy;
    
    /* Release mutex and return number of bytes successfully read.
     * Userspace expects read() to return the actual number of bytes transferred,
     * which may be less than requested if we hit entry boundaries or EOF. */
    mutex_unlock(&dev->lock);
    return retval;
}

/**
 * aesd_write() - Write data to the device
 * @filp: File pointer for the open device instance
 * @buf: User-space buffer containing data to write
 * @count: Number of bytes to write from the buffer
 * @f_pos: Current file position (ignored - writes always append)
 *
 * Accepts write data from user space and stores it in a circular buffer after a
 * complete command (terminated by newline) is received. The function accumulates
 * partial writes in a temporary buffer until a newline character is encountered.
 * Once a newline is found, the complete command is added to the circular buffer
 * which maintains the most recent 10 commands.
 *
 * Write behavior:
 * - Data without '\n': temp in temp_buffer for future writes to complete
 * - Data with '\n': Complete command added to circular buffer, temp_buffer reset
 * - Circular buffer full: Oldest entry is automatically freed when new entry added
 * - Memory allocation: Uses kmalloc with GFP_KERNEL flag for each complete command
 *
 * The function is thread-safe, using a mutex to ensure atomic write operations.
 * Multiple processes can write simultaneously, but each write completes fully
 * before the next begins.
 *
 * Return: Number of bytes written on success, -ENOMEM on allocation failure,
 *         -ERESTARTSYS if interrupted, -EFAULT if copy_from_user fails
 */
ssize_t aesd_write(struct file *filp, const char __user *buf, size_t count,
                loff_t *f_pos)
{
    ssize_t retval = -ENOMEM;
    struct aesd_dev *dev = filp->private_data;
    char *new_buffer;
    size_t i;
    bool found_newline = false;
    
    PDEBUG("write %zu bytes with offset %lld",count,*f_pos);
    
    /* Acquire mutex lock to ensure thread-safe access to device structures.
     * mutex_lock_interruptible() allows the operation to be interrupted by signals,
     * which is important for userspace applications that may want to cancel I/O.
     * Returns non-zero if interrupted by a signal. */
    if (mutex_lock_interruptible(&dev->lock)) {
        return -ERESTARTSYS;  /* Tell kernel to restart syscall after signal handled */
    }
    
    /* Scan incoming data for newline character to determine if this write
     * completes a command. We need to check this BEFORE allocating memory
     * so we know whether to add the data to the circular buffer or not.
     * get_user() safely reads one byte from userspace memory. */
    for (i = 0; i < count; i++) {
        char c;
        if (get_user(c, buf + i)) {
            mutex_unlock(&dev->lock);
            return -EFAULT;  /* Bad address - userspace buffer was invalid */
        }
        if (c == '\n') {
            found_newline = true;
            break;  /* Found terminator, no need to scan further */
        }
    }
    
    /* Allocate kernel memory to hold temp data: previous partial writes
     * (temp_buffer_size) plus this new write (count). We can't write directly to
     * the circular buffer until we have a complete newline-terminated command.
     * GFP_KERNEL flag means: can sleep (OK since we're not in interrupt context),
     * and memory is allocated from normal kernel memory zone. */
    new_buffer = kmalloc(dev->temp_buffer_size + count, GFP_KERNEL);
    if (!new_buffer) {
        mutex_unlock(&dev->lock);
        return -ENOMEM;  /* Out of memory - kernel couldn't allocate */
    }
    
    /* Copy any previously temp partial data from temp_buffer to new_buffer.
     * This handles cases like: write("hel"), write("lo\n") → accumulates to "hello\n"
     * memcpy() is kernel version (similar to userspace memcpy). */
    if (dev->temp_buffer_size > 0) {
        memcpy(new_buffer, dev->temp_buffer, dev->temp_buffer_size);
    }
    
    /* Copy new data from userspace buffer to kernel buffer.
     * copy_from_user() is required because kernel can't directly access userspace memory.
     * It also handles cases where userspace memory becomes invalid during copy.
     * Appends new data after any existing data in new_buffer. */
    if (copy_from_user(new_buffer + dev->temp_buffer_size, buf, count)) {
        kfree(new_buffer);  /* Clean up allocated memory before returning error */
        mutex_unlock(&dev->lock);
        return -EFAULT;  /* Failed to read from userspace address */
    }
    
    /* Free the old temp_buffer since we've copied its contents to new_buffer */
    if (dev->temp_buffer) {
        kfree(dev->temp_buffer);
    }
    
    /* Update device state with the new temp buffer and its size.
     * At this point, new_buffer contains all temp data (old + new). */
    dev->temp_buffer = new_buffer;
    dev->temp_buffer_size += count;
    
    /* If we found a newline, we have a complete command ready to store.
     * Move the temp buffer into the circular buffer. */
    if (found_newline) {
        struct aesd_buffer_entry new_entry;
        const char *old_entry_ptr = NULL;
        
        /* Save pointer to old entry if buffer is full, BEFORE adding new entry.
         * aesd_circular_buffer_add_entry() handles all the circular buffer logic
         * (advancing in_offs, out_offs, managing full flag), but it does NOT manage
         * memory! Per the function's contract: "memory lifetime managed by the caller".
         * When buffer is full, in_offs points to the slot that will be overwritten. */
        if (dev->circular_buffer.full) {
            old_entry_ptr = dev->circular_buffer.entry[dev->circular_buffer.in_offs].buffptr;
        }
        
        /* Prepare new entry with pointer and size. The circular buffer stores these
         * values but doesn't own the memory - we're responsible for allocation/deallocation. */
        new_entry.buffptr = dev->temp_buffer;
        new_entry.size = dev->temp_buffer_size;
        
        /* Add to circular buffer */
        aesd_circular_buffer_add_entry(&dev->circular_buffer, &new_entry);
        
        /* Now free the old memory that was overwritten */
        if (old_entry_ptr) {
            kfree(old_entry_ptr);
        }
        
        /* Reset temp_buffer state since we've moved it into the circular buffer */
        dev->temp_buffer = NULL;
        dev->temp_buffer_size = 0;
    }
    
    /* Write succeeded - return number of bytes accepted from userspace. */
    retval = count;
    mutex_unlock(&dev->lock);
    return retval;
}

struct file_operations aesd_fops = {
    .owner =    THIS_MODULE,
    .read =     aesd_read,
    .write =    aesd_write,
    .open =     aesd_open,
    .release =  aesd_release,
};

static int aesd_setup_cdev(struct aesd_dev *dev)
{
    int err, devno = MKDEV(aesd_major, aesd_minor);

    cdev_init(&dev->cdev, &aesd_fops);
    dev->cdev.owner = THIS_MODULE;
    dev->cdev.ops = &aesd_fops;
    err = cdev_add (&dev->cdev, devno, 1);
    if (err) {
        printk(KERN_ERR "Error %d adding aesd cdev", err);
    }
    return err;
}



int aesd_init_module(void)
{
    dev_t dev = 0;
    int result;

    /* 1. Allocate a range of device numbers
     * int alloc_chrdev_region(dev_t * dev, unsigned baseminor, unsigned count, const char * name);
     * @param dev: Pointer to dev_t variable to store the first allocated device number
     * @param baseminor: First minor number to allocate (0 in our case)
     * @param count: Number of contiguous minor numbers to allocate (1 in our case)
     * @param name: Name of the device as it will appear in /proc/devices      * 
     */
    result = alloc_chrdev_region(&dev, aesd_minor, 1,
            "aesdchar");
    aesd_major = MAJOR(dev);
    if (result < 0) {
        printk(KERN_WARNING "Can't get major %d\n", aesd_major);
        return result;
    }
    
    /* 2. Initialize the members in the device structure */
    memset(&aesd_device,0,sizeof(struct aesd_dev));
    mutex_init(&aesd_device.lock);
    aesd_circular_buffer_init(&aesd_device.circular_buffer);
    aesd_device.temp_buffer = NULL;
    aesd_device.temp_buffer_size = 0;

    /* 3. Register the character device. 
     * aesd_setup_cdev() initializes and adds our cdev structure to the kernel,
     * connecting our device number (major, minor) to our file_operations (aesd_fops).
     * After this succeeds, the kernel knows to call our read/write/open/release
     * functions when userspace interacts with /dev/aesdchar. */
    result = aesd_setup_cdev(&aesd_device);

    /* 4. If aesd_setup_cdev failed, clean up everything we allocated */
    if( result ) {
        mutex_destroy(&aesd_device.lock);
        unregister_chrdev_region(dev, 1);
    }

    return result;
}

void aesd_cleanup_module(void)
{
    dev_t devno = MKDEV(aesd_major, aesd_minor);
    uint8_t index;
    struct aesd_buffer_entry *entry;

    /* Cleanup should happen in reverse order of initialization */

    /* 1. Remove the character device from the kernel */
    cdev_del(&aesd_device.cdev);

    /* 2. Free all dynamically allocated memory */
    AESD_CIRCULAR_BUFFER_FOREACH(entry, &aesd_device.circular_buffer, index) {
        if (entry->buffptr != NULL) {
            kfree(entry->buffptr);
        }
    }
    
    if (aesd_device.temp_buffer) {
        kfree(aesd_device.temp_buffer);
    }
    
    /* 3. Destroy mutex */
    mutex_destroy(&aesd_device.lock);
    
    /* 4. Unregister device numbers:
     *    - This releases the major/minor number(s) back to the kernel
     *    - Must be done last, after cdev_del(), to ensure no operations are in flight
     *    - The count (1) must match what was allocated in alloc_chrdev_region() */
    unregister_chrdev_region(devno, 1);
}



module_init(aesd_init_module);
module_exit(aesd_cleanup_module);
