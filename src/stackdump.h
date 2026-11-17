#pragma once

namespace stackdump {
// APKL_STACKDUMP=<seconds>: periodically print the stacks of all threads (debug hangs)
void install();
// On SIGBUS/SIGSEGV/SIGILL/SIGFPE print fault address, its mapping and the stack,
// then run the previously installed handler
void installFaultReport();
}
