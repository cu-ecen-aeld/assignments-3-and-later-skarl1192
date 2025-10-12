# Oops Analysis for the Faulty Module

## The Oops
I ran the command `echo "hello_world" > /dev/faulty` in the interactive qemu terminal. The following oops was printed to console.

```
[ 2571.279428] Unable to handle kernel NULL pointer dereference at virtual address 0000000000000000
[ 2571.279987] Mem abort info:
[ 2571.280109]   ESR = 0x0000000096000045
[ 2571.280251]   EC = 0x25: DABT (current EL), IL = 32 bits
[ 2571.280451]   SET = 0, FnV = 0
[ 2571.280587]   EA = 0, S1PTW = 0
[ 2571.280757]   FSC = 0x05: level 1 translation fault
[ 2571.281746] Data abort info:
[ 2571.281935]   ISV = 0, ISS = 0x00000045
[ 2571.282097]   CM = 0, WnR = 1
[ 2571.282341] user pgtable: 4k pages, 39-bit VAs, pgdp=0000000043c5a000
[ 2571.283300] [0000000000000000] pgd=0000000000000000, p4d=0000000000000000, pud=0000000000000000
[ 2571.284183] Internal error: Oops: 0000000096000045 [#1] PREEMPT SMP
[ 2571.284608] Modules linked in: scull(O) faulty(O) hello(O)
[ 2571.285274] CPU: 0 PID: 325 Comm: sh Tainted: G           O      5.15.186-yocto-standard #1
[ 2571.285765] Hardware name: linux,dummy-virt (DT)
[ 2571.286201] pstate: 80000005 (Nzcv daif -PAN -UAO -TCO -DIT -SSBS BTYPE=--)
[ 2571.286480] pc : faulty_write+0x18/0x20 [faulty]
[ 2571.334369] lr : vfs_write+0xf8/0x2a0
[ 2571.334559] sp : ffffffc009753d80
[ 2571.334677] x29: ffffffc009753d80 x28: ffffff8002063700 x27: 0000000000000000
[ 2571.335223] x26: 0000000000000000 x25: 0000000000000000 x24: 0000000000000000
[ 2571.335429] x23: 0000000000000000 x22: ffffffc009753dc0 x21: 0000005585827560
[ 2571.335599] x20: ffffff8003690a00 x19: 000000000000000c x18: 0000000000000000
[ 2571.335767] x17: 0000000000000000 x16: 0000000000000000 x15: 0000000000000000
[ 2571.335937] x14: 0000000000000000 x13: 0000000000000000 x12: 0000000000000000
[ 2571.336106] x11: 0000000000000000 x10: 0000000000000000 x9 : ffffffc008271ad8
[ 2571.336314] x8 : 0000000000000000 x7 : 0000000000000000 x6 : 0000000000000000
[ 2571.336492] x5 : 0000000000000001 x4 : ffffffc000b95000 x3 : ffffffc009753dc0
[ 2571.336665] x2 : 000000000000000c x1 : 0000000000000000 x0 : 0000000000000000
[ 2571.337525] Call trace:
[ 2571.337722]  faulty_write+0x18/0x20 [faulty]
[ 2571.337992]  ksys_write+0x74/0x110
[ 2571.338126]  __arm64_sys_write+0x24/0x30
[ 2571.338264]  invoke_syscall+0x5c/0x130
[ 2571.338417]  el0_svc_common.constprop.0+0x4c/0x100
[ 2571.338599]  do_el0_svc+0x4c/0xc0
[ 2571.338725]  el0_svc+0x28/0x80
[ 2571.338838]  el0t_64_sync_handler+0xa4/0x130
[ 2571.339083]  el0t_64_sync+0x1a0/0x1a4
[ 2571.339474] Code: d2800001 d2800000 d503233f d50323bf (b900003f) 
[ 2571.340200] ---[ end trace a580a9eee7473370 ]---
```

## Analysis of the Oops

### Summary & the First Line of the Oops

When the command `echo "hello_world" > /dev/faulty` was executed, the shell made a write system call that was routed to the `faulty` driver's write function. The `faulty` kernel module intentionally caused the oops by dereferencing a NULL pointer in its write function `*(int *)0 = 0;`.

The kernel's exception handler caught this illegal memory access, printed diagnostic information to printk (the oops), and terminated only the the shell (sent it a segmentation fault signal). 

We can see the NULL pointer dereference from the first line of the oops:

`Unable to handle kernel NULL pointer dereference at virtual address 0000000000000000`

### Rest of the Oops

The oops message provides additional detailed information which we review section-by-section below.

#### Memory Abort Information
```
Mem abort info:
  ESR = 0x0000000096000045
  EC = 0x25: DABT (current EL), IL = 32 bits
  WnR = 1
```

The Exception Syndrome Register (ESR) tells us this was a Data Abort (DABT) at the current exception level. The WnR = 1 bit indicates this was a write operation (not a read). The fault status code 0x05 indicates a level 1 translation fault, meaning the address was never mapped in the page tables.

The page table walk confirms the address was never mapped:
```
[0000000000000000] pgd=0000000000000000, p4d=0000000000000000, pud=0000000000000000
```
All page table levels show zero, confirming address 0x0 has no valid mapping.

#### System State
```
CPU: 0 PID: 325 Comm: sh Tainted: G           O      5.15.186-yocto-standard #1
```

The fault occurred on CPU 0 in process 325 (the sh shell). The kernel is tainted with 'O' indicating out-of-tree modules are loaded (scull, faulty, hello).

#### Program Counter and Faulting Instruction
```
pc : faulty_write+0x18/0x20 [faulty]
lr : vfs_write+0xf8/0x2a0
```

The program counter (pc) shows the fault occurred at offset 0x18 into the faulty_write function, which is only 0x20 (32) bytes total. The link register (lr) shows we were called from vfs_write in the VFS layer.

The actual faulting instruction is shown at the end:
```
Code: d2800001 d2800000 d503233f d50323bf (b900003f)
```
The instruction in parentheses (b900003f) is an ARM64 STR (store) instruction that attempted to write to the address in register x0.

#### Register Dump
```
x0 : 0000000000000000
x1 : 0000000000000000
x2 : 000000000000000c
```

The key register is x0 = 0x0, which was the target address for the write operation. This is the NULL pointer. Register x2 contains 0xc (12 decimal), which is the length of "hello_world\n" that we tried to write.

#### Call Trace
```
Call trace:
 faulty_write+0x18/0x20 [faulty]
 ksys_write+0x74/0x110
 __arm64_sys_write+0x24/0x30
 invoke_syscall+0x5c/0x130
```

The call trace shows the execution path from userspace to the fault:
1. User space: shell executed echo command
2. System call: write() syscall entered kernel via el0t_64_sync
3. Syscall handling: invoke_syscall dispatched to __arm64_sys_write
4. VFS layer: ksys_write called vfs_write
5. Driver: vfs_write called faulty_write where the fault occurred

#### Outcome

After the oops, the kernel sent a SIGSEGV (segmentation fault) signal to process 325 (the shell), terminating it. This oops demonstrates the kernel's robust error handling - even when a driver intentionally attempts illegal memory access, the system detects it, logs detailed information, kills only the offending process, and continues operating normally.
