#include <unistd.h>
#include <iostream>
#include "arrmq.h"
#include "errors.h"

ArrayMQ::ArrayMQ(uint32_t mq_size): 
    block_size(mq_size), head_addr(0), tail_addr(0)
{
    block_ptr = new char[mq_size];
    exit_if(block_ptr == NULL, "no enough space for mq");
    //determine endian of this machine
    short number = 0x0001;
    char *p = (char *)&number;
    endian_solution = *p == 0x00? BIG_ENDIAN_VALUE: LITTLE_ENDIAN_VALUE;
}

ArrayMQ::~ArrayMQ()
{
    delete[] block_ptr;
}

int ArrayMQ::enqueue(const void *data, unsigned data_len)
{
    unsigned head = head_addr, tail = tail_addr;
    unsigned free_len = head <= tail ? block_size - tail + head: head - tail;
    unsigned tail_2_end_len = block_size - tail;
    unsigned total_len = MSG_HEAD_LEN + data_len + BOUND_VALUE_LEN;
    unsigned new_tail_addr = 0;

    //Note: should leave 1 Byte to be a sentinel
    //if not, we can't know that head = tail is empty or full...
    //head = tail: empty
    //head = (tail + 1) % size: full
    if (total_len >= free_len)
        return QUEUE_ERR_FULL;
    //create message head
    char msg_head[MSG_HEAD_LEN] = {};
    unsigned* msg_head_ptr = (unsigned *)msg_head;
    *msg_head_ptr = BEGIN_BOUND_VALUE;
    uint64_t* send_ts_ptr = (uint64_t *)(msg_head + BOUND_VALUE_LEN);
    *send_ts_ptr = getCurrentMillis();//set current ts
    memcpy(msg_head + BOUND_VALUE_LEN + 8, &total_len, sizeof(unsigned));

    if (tail_2_end_len >= total_len)
    {
        memcpy(block_ptr + tail, msg_head, MSG_HEAD_LEN);
        memcpy(block_ptr + tail + MSG_HEAD_LEN, data, data_len);
        *((unsigned *)(block_ptr + tail + MSG_HEAD_LEN + data_len)) = END_BOUND_VALUE;
        new_tail_addr = tail + total_len;
    }
    //tail_2_end_len < total_len
    else if (tail_2_end_len >= MSG_HEAD_LEN)
    {
        memcpy(block_ptr + tail, msg_head, MSG_HEAD_LEN);
        unsigned first_data_len = tail_2_end_len - MSG_HEAD_LEN;
        unsigned second_data_len = data_len + BOUND_VALUE_LEN - first_data_len;
        if (second_data_len >= BOUND_VALUE_LEN)
        {
            memcpy(block_ptr + tail + MSG_HEAD_LEN, data, first_data_len);
            memcpy(block_ptr, (char *)data + first_data_len, data_len - first_data_len);
            *((unsigned *)(block_ptr + data_len - first_data_len)) = END_BOUND_VALUE;
        }
        else
        {
            memcpy(block_ptr + tail + MSG_HEAD_LEN, data, data_len);
            if (endian_solution == LITTLE_ENDIAN_VALUE)
            {
                switch (second_data_len)
                {
                    case 1:
                        *(block_ptr + block_size - 3) = '=';
                        *(block_ptr + block_size - 2) = 'N';
                        *(block_ptr + block_size - 1) = 'D';
                        *block_ptr = '$';
                        break;
                    case 2:
                        *(block_ptr + block_size - 2) = '=';
                        *(block_ptr + block_size - 1) = 'N';
                        *block_ptr = 'D';
                        *(block_ptr + 1) = '$';
                        break;
                    case 3:
                        *(block_ptr + block_size - 1) = '=';
                        *block_ptr = 'N';
                        *(block_ptr + 1) = 'D';
                        *(block_ptr + 2) = '$';
                }
            }
            else
            {
                switch (second_data_len)
                {
                    case 1:
                        *(block_ptr + block_size - 3) = '$';
                        *(block_ptr + block_size - 2) = 'D';
                        *(block_ptr + block_size - 1) = 'N';
                        *block_ptr = '=';
                        break;
                    case 2:
                        *(block_ptr + block_size - 2) = '$';
                        *(block_ptr + block_size - 1) = 'D';
                        *block_ptr = 'N';
                        *(block_ptr + 1) = '=';
                        break;
                    case 3:
                        *(block_ptr + block_size - 1) = '$';
                        *block_ptr = 'D';
                        *(block_ptr + 1) = 'N';
                        *(block_ptr + 2) = '=';
                }
            }
        }
        new_tail_addr = second_data_len;
    }
    //tail_2_end_len < MSG_HEAD_LEN
    else
    {
        memcpy(block_ptr + tail, msg_head, tail_2_end_len);
        unsigned leave_msg_head_len = MSG_HEAD_LEN - tail_2_end_len;
        memcpy(block_ptr, msg_head + tail_2_end_len, leave_msg_head_len);
        memcpy(block_ptr + leave_msg_head_len, data, data_len);
        *((unsigned *)(block_ptr + leave_msg_head_len + data_len)) = END_BOUND_VALUE;
        new_tail_addr = leave_msg_head_len + data_len + BOUND_VALUE_LEN;//tail
    }
    tail_addr = new_tail_addr;//update tail addr
    return QUEUE_SUCC;
}

