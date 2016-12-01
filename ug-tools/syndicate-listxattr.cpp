/*
   Copyright 2015 The Trustees of Princeton University

   Licensed under the Apache License, Version 2.0 (the "License" );
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.
*/

#include "syndicate-stat.h"

// entry point 
int main( int argc, char** argv ) {
   
   int rc = 0;
   struct UG_state* ug = NULL;
   struct SG_gateway* gateway = NULL;
   char* path = NULL;
   int path_optind = 0;
   struct tool_opts opts;
   ssize_t sz = 0;
   ssize_t sz2 = 0;
   off_t offset = 0;
  
   uint64_t* times = NULL; 
   struct timespec ts_begin;
   struct timespec ts_end;
   char* buf = NULL;
   
   memset( &opts, 0, sizeof(tool_opts) );
   
   argc = parse_args( argc, argv, &opts );
   if( argc < 0 ) {
      
      usage( argv[0], "path [path...]" );
      md_common_usage();
      exit(1);
   }
   
   // setup...
   ug = UG_init( argc, argv );
   if( ug == NULL ) {
      
      SG_error("%s", "UG_init failed\n" );
      exit(1);
   }
   
   gateway = UG_state_gateway( ug );
   
   // get the directory path 
   path_optind = SG_gateway_first_arg_optind( gateway );
   if( path_optind == argc ) {
      
      usage( argv[0], "path [path...]" );
      md_common_usage();
      UG_shutdown( ug );
      exit(1);
   }
   
   if( opts.benchmark ) {
      times = SG_CALLOC( uint64_t, argc - path_optind + 1 );
      if( times == NULL ) {
          UG_shutdown( ug );
          SG_error("%s", "Out of memory\n");
          exit(1);
      }
   }

   for( int i = path_optind; i < argc; i++ ) {
            
        path = argv[ i ];

        // load up...
        clock_gettime( CLOCK_MONOTONIC, &ts_begin );

        while( true ) {
            rc = 0;
            sz = UG_listxattr( ug, path, NULL, 0 );
            if( sz < 0 ) {
                fprintf(stderr, "Failed to listxattr '%s': %s\n", path, strerror(abs(sz)) );
                rc = sz;
                break;
            }
            
            if( sz == 0 ) {
               break;
            } 

            buf = SG_CALLOC( char, sz + 2 );
            if( buf == NULL ) {
               UG_shutdown( ug );
               SG_error("%s", "Out of memory\n");
               exit(1);
            }

            sz2 = UG_listxattr( ug, path, buf, sz );
            if( sz2 > sz ) {
               SG_debug("Range expanded (from %zd to %zd)\n", sz, sz2); 
               sz2 = -ERANGE;
            }

            if( sz2 < 0 ) {
               if( sz2 == -ERANGE ) {
                  // expanded 
                  SG_debug("Range expanded (from %zd)\n", sz); 
                  SG_safe_free( buf );
                  buf = NULL;
                  continue;
               }

               fprintf(stderr, "Failed to listxattr '%s': %s\n", path, strerror(abs(sz)) );
               rc = sz;
               break;
            }

            break;
        }
        if( rc != 0 ) {
           rc = 1;
           break;
        }

        clock_gettime( CLOCK_MONOTONIC, &ts_end );

        if( buf != NULL ) {
            for( offset = 0; offset < sz2; ) {
                printf("%s\n", buf + offset);
                offset += strlen(buf + offset) + 1;
            }
        }

        SG_safe_free( buf );

        if( times != NULL ) {
            times[i - path_optind] = md_timespec_diff( &ts_end, &ts_begin );
        }
   }

   if( times != NULL ) {
    
      printf("@@@@@");
      for( int i = path_optind; i < argc - 1; i++ ) {
         printf("%" PRIu64 ",", times[i - path_optind] );
      }
      printf("%" PRIu64 "@@@@@\n", times[argc - 1 - path_optind] );

      SG_safe_free( times );
   }

   UG_shutdown( ug );
   exit(rc);
}
