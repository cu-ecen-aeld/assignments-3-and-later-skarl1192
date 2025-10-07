/**
 * @file aesd-circular-buffer.c
 * @brief Functions and data related to a circular buffer imlementation
 *
 * @author Dan Walkes
 * @date 2020-03-01
 * @copyright Copyright (c) 2020
 *
 */

#ifdef __KERNEL__
#include <linux/string.h>
#else
#include <string.h>
#endif

#include "aesd-circular-buffer.h"

/**
 * @param buffer the buffer to search for corresponding offset.  Any necessary locking must be performed by caller.
 * @param char_offset the position to search for in the buffer list, describing the zero referenced
 *      character index if all buffer strings were concatenated end to end
 * @param entry_offset_byte_rtn is a pointer specifying a location to store the byte of the returned aesd_buffer_entry
 *      buffptr member corresponding to char_offset.  This value is only set when a matching char_offset is found
 *      in aesd_buffer.
 * @return the struct aesd_buffer_entry structure representing the position described by char_offset, or
 * NULL if this position is not available in the buffer (not enough data is written).
 */
struct aesd_buffer_entry *aesd_circular_buffer_find_entry_offset_for_fpos(struct aesd_circular_buffer *buffer,
            size_t char_offset, size_t *entry_offset_byte_rtn )
{
    /**
    * TODO: implement per description
    */

    uint8_t currentIndex;
    size_t cumulativeOffset = 0;
    
    /* Check if buffer is empty. Must consider the full flag as well,
     * since in_offs == out_offs can mean the buffer is either full or empty */
    if (!buffer->full && buffer->in_offs == buffer->out_offs) {
        return NULL;
    }
    
    /* Start from output entry */
    currentIndex = buffer->out_offs;
    
    /* Iterate through all entries in the buffer */
    while (1) {
        struct aesd_buffer_entry *currentEntry = &buffer->entry[currentIndex];
        
        /* Check if this entry contains the requested offset */
        if (char_offset < cumulativeOffset + currentEntry->size) {
            /* Found the entry containing the requested offset */
            *entry_offset_byte_rtn = char_offset - cumulativeOffset;
            return currentEntry;
        }
        
        /* Move to the next entry */
        cumulativeOffset += currentEntry->size;
        currentIndex = (currentIndex + 1) % AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED;
        
        /* Stop when we've wrapped around to in_offs */
        if (currentIndex == buffer->in_offs) {
            break;
        }
    }
    
    /* char_offset is beyond the available data */
    return NULL;
}

/**
* Adds entry @param add_entry to @param buffer in the location specified in buffer->in_offs.
* If the buffer was already full, overwrites the oldest entry and advances buffer->out_offs to the
* new start location.
* Any necessary locking must be handled by the caller
* Any memory referenced in @param add_entry must be allocated by and/or must have a lifetime managed by the caller.
*/
void aesd_circular_buffer_add_entry(struct aesd_circular_buffer *buffer, const struct aesd_buffer_entry *add_entry)
{
    /**
    * TODO: implement per description
    */

    /* Since we are given the 'full' flag in the structure, we'll implement it
     * that way as stated in lecture (since we can implement without the `full` flag,
     * but would waste one index). When the buffer is full, we overwrite the oldest
     * entry (at out_offs) and advance out_offs.
     */
    
    /* Add the new entry at the in_offs position */
    buffer->entry[buffer->in_offs] = *add_entry;
    
    /* If the buffer was full, we're overwriting the oldest entry
     * so we need to advance out_offs to the next entry */
    if (buffer->full) {
        buffer->out_offs = (buffer->out_offs + 1) % AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED;
    }
    
    /* Advance in_offs to the next position */
    buffer->in_offs = (buffer->in_offs + 1) % AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED;
    
    /* If in_offs catches up to out_offs after advancing, the buffer is now full */
    if (buffer->in_offs == buffer->out_offs) {
        buffer->full = true;
    }
}

/**
* Initializes the circular buffer described by @param buffer to an empty struct
*/
void aesd_circular_buffer_init(struct aesd_circular_buffer *buffer)
{
    memset(buffer,0,sizeof(struct aesd_circular_buffer));
}
