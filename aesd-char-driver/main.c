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
#include "aesd_ioctl.h"
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
 * @f_pos: Pointer to current file position (USED and updated to track read position).
 *         The kernel VFS passes &filp->f_pos as this parameter, not userspace.
 *         When userspace calls read(fd, buf, count), the VFS translates fd to filp
 *         and calls aesd_read(filp, buf, count, &filp->f_pos). Modifying *f_pos
 *         updates filp->f_pos, persisting across calls to support sequential reads.
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
 * @f_pos: Pointer to current file position (updated after write to reflect new position)
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
    
    /* Update file position to point to the end of all data in the buffer.
     * Although the write position is determined by the circular buffer's internal
     * logic (always appending), updating f_pos ensures proper cooperation with
     * llseek(). */
    *f_pos = aesd_get_total_size(dev);
    
    /* Write succeeded - return number of bytes accepted from userspace.
     * The kernel will use this return value to update userspace's write() return
     * and advance any internal tracking. We return count (not the total size) to
     * indicate how many bytes from the user buffer were successfully processed. */
    retval = count;
    mutex_unlock(&dev->lock);
    return retval;
}

/**
 * aesd_get_total_size() - Calculate total size of all data in circular buffer
 * @dev: Pointer to device structure
 *
 * Iterates through all entries in the circular buffer and sums their sizes.
 *
 * Caller must hold the device lock to ensure thread-safe access to the
 * circular buffer during iteration.
 *
 * Return: Total number of bytes stored in the circular buffer
 */
static size_t aesd_get_total_size(struct aesd_dev *dev)
{
    size_t total_size = 0;
    uint8_t index;
    struct aesd_circular_buffer *buffer = &dev->circular_buffer;
    
    /* Check if buffer is empty. When in_offs equals out_offs, the buffer
     * could be either completely empty or completely full. We use the full
     * flag to distinguish between these two cases. If not full and indices
     * are equal, there's no data in the buffer. */
    if (!buffer->full && buffer->in_offs == buffer->out_offs) {
        return 0;
    }
    
    /* Start from output index (oldest entry) and iterate through all valid
     * entries up to in_offs (where next write will go). This traverses the
     * logical order of the buffer regardless of how it wraps in the array. */
    index = buffer->out_offs;
    while (1) {
        /* Accumulate size of current entry. Each entry stores a complete
         * write command (terminated by newline) with its associated size. */
        total_size += buffer->entry[index].size;
        
        /* Advance to next entry with wraparound */
        index = (index + 1) % AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED;
        
        /* Stop when we reach in_offs. We've now counted all valid entries.
         * in_offs points to where the next write will be stored, so it's
         * not included in our count. */
        if (index == buffer->in_offs) {
            break;
        }
    }
    
    return total_size;
}

/**
 * aesd_llseek() - Reposition file offset for the device
 * @filp: Pointer to the kernel's struct file representing this open file INSTANCE.
 *        Important distinctions in the three-level hierarchy:
 *        
 *        1. struct inode - represents the DEVICE FILE (/dev/aesdchar) on the filesystem
 *           ONE inode exists regardless of how many times the device is opened
 *           
 *        2. struct file (this parameter) - represents ONE OPEN INSTANCE of the device
 *           Each open() call creates a NEW struct file with its own independent f_pos
 * @offset: Offset value for repositioning (can be positive or negative)
 * @whence: Starting position (SEEK_SET, SEEK_CUR, or SEEK_END)
 *
 * Implements custom seek functionality for the character device, allowing
 * userspace applications to reposition the file pointer within the circular
 * buffer's data. This enables random access to the buffered write commands
 * instead of only sequential access.
 *
 * Supports all three seek modes:
 * - SEEK_SET (0): Set position to offset bytes from the start (byte 0)
 * - SEEK_CUR (1): Set position to offset bytes from current position
 * - SEEK_END (2): Set position to offset bytes from the end of all data
 *
 * The circular buffer stores multiple write commands.
 * This function treats them as a contiguous stream of bytes numbered
 * from 0 to total_size-1.
 *
 * After a successful seek, subsequent read operations will start from the new position.
 * Write operations are not affected by file position - they always append.
 *
 * Return: New file position (>= 0) on success, negative error code on failure
 *         -ERESTARTSYS if interrupted by signal, -EINVAL for invalid whence or out of range position
 */
