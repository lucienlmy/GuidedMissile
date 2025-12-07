#pragma once
#include "Vector.hpp"

namespace rage
{
    union scrValue;

    struct scrNativeCallContext
    {
        scrValue* m_ReturnValue;
        uint32_t m_ArgCount;
        scrValue* m_Args;
        int32_t m_NumVectorRefs;
        scrValue* m_VectorRefTargets[4];
        Vector3 m_VectorRefSources[4]; // not scrVector
    };
    static_assert(sizeof(scrNativeCallContext) == 0x80);

    using scrNativeHandler = void (*)(scrNativeCallContext*);
}