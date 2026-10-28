#pragma once

class FXmppConnectionJingle
{
public:
    static bool Hook();

private:
    static bool IsXmppThreadQuittingHK(void* XmppThread);
};
