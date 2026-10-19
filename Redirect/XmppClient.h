#pragma once

namespace buzz
{
    class XmppClient
    {
    public:
        class Private
        {
        public:
            static bool Hook();

        private:
            static inline void* (*ChooseBestSaslMechanismOG)(Private* This, void* Result, const void* Mechanisms, bool bEncrypted) = nullptr;
            static void* ChooseBestSaslMechanismHK(Private* This, void* Result, const void* Mechanisms, bool bEncrypted);
        };
    };
}
