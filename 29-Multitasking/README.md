# Multitasking on the Z80

This "Proof of concept" program simulates task multitasking that can run up to eight separate tasks at the same time.  When the CPU triggers an interrupt, CPU registers are saved onto the task's stack, then the next task stack is swapped into the stack pointer.  The CPU registers are restored from the stack and the Program Counter is resumed to the new task.

This code does have limitation in particular the size of the task stacks and the number of tasks.  Also, the more tasks loaded, the slower each task goes.  The task swapper routine which is called when an interrupt happens is about 430 clock cycles.  Its the smallest I can get it.  

Interrupts are set at about 50Hz, but it can be lower.  A separate add-on board is required to be attached to the INT line which produces are square wave of about 50Hz or less.

This code is designed for the TEC-1G Z80 computer using Mon3 as the monitor.  The Task Swapper routine is inserted into address 0892H which will be called when an INT occurs.

