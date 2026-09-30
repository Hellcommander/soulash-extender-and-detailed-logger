#include <windows.h>

namespace
{
    int constructorCalls = 0;
    int sendCalls = 0;
    int destructorCalls = 0;
}

class __declspec(dllexport) MiniDmpSender
{
public:
    MiniDmpSender(
        const wchar_t*,
        const wchar_t*,
        const wchar_t*,
        const wchar_t*,
        unsigned long)
        : guardSize(0)
    {
        ++constructorCalls;
        SetLastError(0x5101);
    }

    virtual ~MiniDmpSender()
    {
        ++destructorCalls;
        SetLastError(0x5102);
    }

    void sendAdditionalFile(const wchar_t*)
    {
        ++sendCalls;
        SetLastError(0x5103);
    }

    int setGuardByteBufferSize(int value)
    {
        guardSize = value;
        SetLastError(0x5104);
        return value + 7;
    }

private:
    int guardSize;
};

extern "C" __declspec(dllexport) void WINAPI FakeGetCallCounts(int* constructors, int* sends, int* destructors)
{
    *constructors = constructorCalls;
    *sends = sendCalls;
    *destructors = destructorCalls;
}
