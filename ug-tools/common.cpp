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

#include "common.h"

// print a single entry 
int print_entry( struct md_entry* dirent ) {
   
   char* entry_data = NULL;
   int rc = 0;
   
   rc = md_entry_to_string( dirent, &entry_data );
   if( rc != 0 ) {
      return rc;  
   }
   
   printf("%s\n", entry_data );
   free( entry_data );
   
   return 0;
}


// shift args down 
static int shift_args( int argc, char** argv, int index ) {

   for( int i = index; i < argc - 1; i++ ) {
      argv[i] = argv[i+1];
   }

   argv[argc-1] = NULL;
   return argc - 1;
}

// parse args for common tool options
// consume options that apply to the tool
// return the new argc
int parse_args( int argc, char** argv, struct tool_opts* opts ) {
    
   static struct option tool_options[] = {
      {"benchmark",       no_argument,   0, 'B'},
      {0, 0, 0, 0}
   };

   char const* optstr = "B";
   int c = 0;
   int opt_index = 0;
   
   // duplicate argv, since getopt_long reorders it
   char** argv_dup = SG_CALLOC( char*, argc + 1 );
   if( argv_dup == NULL ) {
       return -ENOMEM;
   }
   
   for( int i = 0; i < argc; i++ ) {
       argv_dup[i] = argv[i];
   }
   
   opterr = 0;
   
   while( c != -1 ) {
       
       c = getopt_long( argc, argv_dup, optstr, tool_options, &opt_index );
       if( c < 0 ) {
           break;
       }
       
       switch( c ) {
           
           case 'B': {
               opts->benchmark = true;
               argc = shift_args( argc, argv, optind - 1 );
               break;
           }

           default: {
               
               break;
           }
       }
   }
   
   SG_safe_free( argv_dup );
   
   optind = 0;
   opterr = 0;
   
   return argc;
}

// usage 
int usage( char const* progname, char const* args ) {
    
    printf("Usage: %s [syndicate arguments] [-B|--benchmark] %s\n", progname, args);
    return 0;
}
