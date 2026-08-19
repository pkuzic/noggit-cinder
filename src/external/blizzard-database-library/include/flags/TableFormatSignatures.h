#pragma once

namespace BlizzardDatabaseLib {
    namespace Flag {

        struct TableFormatSignatures
        {
            static const unsigned int WDC5_FMT_SIGNATURE = 0x35434457;
            static const unsigned int WDC3_FMT_SIGNATURE = 0x33434457;
            static const unsigned int WDBC_FMT_SIGNATURE = 0x43424457;
        };
    }
}