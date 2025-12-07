#include "scrThread.hpp"
#include "Memory.hpp"
#include "atArray.hpp"

namespace rage
{
    scrThread* scrThread::GetThread(uint32_t id)
    {
        static bool init = [] {
            if (auto addr = Memory::ScanPattern("48 8B 05 ? ? ? ? 48 89 34 F8 48 FF C7 48 39 FB 75 97"))
                m_Threads = addr->Add(3).Rip().As<decltype(m_Threads)>();

            return true;
        }();

        for (auto& thread : *m_Threads)
        {
            if (thread && thread->m_Context.m_ThreadId == id)
                return thread;
        }

        return nullptr;
    }
}