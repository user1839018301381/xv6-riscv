#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "kernel/param.h"

// KernighanとRitchieによるメモリアロケータ。
// 『The C Programming Language』第2版、第8.7節。

typedef long Alignment;

union allocation_header {
    struct {
        union allocation_header *next;
        uint unit_count;
    } metadata;
    Alignment alignment;
};

typedef union allocation_header AllocationHeader;

static AllocationHeader free_list_anchor;
static AllocationHeader *free_list;

void free(void *address)
{
    AllocationHeader *freed_block, *current_block;

    freed_block = (AllocationHeader *)address - 1;
    for (current_block = free_list;
         !(freed_block > current_block &&
           freed_block < current_block->metadata.next);
         current_block = current_block->metadata.next)
        if (current_block >= current_block->metadata.next &&
            (freed_block > current_block ||
             freed_block < current_block->metadata.next))
            break;
    if (freed_block + freed_block->metadata.unit_count ==
        current_block->metadata.next) {
        freed_block->metadata.unit_count +=
            current_block->metadata.next->metadata.unit_count;
        freed_block->metadata.next =
            current_block->metadata.next->metadata.next;
    } else
        freed_block->metadata.next = current_block->metadata.next;
    if (current_block + current_block->metadata.unit_count == freed_block) {
        current_block->metadata.unit_count += freed_block->metadata.unit_count;
        current_block->metadata.next = freed_block->metadata.next;
    } else
        current_block->metadata.next = freed_block;
    free_list = current_block;
}

static AllocationHeader *grow_heap(uint requested_units)
{
    char *allocated_memory;
    AllocationHeader *new_block;

    if (requested_units < 4096)
        requested_units = 4096;
    allocated_memory = sbrk(requested_units * sizeof(AllocationHeader));
    if (allocated_memory == SBRK_ERROR)
        return 0;
    new_block = (AllocationHeader *)allocated_memory;
    new_block->metadata.unit_count = requested_units;
    free((void *)(new_block + 1));
    return free_list;
}

void *malloc(uint byte_count)
{
    AllocationHeader *block, *previous_block;
    uint required_units;

    required_units =
        (byte_count + sizeof(AllocationHeader) - 1) /
            sizeof(AllocationHeader) +
        1;
    if ((previous_block = free_list) == 0) {
        free_list_anchor.metadata.next = free_list = previous_block =
            &free_list_anchor;
        free_list_anchor.metadata.unit_count = 0;
    }
    for (block = previous_block->metadata.next;;
         previous_block = block, block = block->metadata.next) {
        if (block->metadata.unit_count >= required_units) {
            if (block->metadata.unit_count == required_units)
                previous_block->metadata.next = block->metadata.next;
            else {
                block->metadata.unit_count -= required_units;
                block += block->metadata.unit_count;
                block->metadata.unit_count = required_units;
            }
            free_list = previous_block;
            return (void *)(block + 1);
        }
        if (block == free_list)
            if ((block = grow_heap(required_units)) == 0)
                return 0;
    }
}
