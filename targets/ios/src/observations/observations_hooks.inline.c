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

#ifndef _OB_HOOKS_INLINE_C_
#define _OB_HOOKS_INLINE_C_

#include "as_ma_private.h"
#include "ascti_tests.h"
#include "as_defs_flags.h"
#include "as_mobileawareness.h"

#include <CoreFoundation/CoreFoundation.h>
#include <objc/runtime.h>
#include <objc/message.h>

#include <sys/types.h>
#include <unistd.h>
#include <dlfcn.h>

#include "tf_cal.h"

#include PLATFORM_H

#define ERRORBASE_OBH 58000

#define H_CACHE_SIZE 8
static uint64_t _h_cache[H_CACHE_SIZE] = {0};
static uint8_t  _h_cache_ptr = 0;

//
// RESEARCH NOTE: We got Substrate values from actually dumping hooked functions; it also aligns
// with old open-source copies of substrate.
//
// We got Frida values through live testing.
//

extern void* hook_cache[HOOK_CACHE_MAX];
extern int hook_cache_lock;

#define WORK_MAX_H	2
static const uint32_t PREFIX[] = {0x1e991ad,0x5a9821b6,}; // "ADDSEC"
#define _S(nom) _decode((sizeof(nom)/4)-1,nom,work)


__attribute__((always_inline))
static inline void observations_hooks(int do_fdc, int force_resolve)
{
	ASCTI_Item_t item;

	uint32_t work[WORK_MAX_H];
	_S(PREFIX);

	// First check the direct functions + ourselves
	uintptr_t x;
	uint32_t *a;
	uint16_t *b;

	// ARM64 note: the veneers may not get us into the right spot for any of these, so it's
	// somewhat best effort
	void *targets[] = { &dlsym, &dladdr, &AS_Initialize, &AS_Heartbeat, &AS_Security_Posture, 
		&bootstrap, &bootstrap_ex, &analytics_get_posture, &analytics_posture_contribution, 
		&heartbeat, &message_add, 
#if defined(__i386__) || defined(__x86_64__)
	NULL,
#else
	&syscall4_tfs,
#endif
		NULL };

	void **tp = &targets[0];
	while( *tp != NULL ){
		x = (uintptr_t)*tp;

		int hooked = 0;

// arm or arm64
		a = (uint32_t*)(x & ~1); // Remove thumb bit
		b = (uint16_t*)(x & ~1); // Remove thumb bit

#if defined(__LP64__)
		// arm64
		// TODO: a[1] == 0xd0fe6850
		if( a[1] == 0xd61f0200 && (a[0] == 0x58000050 || (a[0] & 0x0fffff0f) == 0x00ff8a00) ) // Frida
			hooked++;
#else
		// arm32
		if( (x & 1) == 0 ){  // ARM
			if( a[0] == 0xe51ff004 		// substrate & Frida ARM: ldr pc, [pc + 4]
				|| a[0] == 0xe7f001f0	// Frida ARM breakpoint
			)
				hooked++;
		} else { // Thumb
			if( ( b[0] & 0xff00 ) == 0xbe00  // Frida Thumb breakpoint 
				|| (a[0] & 0xfff0ffff) == 0xf000f8df 	// substrate & Frida Thumb2: ldr.w pc, [pc + 4]
			)
				hooked++;
		}

#endif // arm32 vs 64

		if( hooked > 0 ){
			ALOG("CI:TAG: direct %p is hooked", *tp);
			MEMSET(&item, 0, sizeof(item));
			item.test = CTI_TEST_APPLICATIONTAMPERINGDETECTED;
			if( *tp == &dlsym || *tp == &dladdr ) item.subtest = 51; // dyld hook
			else item.subtest = 52; // internal hook

			if( _CONFIG.flag_analytics_coalesce == 0 ||
				analytics_coalesce_check( _h_cache, H_CACHE_SIZE, 0, item.subtest ) == 0 )
			{
				message_add( &item );
				if( _CONFIG.flag_analytics_coalesce > 0 )
					analytics_coalesce_add( _h_cache, H_CACHE_SIZE, &_h_cache_ptr, 0, item.subtest );
			}
		}
		tp++;
	}

	// Init our strings
	uint16_t id=0;
	uint32_t resume=0, flags=0;
	uint8_t buffer[ 256 ];

	int res = TFDefs_String_Lookup( &_CONFIG.defs_as, ASDEFS_SECTION_HOOKS, buffer,
		(uint16_t)sizeof(buffer), &resume, &flags, &id );
	if( res != TFDEFS_FOUND ){
		ALOG("CI:ERR: unable to find hooks section in defs");
		// NOT-MVP-TODO: ATD internal integrity failure?
		error_report(ERRORBASE_OBH+__LINE__, res, 0);
		return;
	}
	uint16_t reqlen = (buffer[1] << 8) + buffer[0];
	ASSERT( reqlen < sizeof(buffer) );

	// Now check the other symbols
	int lookup_cache_idx = 0;
	while( 1 ){
		res = TFDefs_String_Lookup( &_CONFIG.defs_as, ASDEFS_SECTION_HOOKS, buffer,
			(uint16_t)sizeof(buffer), &resume, &flags, &id );
		if( res != TFDEFS_FOUND ) break;

		if( _CONFIG.flag_analytics_coalesce > 0 &&
			analytics_coalesce_check( _h_cache, H_CACHE_SIZE, flags, id ) == 1 ){
			lookup_cache_idx++;
			continue;
		}

		int hooked = 0;

		ASSERT( lookup_cache_idx < HOOK_CACHE_MAX );

		if( (flags & ASDEFS_FLAGS_SYM_NATIVE) == ASDEFS_FLAGS_SYM_NATIVE ){

			void *prior = NULL;

			while( !__sync_bool_compare_and_swap( &hook_cache_lock, 0, 1 ) ){}
			__sync_synchronize();
			prior = hook_cache[lookup_cache_idx];
			__sync_synchronize();
			hook_cache_lock = 0;

			if( prior == NULL || force_resolve > 0 ){
				x = (uintptr_t)dlsym(RTLD_DEFAULT, (char*)buffer);
				ALOG("CI:TAG: Hook resolving '%s' = %p", (char*)buffer, (void*)x);

				while( !__sync_bool_compare_and_swap( &hook_cache_lock, 0, 1 ) ){}
				__sync_synchronize();
				hook_cache[lookup_cache_idx] = (void*)x;
				__sync_synchronize();
				hook_cache_lock = 0;

			} else {
				x = (uintptr_t)prior;
			}

			if( x != (uintptr_t)prior ){
				// There was a change -- so let's confirm where it points

				Dl_info image_info;
				if( dladdr((void*)x, &image_info) == 0 ){
					ALOG("CI:WARN: dladdr failed on symbol %p", (void*)x);
					// TODO: this may happen on an anon memory region, i.e. hooking
					//error_report( ERRORBASE_OBH+__LINE__, 0, 0 );
					hooked++;
				} else {
					if( image_info.dli_fname == NULL || image_info.dli_fname[0] == 0 ){
						ALOG("CI:WARN: no image name for symbol");
						hooked++;
					} else {
						uint8_t hash[TCL_MD5_DIGEST_SIZE];
						TCL_MD5_2( (uint8_t*)work, 6, (uint8_t*)image_info.dli_fname,
							(uint32_t)STRLEN(image_info.dli_fname), hash );
	
						uint32_t flags2=0;
						uint16_t id2=0;
						int res = TFDefs_Hash_Lookup( &_CONFIG.defs_as, ASDEFS_SECTION_APPROVEDDYLIBS,
							(uint8_t*)hash, &flags2, &id2 );
						if( res != TFDEFS_FOUND ){
							ALOG("CI:TAG: non-approved dylib");
							ALOG("CI:TAG: sym %s lives in %s", (char*)buffer, image_info.dli_fname);

							// TODO: This is hooked, or it points to a new sys lib (false positive)
							//hooked++;

							if( _CONFIG.flag_fdc > 0 ){
			       		        		ASCTI_Item_t item;
								MEMSET( &item, 0, sizeof(item) );
								item.test = CTI_TEST_FDC;
			       		        		item.subtest = FDC_SYMBOL;
       		        					item.data1_type = ASCTI_DT_SYMBOL;
       		        					item.data1 = (char*)buffer;
       		        					item.data1_len = STRLEN((char*)buffer);
       		        					item.data2_type = ASCTI_DT_LIBRARY;
       		        					item.data2 = (void*)image_info.dli_fname;
       		        					item.data2_len = STRLEN(image_info.dli_fname);
								message_add( &item );
							}
						}
						else if( id2 != id ){
							// We found the hash, but the symbol doesn't point to an approved
							// subset.
							ALOG("CI:WARN: sym %s wants %d, got %d", (char*)buffer, id, id2);
							ALOG("CI:TAG: sym %s lives in %s", (char*)buffer, image_info.dli_fname);
							hooked++;
						}
					}
				}
			}

			if( x == 0 ){
				error_report(ERRORBASE_OBH+__LINE__, lookup_cache_idx, 0);
				ALOG("CI:ERR: Unable to locate '%s'", (char*)buffer);
				lookup_cache_idx++;
				continue;
			}

			if( prior != NULL && (uintptr_t)prior != x ){
				ALOG("CI:ERR: address for %s changed %p -> %p", (char*)buffer, (void*)x,
					hook_cache[lookup_cache_idx]);
				hooked++;
			}

			// arm or arm64
			a = (uint32_t*)(x & ~1); // Remove thumb bit
			b = (uint16_t*)(x & ~1); // Remove thumb bit

#if defined(__LP64__)
			// arm64
			// TODO: a[1] == 0xd0fe6850
			if( a[1] == 0xd61f0200 && (a[0] == 0x58000050 || (a[0] & 0x0fffff0f) == 0x00ff8a00) ) // Frida
				hooked++;
#else
			// arm32
			if( (x & 1) == 0 ){  // ARM
				if( a[0] == 0xe51ff004 		// substrate & Frida ARM: ldr pc, [pc + 4]
					|| a[0] == 0xe7f001f0	// Frida ARM breakpoint
				)
					hooked++;
			} else { // Thumb
				if( ( b[0] & 0xff00 ) == 0xbe00  // Frida Thumb breakpoint 
					|| (a[0] & 0xfff0ffff) == 0xf000f8df 	// substrate & Frida Thumb2: ldr.w pc, [pc + 4]
				)
					hooked++;
			}
#endif // arm32 vs 64


		}
		else if( (flags & ASDEFS_FLAGS_SYM_OBJC_CLASS) == ASDEFS_FLAGS_SYM_OBJC_CLASS )
		{
			void *prior = NULL;

			while( !__sync_bool_compare_and_swap( &hook_cache_lock, 0, 1 ) ){}
			__sync_synchronize();
			prior = hook_cache[lookup_cache_idx];
			__sync_synchronize();
			hook_cache_lock = 0;

			//ALOG("CI:TAG: Hook resolving '%s'", (char*)buffer);

			// Locate the class.method separator period
			uint8_t *sep = buffer;
			while( *sep != '.' && *sep != 0 ) sep++;
			ASSERT( *sep == '.' );

			sep[0] = 0;
			Class cl = objc_getClass((char*)buffer);
			SEL sel = sel_getUid((char*)&sep[1]);
			sep[0] = '.';

			if( cl != NULL && sel != NULL ){
				IMP imp = class_getMethodImplementation(cl, sel);
				if( force_resolve > 0 ){
					ALOG("CI:TAG: Hook resolving '%s' = %p", (char*)buffer, (void*)imp);
				}

				if( imp != prior ){
					while( !__sync_bool_compare_and_swap( &hook_cache_lock, 0, 1 ) ){}
					__sync_synchronize();
					hook_cache[lookup_cache_idx] = (void*)imp;
					__sync_synchronize();
					hook_cache_lock = 0;
				}

				if( prior != NULL && (void*)imp != prior ){
					ALOG("CI:ERR: address for %s changed %p -> %p", (char*)buffer, imp, hook_cache[lookup_cache_idx]);
					hooked++;
				}

				if( (void*)imp != prior ){

					// There was a change -- so let's confirm where it points
					Dl_info image_info;
					if( dladdr(imp, &image_info) == 0 ){
						ALOG("CI:WARN: dladdr failed on imp %p", imp);
						// TODO: this may happen on an anon memory region, i.e. hooking
						//error_report( ERRORBASE_OBH+__LINE__, 0, 0 );
						hooked++;
					} else {
						if( image_info.dli_fname == NULL || image_info.dli_fname[0] == 0 ){
							ALOG("CI:WARN: no image name for imp");
							hooked++;
						} else {
							uint8_t hash[TCL_MD5_DIGEST_SIZE];
							TCL_MD5_2( (uint8_t*)work, 6, (uint8_t*)image_info.dli_fname,
								(uint32_t)STRLEN(image_info.dli_fname), hash );
	
							uint32_t flags2=0;
							uint16_t id2=0;
							int res = TFDefs_Hash_Lookup( &_CONFIG.defs_as, ASDEFS_SECTION_APPROVEDDYLIBS,
								(uint8_t*)hash, &flags2, &id2 );
							if( res != TFDEFS_FOUND ){
								ALOG("CI:TAG: non-approved dylib");
								ALOG("CI:TAG: imp %s lives in %s", (char*)buffer, image_info.dli_fname);
								hooked++;

								// Field data collection, if warranted
								if( _CONFIG.flag_fdc > 0 ){
				       		        		ASCTI_Item_t item;
									MEMSET( &item, 0, sizeof(item) );
									item.test = CTI_TEST_FDC;
       		        						item.subtest = FDC_SYMLOC;
       		        						item.data1_type = ASCTI_DT_SYMBOL;
       		        						item.data1 = (char*)buffer;
       		        						item.data1_len = STRLEN((char*)buffer);
       		        						item.data2_type = ASCTI_DT_LIBRARY;
       		        						item.data2 = (void*)image_info.dli_fname;
       		        						item.data2_len = STRLEN(image_info.dli_fname);
									message_add( &item );
								}
							}
							else if( id2 != id ){
								// We found the hash, but the symbol doesn't point to an approved
								// subset.
								ALOG("CI:WARN: imp %s wants %d, got %d", (char*)buffer, id, id2);
								ALOG("CI:TAG: imp %s lives in %s", (char*)buffer, image_info.dli_fname);
								hooked++;
							}
						}
					}
				}

			} else {
				ALOG("CI:ERR: Unable to locate %s", (char*)buffer);
			}
		}

		if( hooked > 0 ){
			ALOG("CI:TAG: symbol %s is hooked", (char*)buffer);
			MEMSET(&item, 0, sizeof(item));
			item.test = CTI_TEST_APPLICATIONTAMPERINGDETECTED;
			item.subtest = 53;
			item.data1 = buffer;
			item.data1_len = STRLEN((char*)buffer);
			item.data1_type = ASCTI_DT_SYMBOL;
			message_add( &item );

			if( _CONFIG.flag_analytics_coalesce > 0 ){
				analytics_coalesce_add( _h_cache, H_CACHE_SIZE, 
					&_h_cache_ptr, flags, id );
			}
		}
		
		lookup_cache_idx++;
	} // while
}

#endif

