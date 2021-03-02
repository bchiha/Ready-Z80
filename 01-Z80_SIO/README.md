# Z80 Serial Input / Outupt SIO

This is a basic terminal echo implementation using a Circular Buffer with serial data transmission using the Z80 SIO for the TEC computer.

There are three parts to this program:
1. A Producer - This is data coming from the SIO from an external source.
2. A Consumer - This is the TEC that consumes data on a key press or automatically
3. A Background Task - This is the TEC waiting for a non empty buffer to start a transmit

A Circular Buffer is a finite set of memory that has a pointer to the Head and a pointer to the end off the buffer.  A producer will place data at the head of the buffer, and a consumer will remove data at the tail of the buffer.  Once the head pointer reaches the end of the finite buffer, it will wrap around to the start of the buffer.  Likewise for the tail pointer.  If head pointer reaches the tail pointer, no more data is accepted and the producer will wait until a free spot is available.  If the tail pointer reaches the head pointer, the buffer is empty and the consumer waits.

An issue arises when the head and tail pointer are pointing to the same location.  Is the buffer empty or full?  To determine this, the following logic is applied.

If the head pointer = tail pointer, then the buffer is empty.
If the head pointer + 1 = tail pointer, then the buffer is full.

A simple process of bit masking on the pointers will reset them to the start of the buffer if they reach the end.  Pointer movement and buffer adding/taking is to be done while interrupts are disabled to ensure no data will be corrupted by another interrupt.

The producer with do the following:
 
 - Read the current value of the head pointer
 - If the head pointer + 1 equals the tail pointer, the buffer is full and raise an error
 - Otherwise, store the data in the head position and increase the head pointer

The consumer will do the following:
 - Read the current value of the tail pointer
 - If the tail pointer equals the head pointer the buffer is empty and exit
 - Otherwise, read the data in the tail position and increase the tail pointer