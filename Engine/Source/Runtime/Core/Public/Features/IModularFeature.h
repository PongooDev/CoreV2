#pragma once

// Base class for modular features, usually interfaces (Epic's Features/IModularFeature.h): empty, so a feature
// interface puts its own vtable pointer first and this base after it. 21.40 PDB: IVoiceChatTokenFeature has
// __vftable at +0 and IModularFeature at +8, and the game turns the IModularFeature* the registry gives back into the
// interface by subtracting 8 (7.00 0xEE0731, "lea rsi, [rax-8]").
class IModularFeature
{
};
