/*
   Copyright 2016 The Trustees of Princeton University

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

#include "syndicate-read.h"

#define BUF_LEN 1024000

// entry point 
int main( int argc, char** argv ) {
   
   int rc = 0;
   struct UG_state* ug = NULL;
   struct SG_gateway* gateway = NULL;
   char* path = NULL;
   int path_optind = 0;
   char buf[BUF_LEN]; 
   ssize_t nr = 0;
   int close_rc = 0;
   UG_handle_t* fh = NULL;
   uint64_t offset = 0;
   uint64_t len = 0;
   char* tmp = NULL;
   uint64_t num_read = 0;
   char debug_buf[52];

   mode_t um = umask(0);
   umask( um );
   
   struct tool_opts opts;
   
   memset( &opts, 0, sizeof(tool_opts) );
   
   argc = parse_args( argc, argv, &opts );
   if( argc < 0 ) {
      
      usage( argv[0], "syndicate_file offset len [syndicate_file offset len..]" );
      md_common_usage();
      exit(1);
   }
   
   // setup...
   ug = UG_init( argc, argv, opts.anonymous );
   if( ug == NULL ) {
      
      SG_error("%s", "UG_init failed\n" );
      exit(1);
   }
   
   gateway = UG_state_gateway( ug );
   
   // get the path list...
   path_optind = SG_gateway_first_arg_optind( gateway );
   if( path_optind == argc ) {
      
      usage( argv[0], "syndicate_file syndicate_file offset len [syndicate_file offset len...]" );
      UG_shutdown( ug );
      exit(1);
   }

   // sanity check 
   if( (argc - path_optind) % 3 != 0 ) {

      usage( argv[0], "syndicate_file syndicate_file offset len [syndicate_file offset len...]");
      UG_shutdown( ug );
      exit(1);
   }

   for( int i = path_optind; i < argc; i+=3 ) {

      path = argv[i];
      offset = (uint64_t)strtoull( argv[i+1], &tmp, 10 );
      if( offset == 0 && *tmp != '\0' ) {
         fprintf(stderr, "Failed to parse offset (argument %d)\n", i+1 );
         UG_shutdown(ug);
         exit(1);
      }

      len = (uint64_t)strtoull( argv[i+2], &tmp, 10 );
      if( len == 0 && *tmp != '\0' ) {
         fprintf(stderr, "Failed to parse len (argument %d)\n", i+2 );
         UG_shutdown(ug);
         exit(1);
      }

      SG_debug("Read: '%s' %" PRIu64 " %" PRIu64 "\n", path, offset, len );

      // try to open...
      fh = UG_open( ug, path, O_RDONLY, &rc );
      if( rc != 0 ) {

         fprintf(stderr, "Failed to open %s: %s\n", path, strerror(-rc));
         goto read_end;
      }

      // try to seek... 
      UG_seek( fh, offset, SEEK_SET );

      // try to read 
      nr = 0;
      num_read = 0;
      while( num_read < len ) {
          nr = UG_read( ug, buf, std::min(len,(uint64_t)BUF_LEN), fh );
          if( nr < 0 ) {
    
             fprintf(stderr, "%s: read: %s\n", path, strerror(-nr));
             rc = nr;
             break;
          }
          if( nr == 0 ) {

             // EOF
             SG_debug("EOF on %s\n", path ); 
             rc = 0;
             break;
          }

          memset(debug_buf, 0, 52);
          for( int i = 0; i < (50 / 3) && i < nr; i++ ) {
             char nbuf[5];
             memset(nbuf, 0, 5);
             snprintf(nbuf, 4, " %02X", buf[i]);
             strcat(debug_buf, nbuf);
          }
          
          SG_debug("Read %zd bytes (%s...)\n", nr, debug_buf );

          fwrite( buf, 1, nr, stdout );
          fflush( stdout );

          num_read += nr;
      }
      
      // close up 
      close_rc = UG_close( ug, fh );
      if( close_rc < 0 ) {

         fprintf(stderr, "%s: close: %s\n", path, strerror(-close_rc));
         break;
      }

      if( rc != 0 ) {
         break;
      }
   }

read_end:
   UG_shutdown( ug );

   if( rc != 0 ) {
      exit(1);
   }
   else {
      exit(0);
   }
}
