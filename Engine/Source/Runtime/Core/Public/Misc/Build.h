#pragma once

#ifndef UE_BUILD_DEBUG
    #define UE_BUILD_DEBUG 0
#endif
#ifndef UE_BUILD_DEVELOPMENT
    #define UE_BUILD_DEVELOPMENT 0
#endif
#ifndef UE_BUILD_TEST
    #define UE_BUILD_TEST 0
#endif
#ifndef UE_BUILD_SHIPPING
    #define UE_BUILD_SHIPPING 0
#endif

#ifndef USE_LOGGING_IN_SHIPPING
    #define USE_LOGGING_IN_SHIPPING 0
#endif
#ifndef USE_CHECKS_IN_SHIPPING
    #define USE_CHECKS_IN_SHIPPING 0
#endif
#ifndef USE_MALLOC_PROFILER
    #define USE_MALLOC_PROFILER 0
#endif

#if UE_BUILD_DEBUG
    #define DO_GUARD_SLOW 1
    #define DO_CHECK 1
    #define STATS 1
    #define ALLOW_CONSOLE 1
    #define NO_LOGGING 0
#elif UE_BUILD_DEVELOPMENT
    #define DO_GUARD_SLOW 0
    #define DO_CHECK 1
    #define STATS 1
    #define ALLOW_CONSOLE 1
    #define NO_LOGGING 0
#elif UE_BUILD_TEST
    #define DO_GUARD_SLOW 0
    #define DO_CHECK USE_CHECKS_IN_SHIPPING
    #define STATS USE_MALLOC_PROFILER
    #define ALLOW_CONSOLE 1
    #define NO_LOGGING !USE_LOGGING_IN_SHIPPING
#elif UE_BUILD_SHIPPING
    #define DO_GUARD_SLOW 0
    #define DO_CHECK USE_CHECKS_IN_SHIPPING
    #define STATS 0
    #define ALLOW_CONSOLE 0
    #define NO_LOGGING !USE_LOGGING_IN_SHIPPING
#else
    #error Exactly one of [UE_BUILD_DEBUG UE_BUILD_DEVELOPMENT UE_BUILD_TEST UE_BUILD_SHIPPING] should be defined to be 1
#endif
