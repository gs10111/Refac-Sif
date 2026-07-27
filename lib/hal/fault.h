#ifndef FAULT_H
#define FAULT_H

// An unrecoverable condition. On the device this prints to the serial line and
// spins forever, exactly as production does at main.cpp:61-62 — it does not
// return, and it deliberately does not reboot. A device that reboot-loops on a
// hardware fault is harder to diagnose in a plant than one sitting dead with a
// message on the wire.
//
// A test double cannot spin without hanging the suite, so it returns. That makes
// the code after a fatal() call reachable in tests on a path the device never
// takes — which is the only place the question "what does this do after a failure
// we called unrecoverable" can be answered at all. Keep that code safe.
class IFault
{
public:
    virtual ~IFault() {}
    virtual void fatal(const char *message) = 0;
};

#endif // FAULT_H
