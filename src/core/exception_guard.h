#pragma once

namespace ExceptionGuard
{
    struct InstructionSkipperOptions
    {
        bool enabled;
        bool skipAllExceptions;
        bool allowExternalInstructions;
    };

    void Install();
    void Remove();

    InstructionSkipperOptions GetInstructionSkipperOptions();
    void SetInstructionSkipperOptions(const InstructionSkipperOptions& options);
}
