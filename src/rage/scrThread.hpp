#pragma once

namespace rage
{
    template <typename T>
    class atArray;
    union scrValue;

    class scrThread
    {
    public:
        enum State : uint32_t
        {
            RUNNING,
            IDLE,
            KILLED,
            PAUSED
        };

        enum Priority : uint32_t
        {
            HIGHEST,
            NORMAL,
            LOWEST,
            MANUAL_UPDATE = 100
        };

        struct Context
        {
            uint32_t m_ThreadId;
            uint64_t m_Program;
            State m_State;
            uint32_t m_ProgramCounter;
            uint32_t m_FramePointer;
            uint32_t m_StackPointer;
            float m_TimerA;
            float m_TimerB;
            float m_WaitTimer;
            char m_Pad1[0x2C];
            uint32_t m_StackSize;
            uint32_t m_CatchProgramCounter;
            uint32_t m_CatchFramePointer;
            uint32_t m_CatchStackPointer;
            Priority m_Priority;
            uint8_t m_CallDepth;
            uint32_t m_CallStack[16];
        };
        static_assert(sizeof(Context) == 0xB0);

        virtual ~scrThread() = default;
        virtual void Reset(uint64_t program, void* args, uint32_t argCount) = 0;
        virtual State Run() = 0;
        virtual State Update() = 0;
        virtual void Kill() = 0;
        virtual void GetInfo(void* info) = 0;

        Context m_Context;
        scrValue* m_Stack;
        char m_Pad1[0x04];
        uint32_t m_ArgSize;
        uint32_t m_ArgLoc;
        char m_Pad2[0x04];
        char m_ErrorMessage[128];
        uint32_t m_ScriptHash;
        char m_ScriptName[64];

        static scrThread* GetThread(uint32_t id);

        static inline atArray<scrThread*>* m_Threads;
    };
    static_assert(sizeof(scrThread) == 0x198);
}