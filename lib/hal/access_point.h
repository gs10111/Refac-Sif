#ifndef ACCESS_POINT_H
#define ACCESS_POINT_H

// The SoftAP that serves the firmware upload page.
//
// Both calls return a bool because the arming may only be spent once the operator
// can actually reach the page. Production ignores softAP()'s result and starts the
// web server after clearing the flag, so either failure spends the request and
// leaves the operator with nothing.
class IAccessPoint
{
public:
    virtual ~IAccessPoint() {}

    virtual bool start(const char *ssid, const char *password) = 0;
    virtual bool serveUpdatePage() = 0;
    virtual void stop() = 0;
};

#endif // ACCESS_POINT_H