int ArrayMQ::dequeue(void *buffer, unsigned buffer_size, unsigned &data_len, uint64_t &send_ts)
{
    unsigned head = head_addr, tail = tail_addr;
    if (head == tail)
    {
        data_len = 0;
        return QUEUE_ERR_EMPTY;
    }

    unsigned new_head_addr = 0;
    unsigned used_len = head < tail ? tail - head: block_size + tail - head;
    //copy msg_head out first
    char msg_head[MSG_HEAD_LEN] = {};
    //msg_head is in [head, ...)
    if (head + MSG_HEAD_LEN <= block_size)
    {
        memcpy(msg_head, block_ptr + head, MSG_HEAD_LEN);
        head += MSG_HEAD_LEN;
    }
    //msg_head is sub in [*block_ptr, tail) and sub in [head, ...)
    else
    {
        unsigned first_msg_head_len = block_size - head;
        unsigned second_msg_head_len = MSG_HEAD_LEN - first_msg_head_len;
        memcpy(msg_head, block_ptr + head, first_msg_head_len);
        memcpy(msg_head + first_msg_head_len, block_ptr, second_msg_head_len);
        head = second_msg_head_len;
    }
    //read message head info
    unsigned* msg_head_ptr = (unsigned *)msg_head;
    unsigned sentinel_head =  *msg_head_ptr;
    send_ts = *((uint64_t *)(msg_head + BOUND_VALUE_LEN));
    unsigned total_len = *((unsigned *)(msg_head + BOUND_VALUE_LEN + 8));
    //copy real data now
    if (sentinel_head != BEGIN_BOUND_VALUE)
        return QUEUE_ERR_CHECKSEN;

    if (total_len > used_len)
        return QUEUE_ERR_MEMESS;

    data_len = total_len - MSG_HEAD_LEN;
    if (data_len > buffer_size)
        return QUEUE_ERR_OTFBUFF;

    //data is in [head, ...)
    if (head + data_len <= block_size)
    {
        memcpy(buffer, block_ptr + head, data_len);
        new_head_addr = head + data_len;
    }
    //data is sub in [*block_ptr, tail) and sub in [head, ...)
    else
    {
        unsigned first_data_len = block_size - head;
        unsigned second_data_len = data_len - first_data_len;
        memcpy(buffer, block_ptr + head, first_data_len);
        memcpy((char *)buffer + first_data_len, block_ptr, second_data_len);
        new_head_addr = second_data_len;
    }
    data_len -= BOUND_VALUE_LEN;
    unsigned sentinel_tail = *((unsigned *)((char *)buffer + data_len));
    if (sentinel_tail != END_BOUND_VALUE)
        return QUEUE_ERR_CHECKSEN;

    head_addr = new_head_addr;//update head addr
    return QUEUE_SUCC;
}