static loff_t aesd_llseek(struct file *filp, loff_t offset, int whence)
{
    struct aesd_dev *dev = filp->private_data;
    loff_t retval;
    size_t total_size;
    
    PDEBUG("llseek offset %lld whence %d", offset, whence);
    
    /* Acquire mutex lock to ensure thread-safe access to the circular buffer.*/
    if (mutex_lock_interruptible(&dev->lock)) {
        return -ERESTARTSYS;  /* Tell kernel to restart syscall after signal handled */
    }
    
    /* Calculate the total size of all data currently stored in the circular buffer.
     * This represents the sum of all entry sizes in logical order (out_offs to in_offs).
     * We pass this to fixed_size_llseek() which uses it as the maximum valid position.
     * The size is essentially a snapshot - it may change after we release the lock,
     * but for the duration of this seek operation, it defines the valid range. */
    total_size = aesd_get_total_size(dev);
    
    /* Use the kernel's fixed_size_llseek() helper function to perform the actual seek.
     * This function is specifically designed for devices with a fixed (or known) size.
     * It handles all the seek logic for us:
     * 
     * 1. Interprets whence (SEEK_SET, SEEK_CUR, SEEK_END) and calculates new position
     * 2. Validates the new position is within [0, total_size]
     * 3. Updates filp->f_pos if valid
     * 4. Returns the new position or error code
     */
    retval = fixed_size_llseek(filp, offset, whence, total_size);
    
    /* Release mutex - the seek operation is complete. fixed_size_llseek() has
     * already updated filp->f_pos if the seek was valid. Any subsequent operations
     * will use the new position.*/
    mutex_unlock(&dev->lock);
    
    /* Return the result from fixed_size_llseek(). On success, this is the new
     * absolute file position (>= 0). On failure, it's a negative error code */
    return retval;
}

/**
 * aesd_adjust_file_offset () - Adjust the file offset (f_pos) parameter of
 * @param filp based on the location specified by
 * @filp: File pointer for the open device instance
 * @write_cmd: Zero-referenced command index to locate (0 = oldest command in buffer)
 * @write_cmd_offset: Zero-referenced byte offset into the specified command
 *
 * The write_cmd parameter is relative to the current buffer contents:
 * - Command 0 always refers to the oldest command still in the buffer (at out_offs)
 * - Commands are numbered sequentially in write order
 *
 * Return: 0 if successful
 */
static long aesd_adjust_file_offset(struct file *filp, unsigned int write_cmd, unsigned int write_cmd_offset)
{
    struct aesd_dev *dev;
    struct aesd_circular_buffer *buffer;
    uint8_t count;
    uint8_t current_index;
    uint8_t index;
    size_t cumulative_offset = 0;
    
    /* Retrieve device structure from file's private data.
     * This was set during open() and provides access to our device state. */
    dev = filp->private_data;
    
    /* Acquire mutex lock */
    if (mutex_lock_interruptible(&dev->lock)) {
        return -ERESTARTSYS;  /* Interrupted by signal - kernel will retry */
    }
    
    buffer = &dev->circular_buffer;
    
    /* Check if buffer is empty */
    if (!buffer->full && buffer->in_offs == buffer->out_offs) {
        mutex_unlock(&dev->lock);
        return -EINVAL;  /* Can't seek in empty buffer */
    }
    
    /* Calculate the number of valid entries currently in the circular buffer.
     * This determines the valid range for write_cmd parameter [0, count-1].
     * 
     * Three cases to consider:
     * 1. Buffer is full: All slots are occupied, so count = max capacity
     * 2. in_offs >= out_offs: Buffer hasn't wrapped yet, count = in_offs - out_offs
     * 3. in_offs < out_offs: Buffer has wrapped around */
    if (buffer->full) {
        count = AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED;
    } else if (buffer->in_offs >= buffer->out_offs) {
        count = buffer->in_offs - buffer->out_offs;
    } else {
        /* Wrapped case: entries from out_offs to end, plus entries from 0 to in_offs */
        count = AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED - buffer->out_offs + buffer->in_offs;
    }
    
    /* Validate that write_cmd is within the valid range of available commands.
     * write_cmd is zero-based, so valid range is [0, count-1]. */
    if (write_cmd >= count) {
        mutex_unlock(&dev->lock);
        return -EINVAL;
    }
    
    /* Calculate the actual array index in the circular buffer for the requested
     * command wrapping with modulo. */
    current_index = (buffer->out_offs + write_cmd) % AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED;
    
    /* Validate that write_cmd_offset is within the size of the specified command. */
    if (write_cmd_offset >= buffer->entry[current_index].size) {
        mutex_unlock(&dev->lock);
        return -EINVAL;
    }
    
    /* Calculate the cumulative byte offset from the start of the buffer to the
     * beginning of the target command iterating from out_offs to current_index. */
    index = buffer->out_offs;
    while (index != current_index) {
        cumulative_offset += buffer->entry[index].size;
        index = (index + 1) % AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED;
    }
    
    /* Add the byte offset within the target command */
    cumulative_offset += write_cmd_offset;
    
    /* Update the file structure's position field. This affects subsequent read
     * operations on this file descriptor - they'll start reading from this new
     * position. Each open file descriptor maintains its own f_pos, so different
     * processes can seek to different positions independently. */
    filp->f_pos = cumulative_offset;
    
    /* Release mutex */
    mutex_unlock(&dev->lock);
    return 0;
}

