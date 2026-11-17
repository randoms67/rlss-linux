// sigsetjmp / siglongjmp for Android (bionic) arm64 code running on glibc.
//
// bionic's sigjmp_buf is 256 bytes. glibc's __sigsetjmp with savemask=1
// writes 312 bytes (registers + mask flag + a 128 byte sigset), which would
// overflow the app's buffer. So the registers are saved by glibc with
// savemask=0 (184 bytes) and the 8 byte kernel signal mask is kept by us in
// the unused tail of the bionic buffer:
//   [env + 192] savemask flag
//   [env + 200] saved signal mask (kernel sigset, 8 bytes)
// These are tail calls (b, not bl), so glibc records the app's own frame.
#if defined(__aarch64__)
    .text
    .globl apkl_sigsetjmp
    .type apkl_sigsetjmp, %function
apkl_sigsetjmp:
    mov     x9, x0                  // env
    str     x1, [x0, #192]          // remember savemask
    cbz     x1, 1f
    mov     x10, x1
    mov     x0, #0                  // SIG_BLOCK
    mov     x1, #0                  // set = NULL -> just read the mask
    add     x2, x9, #200            // oldset
    mov     x3, #8                  // sizeof(kernel sigset)
    mov     x8, #135                // __NR_rt_sigprocmask
    svc     #0
1:  mov     x0, x9
    mov     w1, #0
    b       __sigsetjmp
    .size apkl_sigsetjmp, .-apkl_sigsetjmp

    .globl apkl_siglongjmp
    .type apkl_siglongjmp, %function
apkl_siglongjmp:
    ldr     x9, [x0, #192]
    cbz     x9, 2f
    mov     x10, x0                 // env
    mov     x11, x1                 // val
    mov     x0, #2                  // SIG_SETMASK
    add     x1, x10, #200           // saved mask
    mov     x2, #0
    mov     x3, #8
    mov     x8, #135                // __NR_rt_sigprocmask
    svc     #0
    mov     x0, x10
    mov     x1, x11
2:  b       longjmp
    .size apkl_siglongjmp, .-apkl_siglongjmp

    .section .note.GNU-stack,"",%progbits
#endif
