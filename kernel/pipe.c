#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "spinlock.h"
#include "proc.h"
#include "fs.h"
#include "sleeplock.h"
#include "file.h"

#define PIPESIZE 512

struct pipe {
    struct spinlock lock;
    char buffer[PIPESIZE];
    uint total_bytes_read;
    uint total_bytes_written;
    int is_read_open;
    int is_write_open;
};

int pipealloc(struct file **read_file_out, struct file **write_file_out)
{
    struct pipe *pipe;

    pipe = 0;
    *read_file_out = *write_file_out = 0;
    if ((*read_file_out = filealloc()) == 0 ||
        (*write_file_out = filealloc()) == 0)
        goto allocation_failed;
    if ((pipe = (struct pipe *)kalloc()) == 0)
        goto allocation_failed;
    pipe->is_read_open = 1;
    pipe->is_write_open = 1;
    pipe->total_bytes_written = 0;
    pipe->total_bytes_read = 0;
    initlock(&pipe->lock, "pipe");
    (*read_file_out)->type = FD_PIPE;
    (*read_file_out)->is_readable = 1;
    (*read_file_out)->is_writable = 0;
    (*read_file_out)->pipe = pipe;
    (*write_file_out)->type = FD_PIPE;
    (*write_file_out)->is_readable = 0;
    (*write_file_out)->is_writable = 1;
    (*write_file_out)->pipe = pipe;
    return 0;

allocation_failed:
    if (pipe)
        kfree((char *)pipe);
    if (*read_file_out)
        fileclose(*read_file_out);
    if (*write_file_out)
        fileclose(*write_file_out);
    return -1;
}

void pipeclose(struct pipe *pipe, int is_writable)
{
    acquire(&pipe->lock);
    if (is_writable) {
        pipe->is_write_open = 0;
        wakeup(&pipe->total_bytes_read);
    } else {
        pipe->is_read_open = 0;
        wakeup(&pipe->total_bytes_written);
    }
    if (pipe->is_read_open == 0 && pipe->is_write_open == 0) {
        release(&pipe->lock);
        kfree((char *)pipe);
    } else
        release(&pipe->lock);
}

int pipewrite(struct pipe *pipe, uint64 source_address, int byte_count)
{
    int bytes_written = 0;
    struct proc *process = myproc();

    acquire(&pipe->lock);
    while (bytes_written < byte_count) {
        if (pipe->is_read_open == 0 || is_killed(process)) {
            release(&pipe->lock);
            return -1;
        }
        if (pipe->total_bytes_written ==
            pipe->total_bytes_read + PIPESIZE) { //DOC: pipewrite-full
            wakeup(&pipe->total_bytes_read);
            sleep_prepare(&pipe->total_bytes_written);
            release(&pipe->lock);
            sleep();
            acquire(&pipe->lock);
        } else {
            char byte;
            if (copyin(process->pagetable, process->memory_size, &byte,
                       source_address + bytes_written, 1) == -1) {
                if (bytes_written == 0)
                    bytes_written = -1;
                break;
            }
            pipe->buffer[pipe->total_bytes_written++ % PIPESIZE] = byte;
            bytes_written++;
        }
    }
    wakeup(&pipe->total_bytes_read);
    release(&pipe->lock);

    return bytes_written;
}

int piperead(struct pipe *pipe, uint64 destination_address, int byte_count)
{
    int bytes_read;
    struct proc *process = myproc();
    char byte;

    acquire(&pipe->lock);
    while (pipe->total_bytes_read == pipe->total_bytes_written &&
           pipe->is_write_open) { //DOC: pipe-empty
        if (is_killed(process)) {
            release(&pipe->lock);
            return -1;
        }
        sleep_prepare(&pipe->total_bytes_read); //DOC: piperead-sleep
        release(&pipe->lock);
        sleep();
        acquire(&pipe->lock);
    }
    for (bytes_read = 0; bytes_read < byte_count; bytes_read++) { //DOC: piperead-copy
        if (pipe->total_bytes_read == pipe->total_bytes_written)
            break;
        byte = pipe->buffer[pipe->total_bytes_read % PIPESIZE];
        if (copyout(process->pagetable, process->memory_size,
                    destination_address + bytes_read, &byte, 1) == -1) {
            if (bytes_read == 0)
                bytes_read = -1;
            break;
        }
        pipe->total_bytes_read++;
    }
    wakeup(&pipe->total_bytes_written); //DOC: piperead-wakeup
    release(&pipe->lock);
    return bytes_read;
}