/**
 * aesd_ioctl() - Handle ioctl commands for the device
 * @filp: File pointer for the open device instance
 * @cmd: ioctl command number (encoded with direction, size, type, and number)
 * @arg: Argument passed from user space (typically a pointer to a structure)
 * 
 * Currently supports:
 * - AESDCHAR_IOCSEEKTO: Seek to a specific write command and byte offset within it
 *
 * The AESDCHAR_IOCSEEKTO command takes a struct aesd_seekto from user space:
 *
 *   struct aesd_seekto {
 *       uint32_t write_cmd;        // Zero-based command index (0 = oldest command)
 *       uint32_t write_cmd_offset; // Zero-based byte offset within that command
 *   };
 *
 * Return: New file position (>= 0) on success, negative error code on failure
 */
static long aesd_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
    struct aesd_seekto seekto;
    long retval;
    
    PDEBUG("ioctl cmd %u", cmd);
    
    /* Verify that the ioctl command is valid for this driver.
     * The ioctl command number is encoded with several fields using _IOC() macro:
     * - Type (magic number): Identifies which driver this command belongs to
     * - Number: The specific command within this driver
     * - Direction: Whether data is read, written, or both
     * - Size: Size of the argument structure
     * 
     * _IOC_TYPE extracts the magic number - must match AESD_IOC_MAGIC (0x16).
     * _IOC_NR extracts the command number - must be <= AESDCHAR_IOC_MAXNR.
     * 
     * Returning -ENOTTY (inappropriate ioctl for device) is the standard way
     * to indicate "this ioctl command doesn't belong to this driver". */
    if (_IOC_TYPE(cmd) != AESD_IOC_MAGIC) return -ENOTTY;
    if (_IOC_NR(cmd) > AESDCHAR_IOC_MAXNR) return -ENOTTY;
    
    switch (cmd) {
        case AESDCHAR_IOCSEEKTO:
            /* Copy the aesd_seekto structure from userspace into kernel space.
             * arg is a userspace pointer to the structure. */
            if (copy_from_user(&seekto, (const void __user *)arg, sizeof(seekto)) != 0) {
                return -EFAULT;  /* Bad userspace address */
            }
            
            /* Use the helper function to adjust the file offset based on the
             * command and offset specified in the seekto structure. This helper
             * handles all the validation, calculation, and mutex locking. */
            retval = aesd_adjust_file_offset(filp, seekto.write_cmd, seekto.write_cmd_offset);
            
            if (retval != 0) {
                /* Helper function returned an error - pass it through to userspace */
                return retval;
            }
            
            /* Return the new file position to userspace. We read filp->f_pos
             * which was set by aesd_adjust_file_offset(). */
            return filp->f_pos;
            
        default:
            /* Unrecognized ioctl command number */
            return -ENOTTY;
    }
}

struct file_operations aesd_fops = {
    .owner =    THIS_MODULE,
    .read =     aesd_read,
    .write =    aesd_write,
    .open =     aesd_open,
    .release =  aesd_release,
    .llseek =   aesd_llseek,
    .unlocked_ioctl = aesd_ioctl
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
