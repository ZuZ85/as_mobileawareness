// Copyright 2019 J Forristal LLC
// Copyright 2016 Addition Security Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//    http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef _CHECKHOOK_H_
#define _CHECKHOOK_H_

#include "ascti_tests.h"


#if defined(__i386__) || defined(__x86_64__)

#define CHECK_HOOK(sym,found,record)
#define REPORT_HOOKING(found,record)

#elif defined(__LP64__)

#define CHECK_HOOK(sym,found,record)	\
	do { \
		if(found == 0){ \
			uintptr_t x = (uintptr_t)sym; \
			uint32_t *a = (uint32_t*)(x & ~1); \
			if( a[1] == 0xd61f0200 && (a[0] == 0x58000050 || (a[0] & 0x0fffff0f) == 0x00ff8a00) ) { found++; record = ERRORBASE+__LINE__; } \
		} \
	} while(0)

#else

#define CHECK_HOOK(sym,found,record)	\
	do { \
		if(found == 0){ \
			uintptr_t x = (uintptr_t)sym; \
			uint32_t *a = (uint32_t*)(x & ~1); \
			if( (x & 1) == 0 ){  \
				if( a[0] == 0xe51ff004 	|| a[0] == 0xe7f001f0 ){ found++; record = ERRORBASE+__LINE__; } \
			} else { \
				uint16_t *b = (uint16_t*)(x & ~1); \
				if( ( b[0] & 0xff00 ) == 0xbe00 || (a[0] & 0xfff0ffff) == 0xf000f8df) { found++; record = ERRORBASE+__LINE__; } \
			} \
		} \
	} while(0)

#endif


// TODO: add analytics coalesce/cache?
#ifndef REPORT_HOOKING
#define REPORT_HOOKING(found,record) \
	if( found > 0 ){\
		ALOG("CI:WARN: internal hooking at %d", record); \
		ASCTI_Item_t item; \
		MEMSET(&item, 0, sizeof(item)); \
		item.test = CTI_TEST_APPLICATIONTAMPERINGDETECTED; \
		item.subtest = _SUBTEST_INTERNAL_HOOKCHECK; \
		item.data3_type = ASCTI_DT_VRID; \
		item.data3 = record; \
		message_add( &item ); \
	}
#endif


#endif
