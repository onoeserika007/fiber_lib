/*
* Tencent is pleased to support the open source community by making Libco available.

* Copyright (C) 2014 THL A29 Limited, a Tencent company. All rights reserved.
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
*	http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing,
* software distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*/

.globl coctx_swap
#if !defined( __APPLE__ )
.type  coctx_swap, @function
#endif
coctx_swap:

#if defined(__i386__)
    movl 4(%esp), %eax
    movl %esp,  28(%eax)
    movl %ebp, 24(%eax)
    movl %esi, 20(%eax)
    movl %edi, 16(%eax)
    movl %edx, 12(%eax)
    movl %ecx, 8(%eax)
    movl %ebx, 4(%eax)


    movl 8(%esp), %eax
    movl 4(%eax), %ebx
    movl 8(%eax), %ecx
    movl 12(%eax), %edx
    movl 16(%eax), %edi
    movl 20(%eax), %esi
    movl 24(%eax), %ebp
    movl 28(%eax), %esp

	ret

#elif defined(__x86_64__)

    leaq (%rsp),%rax        # 1. 获取当前 RSP (栈指针)
    movq %rax, 104(%rdi)    # 2. 保存 RSP 到 curr->regs[kRSP] (104 = 13*8)
    movq %rbx, 96(%rdi)     # 3. 保存 RBX 到 curr->regs[kRBX] (96 = 12*8)
    movq %rcx, 88(%rdi)     # 4. 保存 RCX 到 curr->regs[kRCX] (88 = 11*8)
    movq %rdx, 80(%rdi)     # 5. 保存 RDX 到 curr->regs[kRDX] (80 = 10*8)
    movq 0(%rax), %rax      # 6. 读取返回地址 (RIP)
    movq %rax, 72(%rdi)     # 7. 保存 RIP 到 curr->regs[kRETAddr] (72 = 9*8)
    movq %rsi, 64(%rdi)     # 8. 保存 RSI 到 curr->regs[kRSI] (64 = 8*8)
    movq %rdi, 56(%rdi)     # 9. 保存 RDI 到 curr->regs[kRDI] (56 = 7*8)
    movq %rbp, 48(%rdi)     # 10. 保存 RBP 到 curr->regs[kRBP] (48 = 6*8)
    movq %r8, 40(%rdi)      # 11. 保存 R8 到 curr->regs[kR8] (40 = 5*8)
    movq %r9, 32(%rdi)      # 12. 保存 R9 到 curr->regs[kR9] (32 = 4*8)
    movq %r12, 24(%rdi)     # 13. 保存 R12 到 curr->regs[kR12] (24 = 3*8)
    movq %r13, 16(%rdi)     # 14. 保存 R13 到 curr->regs[kR13] (16 = 2*8)
    movq %r14, 8(%rdi)      # 15. 保存 R14 到 curr->regs[kR14] (8 = 1*8)
    movq %r15, (%rdi)       # 16. 保存 R15 到 curr->regs[kR15] (0 = 0*8)
    xorq %rax, %rax         # 17. 清零 RAX (设置返回值为 0)

    movq 48(%rsi), %rbp     # 1. 恢复 RBP 从 pending->regs[kRBP]
    movq 104(%rsi), %rsp    # 2. 恢复 RSP 从 pending->regs[kRSP]
    movq (%rsi), %r15       # 3. 恢复 R15 从 pending->regs[kR15]
    movq 8(%rsi), %r14      # 4. 恢复 R14 从 pending->regs[kR14]
    movq 16(%rsi), %r13     # 5. 恢复 R13 从 pending->regs[kR13]
    movq 24(%rsi), %r12     # 6. 恢复 R12 从 pending->regs[kR12]
    movq 32(%rsi), %r9      # 7. 恢复 R9 从 pending->regs[kR9]
    movq 40(%rsi), %r8      # 8. 恢复 R8 从 pending->regs[kR8]
    movq 56(%rsi), %rdi     # 9. 恢复 RDI 从 pending->regs[kRDI]
    movq 80(%rsi), %rdx     # 10. 恢复 RDX 从 pending->regs[kRDX]
    movq 88(%rsi), %rcx     # 11. 恢复 RCX 从 pending->regs[kRCX]
    movq 96(%rsi), %rbx     # 12. 恢复 RBX 从 pending->regs[kRBX]
    leaq 8(%rsp), %rsp      # 13. 跳过 coctx_swap 自己的返回地址
    pushq 72(%rsi)          # 14. 压入目标返回地址 (pending->regs[kRETAddr])
    movq 64(%rsi), %rsi     # 15. 恢复 RSI 从 pending->regs[kRSI]
    ret                     # 16. 执行 RET 指令，跳转到目标
#endif
