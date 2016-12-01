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

#include "syndicate-repl.h"

/*
 * The following is basically yoinked from libfskit/repl.c.
 *
 * It has to live here, and not in fskit, since some of the 
 * UG's control-plane information gets managed outside of fskit,
 * within the client interface.
 */

#include <fskit/fskit.h>
#include <fskit/util.h>

#define UG_REPL_ARGC_MAX 10 
#define UG_REPL_FILE_HANDLE_MAX 1024

// REPL statement
struct UG_repl_stmt {
   char* cmd;
   char* argv[UG_REPL_ARGC_MAX+1];
   int argc;

   char* linebuf;
};

// REPL state 
struct UG_repl {

   struct UG_state* ug;
   UG_handle_t* filedes[ UG_REPL_FILE_HANDLE_MAX ];
   UG_handle_t* dirdes[ UG_REPL_FILE_HANDLE_MAX ];
};


// get REPL statement's command
char const* UG_repl_stmt_command( struct UG_repl_stmt* stmt ) {
   return (char const*)stmt->cmd;
}

// get REPL statement args and count
char const** UG_repl_stmt_args( struct UG_repl_stmt* stmt, int* argc ) {
   *argc = stmt->argc;
   return (char const**)stmt->argv;
}

// free a REPL statement 
void UG_repl_stmt_free( struct UG_repl_stmt* stmt ) {
   SG_safe_free( stmt->linebuf );
   memset( stmt, 0, sizeof(struct UG_repl_stmt) );
   SG_safe_free( stmt );
}


// make a repl 
struct UG_repl* UG_repl_new( struct UG_state* ug ) {
   struct UG_repl* repl = SG_CALLOC( struct UG_repl, 1 );
   if( repl == NULL ) {
      return NULL;
   }

   repl->ug = ug;
   return repl;
}

// free a repl; closing all descriptors as well 
void UG_repl_free( struct UG_repl* repl ) {

   int rc = 0;
   for( int i = 0; i < UG_REPL_FILE_HANDLE_MAX; i++ ) {

      if( repl->filedes[i] == NULL ) {
         continue;
      }

      rc = UG_close( repl->ug, repl->filedes[i] );
      if( rc == 0 ) {
         repl->filedes[i] = NULL;
      }
      else {
         SG_error("close(%d) rc = %d\n", i, rc );
         repl->filedes[i] = NULL;
      }
   }

   for( int i = 0; i < UG_REPL_FILE_HANDLE_MAX; i++ ) {

      if( repl->dirdes[i] == NULL ) {
         continue;
      }

      rc = UG_closedir( repl->ug, repl->dirdes[i] );
      if( rc == 0 ) {
         repl->dirdes[i] = NULL;
      }
      else {
         SG_error("close(%d) rc = %d\n", i, rc );
         repl->dirdes[i] = NULL;
      }
   }

   SG_safe_free( repl );

   return;
}
          
 
// parse a REPL statement from a file stream.
// Note that we DO NOT ALLOW SPACES between arguments.
// return the new statement on success, and set *_rc to 0
// return NULL on failure, and set *_rc to:
//  -ENOMEM on OOM
//  -EINVAL on invalid line
//  -ENODATA on EOF
struct UG_repl_stmt* UG_repl_stmt_parse( FILE* input, int* _rc ) {

   ssize_t nr = 0;
   char* linebuf = NULL;
   size_t len = 0;
   char* tmp = NULL;
   char* arg = NULL;
   int i = 0;
   int last_arg_is_literal = 0;
   struct UG_repl_stmt* stmt = NULL;

   nr = getline( &linebuf, &len, input );
   if( nr < 0 ) {
      if( feof(input) ) {
         *_rc = -ENODATA;
         SG_debug("EOF on input %d\n", fileno(input));
      }
      else if( ferror(input) ) {
         *_rc = -EBADF;
         int errsv = -errno;
         errno = 0;
         clearerr(input);
         SG_debug("ferror on input %d (errno was %d)\n", fileno(input), errsv);
      }
      else {
         *_rc = -errno;
         SG_debug("I/O error on input %d: %d\n", fileno(input), *_rc);
      }
      return NULL;
   }

   if( len == 0 ) {
      SG_safe_free(linebuf);
      *_rc = 0;
      return NULL;
   }

   linebuf[nr-1] = '\0';
   if( strlen(linebuf) == 0 ) {
      SG_safe_free( linebuf );
      *_rc = 0;
      return NULL;
   }

   stmt = SG_CALLOC( struct UG_repl_stmt, 1 );
   if( stmt == NULL ) {
      SG_safe_free( linebuf );
      *_rc = -ENOMEM;
      return NULL;
   }

   stmt->linebuf = linebuf;
   stmt->cmd = strtok_r( linebuf, " \t", &tmp );

   if( stmt->cmd == NULL ) {
      // all whitespace
      UG_repl_stmt_free( stmt );
      *_rc = 0;
      return NULL;
   }

   // each argument... 
   // (but for setxattr and write, preserve the last argument as a literal)
   if( strcmp(stmt->cmd, "write") == 0 || strcmp(stmt->cmd, "setxattr") == 0 ) {
      last_arg_is_literal = 1;
   }

   for( i = 0; i < UG_REPL_ARGC_MAX - last_arg_is_literal; i++ ) {
      arg = strtok_r( NULL, " \t", &tmp );
      if( arg == NULL ) {
         break;
      }

      stmt->argv[i] = arg;
   }

   if( i == UG_REPL_ARGC_MAX - last_arg_is_literal ) {
      // no command has this many arguments
      UG_repl_stmt_free( stmt );
      *_rc = -EINVAL;
      return NULL;
   }
      
   if( last_arg_is_literal != 0 ) {
      stmt->argv[i] = strtok_r( NULL, "", &tmp );
   }

   stmt->argc = i;
   *_rc = 0;

   /*
   SG_debug("%s(", stmt->cmd);
   for( i = 0; i < stmt->argc - 1; i++ ) {
      fprintf(stderr, "%s, ", stmt->argv[i]);
   }
   if( stmt->argc > 0 ) {
       fprintf(stderr, stmt->argv[stmt->argc-1]);
   }
   fprintf(stderr, ")\n");
   fflush(stderr);
   */

   return stmt;
}


// insert a filedes 
// return the index (>= 0) on success
// return -ENFILE if we're out of space
static int UG_repl_filedes_insert( struct UG_repl* repl, UG_handle_t* fh ) {

   for( int i = 0; i < UG_REPL_FILE_HANDLE_MAX; i++ ) {
      if( repl->filedes[i] == NULL ) {
         repl->filedes[i] = fh;
         return i;
      }
   }

   return -ENFILE;
}


// insert a dirdes
// return the index (>= 0) on success
// return -ENFILE if we're out of space 
static int UG_repl_dirdes_insert( struct UG_repl* repl, UG_handle_t* dh ) {

   for( int i = 0; i < UG_REPL_FILE_HANDLE_MAX; i++ ) {
      if( repl->dirdes[i] == NULL ) {
         repl->dirdes[i] = dh;
         return i;
      }
   }

   return -ENFILE;
}


// clear a filedes by closing it
// return the result of close on success
// return -EBADF if there is no handle
static int UG_repl_filedes_close( struct UG_repl* repl, int fd ) {

   int rc = 0;
   if( fd < 0 || fd >= UG_REPL_FILE_HANDLE_MAX ) {
      return -EBADF;
   }

   if( repl->filedes[fd] == NULL ) {
      return -EBADF;
   }

   rc = UG_close( repl->ug, repl->filedes[fd] );
   if( rc == 0 ) {
      repl->filedes[fd] = NULL;
   }

   return rc;
}


// clear a dirdes by closing it 
// return the result of closedir on success 
// return -EBADF if there was no handle 
static int UG_repl_dirdes_close( struct UG_repl* repl, int dfd ) {

   int rc = 0;
   if( dfd < 0 || dfd >= UG_REPL_FILE_HANDLE_MAX ) {
      return -EBADF;
   }

   if( repl->dirdes[dfd] == NULL ) {
      return -EBADF;
   }

   rc = UG_closedir( repl->ug, repl->dirdes[dfd] );
   if( rc == 0 ) {
      repl->dirdes[dfd] = NULL;
   }

   return rc;
}


// look up a file descriptor 
// return it on success
// return NULL if not present 
static UG_handle_t* UG_repl_filedes_lookup( struct UG_repl* repl, int fd ) {
   
   if( fd < 0 || fd >= UG_REPL_FILE_HANDLE_MAX ) {
      return NULL;
   }

   return repl->filedes[fd];
}


// look up a directory descriptor 
// return it on success
// return NULL if not present 
static UG_handle_t* UG_repl_dirdes_lookup( struct UG_repl* repl, int dfd ) {

   if( dfd < 0 || dfd >= UG_REPL_FILE_HANDLE_MAX ) {
      return NULL;
   }

   return repl->dirdes[dfd];
}


// parse an unsigned int64 
// return 0 on success and set *ret
// return -EINVAL on failure 
static int UG_repl_stmt_parse_uint64( char* str, uint64_t* ret ) {

   char* tmp = NULL;
   *ret = (uint64_t)strtoull( str, &tmp, 10 );
   if( *ret == 0 && *tmp != '\0' ) {
      return -EINVAL;
   }

   return 0;
}


// get user and group IDs--these are always the first two arguments
// return 0 on success, and set *user_id and *group_id
// return -EINVAL if this is not possible 
static int UG_repl_stmt_parse_user_group( struct UG_repl_stmt* stmt, uint64_t* user, uint64_t* group, int offset ) {

   int rc = 0;

   if( stmt->argc < offset + 2 ) {
      return -EINVAL;
   }

   rc = UG_repl_stmt_parse_uint64( stmt->argv[offset], user );
   if( rc != 0 ) {
      return rc;
   }

   rc = UG_repl_stmt_parse_uint64( stmt->argv[offset + 1], group );
   if( rc != 0 ) {
      return rc;
   }

   return 0;
}


// parse mode as an octal string 
// return 0 on success, and set *mode 
// return -EINVAL otherwise 
static int UG_repl_stmt_parse_mode( char* modearg, mode_t* mode ) {

   char* tmp = NULL;
   *mode = strtoul( modearg, &tmp, 8 );
   if( *mode == 0 && *tmp != '\0' ) {
      return -EINVAL;
   }

   return 0;
}


// dispatch a REPL statement
// return the result of the command
// return -EINVAL for invalid commands
int UG_repl_stmt_dispatch( struct UG_repl* repl, struct UG_repl_stmt* stmt ) {

   int rc = 0;
   uint64_t user_id = 0;
   uint64_t group_id = 0;
   uint64_t new_user_id = 0;
   uint64_t filedes = 0;
   mode_t mode = 0;
   uint64_t flags = 0;
   char* path = NULL;
   char* newpath = NULL;
   char* attrname = NULL;
   char* attrvalue = NULL;
   char* buf = NULL;
   ssize_t attrlen = 0;
   uint64_t offset = 0;
   uint64_t len = 0;
   uint64_t size = 0;
   uint64_t num_children = 0;
   struct stat sb;
   uint64_t timetmp = 0;
   struct utimbuf ubuf;
   struct md_entry** children = NULL;
   UG_handle_t* fh = NULL;
   UG_handle_t* dh = NULL;
   struct UG_state* ug = repl->ug;
   char* full_cmd = NULL;
   char debug_buf[52];
   
   // sanity check... 
   if( stmt->cmd == NULL ) {
      return -EINVAL;
   }

   if( strcmp(stmt->cmd, "access") == 0 ) {

      // path mode
      if( stmt->argc != 2 ) {
         rc = -EINVAL;
         goto UG_repl_stmt_dispatch_out;
      }

      path = stmt->argv[0];
      rc = UG_repl_stmt_parse_mode( stmt->argv[1], &mode );
      if( rc != 0 ) {
         goto UG_repl_stmt_dispatch_out;
      }

      SG_debug("access('%s', %o)\n", path, mode );
      rc = UG_access( ug, path, mode );
      SG_debug("access('%s', %o) rc = %d\n", path, mode, rc );
   }
   else if( strcmp(stmt->cmd, "chmod") == 0 ) {

      // path mode
      if( stmt->argc != 2 ) {
         rc = -EINVAL;
         goto UG_repl_stmt_dispatch_out;
      }

      path = stmt->argv[0];
      rc = UG_repl_stmt_parse_mode( stmt->argv[1], &mode );
      if( rc != 0 ) {
         goto UG_repl_stmt_dispatch_out;
      }

      SG_debug("chmod('%s', %o)\n", path, mode );
      rc = UG_chmod( ug, path, mode );
      SG_debug("chmod('%s', %o) rc = %d\n", path, mode, rc );
   }
   else if( strcmp(stmt->cmd, "chown") == 0 ) {

      // path new_user
      if( stmt->argc != 2 ) {
         rc = -EINVAL;
         goto UG_repl_stmt_dispatch_out;
      }
      
      rc = UG_repl_stmt_parse_user_group( stmt, &user_id, &group_id, 0 );
      if( rc != 0 ) {
         goto UG_repl_stmt_dispatch_out;
      }

      path = stmt->argv[0];
      rc = UG_repl_stmt_parse_uint64( stmt->argv[1], &new_user_id );
      if( rc != 0 ) {
         goto UG_repl_stmt_dispatch_out;
      }

      SG_debug("chown('%s', %" PRIu64 ")\n", path, new_user_id );
      rc = UG_chown( ug, path, new_user_id );
      SG_debug("chown('%s', %" PRIu64 ") rc = %d\n", path, new_user_id, rc );
   }
   else if( strcmp(stmt->cmd, "close") == 0 ) {

      // filedes
      if( stmt->argc != 1 ) {
         rc = -EINVAL;
         goto UG_repl_stmt_dispatch_out;
      }
     
      rc = UG_repl_stmt_parse_uint64( stmt->argv[0], &filedes );
      if( rc != 0 ) {
         goto UG_repl_stmt_dispatch_out;
      }

      SG_debug("close(%d)\n", (int)filedes );
      rc = UG_repl_filedes_close( repl, filedes );
      SG_debug("close(%d) rc = %d\n", (int)filedes, rc );
   }
   else if( strcmp(stmt->cmd, "closedir") == 0 ) {

      // dirdes 
      if( stmt->argc != 1 ) {
         rc = -EINVAL;
         goto UG_repl_stmt_dispatch_out;
      }

      rc = UG_repl_stmt_parse_uint64( stmt->argv[0], &filedes );
      if( rc != 0 ) {
         goto UG_repl_stmt_dispatch_out;
      }

      SG_debug("closedir(%d)\n", (int)filedes );
      rc = UG_repl_dirdes_close( repl, filedes );
      SG_debug("closedir(%d) rc = %d\n", (int)filedes, rc );
   }
   else if( strcmp(stmt->cmd, "create") == 0 ) {

      // path mode
      if( stmt->argc != 2 ) {
         rc = -EINVAL;
         SG_error("argc = %d\n", stmt->argc);
         goto UG_repl_stmt_dispatch_out;
      }

      path = stmt->argv[0];
      rc = UG_repl_stmt_parse_mode( stmt->argv[1], &mode );
      if( rc != 0 ) {
         SG_error("not a mode: '%s'\n", stmt->argv[1]);
         goto UG_repl_stmt_dispatch_out;
      }

      SG_debug("create('%s', %o)\n", path, mode );
      fh = UG_create( ug, path, mode, &rc );
      SG_debug("create('%s', %o) rc = %d\n", path, mode, rc );

      if( fh != NULL ) {
         rc = UG_repl_filedes_insert( repl, fh );
         if( rc < 0 ) {
            UG_close( ug, fh );
            goto UG_repl_stmt_dispatch_out;
         }

         printf("%d\n", rc);
      }

      rc = 0;
   }
   else if( strcmp(stmt->cmd, "getxattr") == 0 ) {

      // path attr
      if( stmt->argc != 2 ) {
         rc = -EINVAL;
         goto UG_repl_stmt_dispatch_out;
      }

      path = stmt->argv[0];
      attrname = stmt->argv[1];

      SG_debug("getxattr('%s', '%s')\n", path, attrname );
      attrlen = UG_getxattr( ug, path, attrname, NULL, 0 ); 
      if( attrlen < 0 ) {
         rc = attrlen;
         SG_debug("getxattr('%s', '%s') rc = %d\n", path, attrname, rc );
         goto UG_repl_stmt_dispatch_out;
      }
      
      attrvalue = SG_CALLOC( char, attrlen+1 );
      if( attrvalue == NULL ) {
         rc = -ENOMEM;
         SG_debug("getxattr('%s', '%s') rc = %d\n", path, attrname, rc );
         goto UG_repl_stmt_dispatch_out;
      }

      attrlen = UG_getxattr( ug, path, attrname, attrvalue, attrlen );
      rc = attrlen;
      if( attrlen < 0 ) {
         SG_safe_free( attrvalue );
         SG_debug("getxattr('%s', '%s') rc = %d\n", path, attrname, rc );
         goto UG_repl_stmt_dispatch_out;
      }

      SG_debug("getxattr('%s', '%s') rc = %d\n", path, attrname, rc );
      printf("%zd\n", attrlen);
      printf("%s\n", attrvalue);
      SG_safe_free( attrvalue );
   }   
   else if( strcmp(stmt->cmd, "listxattr") == 0 ) {

      // path 
      if( stmt->argc != 0 ) {
         rc = -EINVAL;
         goto UG_repl_stmt_dispatch_out;
      }

      path = stmt->argv[0];

      SG_debug("listxattr('%s')\n", path );
      attrlen = UG_listxattr( ug, path, NULL, 0 );
      if( attrlen < 0 ) {
         rc = attrlen;
         SG_debug("listxattr('%s') rc = %zd\n", path, attrlen);
         goto UG_repl_stmt_dispatch_out;
      }

      attrvalue = SG_CALLOC( char, attrlen+1 );
      if( attrvalue == NULL ) {
         rc = -ENOMEM;
         SG_debug("listxattr('%s') rc = %d\n", path, rc);
         goto UG_repl_stmt_dispatch_out;
      }

      attrlen = UG_listxattr( ug, path, attrvalue, attrlen );
      if( attrlen < 0 ) {
         SG_safe_free( attrvalue );
         SG_debug("listxattr('%s') rc = %zd\n", path, attrlen);
         goto UG_repl_stmt_dispatch_out;
      }

      SG_debug("listxattr('%s') rc = %zd\n", path, attrlen);

      printf("%zd\n", attrlen);
      for( offset = 0; offset < (unsigned)attrlen; ) {

         printf("%s\n", attrvalue + offset );
         offset += strlen(attrvalue) + 1;
      }
      SG_safe_free( attrvalue );
   }
   else if( strcmp(stmt->cmd, "mkdir") == 0 ) {

      // path mode
      if( stmt->argc != 4 ) {
         rc = -EINVAL;
         goto UG_repl_stmt_dispatch_out;
      }

      path = stmt->argv[0];
      rc = UG_repl_stmt_parse_mode( stmt->argv[1], &mode );
      if( rc != 0 ) {
         goto UG_repl_stmt_dispatch_out;
      }

      SG_debug("mkdir('%s', %o)\n", path, mode );
      rc = UG_mkdir( ug, path, mode );
      SG_debug("mkdir('%s', %o) rc = %d\n", path, mode, rc );
   }  
   else if( strcmp(stmt->cmd, "open") == 0 ) {

      // path flags 
      if( stmt->argc != 2 ) {
         rc = -EINVAL;
         SG_error("open argc = %d\n", stmt->argc);
         goto UG_repl_stmt_dispatch_out;
      }

      path = stmt->argv[0];
      rc = UG_repl_stmt_parse_uint64( stmt->argv[1], &flags );
      if( rc != 0 ) {

         SG_error("Failed to parse '%s'\n", stmt->argv[1]);
         goto UG_repl_stmt_dispatch_out;
      }

      SG_debug("open('%s', %" PRIx64 ")\n", path, flags );
      fh = UG_open( ug, path, flags, &rc );
      SG_debug("open('%s', %" PRIx64 ") rc = %d\n", path, flags, rc );

      if( fh != NULL ) {
         rc = UG_repl_filedes_insert( repl, fh );
         if( rc < 0 ) {
            UG_close( ug, fh );
            goto UG_repl_stmt_dispatch_out;
         }

         printf("%d\n", rc);
      }

      rc = 0;
   }
   else if( strcmp(stmt->cmd, "opendir") == 0 ) {

      // path 
      if( stmt->argc != 1 ) {
         rc = -EINVAL;
         goto UG_repl_stmt_dispatch_out;
      }

      path = stmt->argv[0];

      SG_debug("opendir('%s')\n", path );
      dh = UG_opendir( ug, path, &rc );
      SG_debug("opendir('%s') rc = %d\n", path, rc );

      if( dh != NULL ) {
         rc = UG_repl_dirdes_insert( repl, dh );
         if( rc < 0 ) {
            UG_closedir( ug, dh );
            goto UG_repl_stmt_dispatch_out;
         }

         printf("%d\n", rc);
      }

      rc = 0;
   }
   else if( strcmp(stmt->cmd, "read") == 0 ) {

      // filedes offset len 
      if( stmt->argc != 3 ) {
         rc = -EINVAL;
         goto UG_repl_stmt_dispatch_out;
      }

      rc = UG_repl_stmt_parse_uint64( stmt->argv[0], &filedes );
      if( rc != 0 ) {
         goto UG_repl_stmt_dispatch_out;
      }

      rc = UG_repl_stmt_parse_uint64( stmt->argv[1], &offset );
      if( rc != 0 ) {
         goto UG_repl_stmt_dispatch_out;
      }

      rc = UG_repl_stmt_parse_uint64( stmt->argv[2], &len );
      if( rc != 0 ) {
         goto UG_repl_stmt_dispatch_out;
      }

      fh = UG_repl_filedes_lookup( repl, filedes );
      if( fh == NULL ) {
         rc = -EBADF;
         goto UG_repl_stmt_dispatch_out;
      }

      buf = SG_CALLOC( char, len+1 );
      if( buf == NULL ) {
         rc = -ENOMEM;
         goto UG_repl_stmt_dispatch_out;
      }

      SG_debug("read(%" PRIu64 ", %" PRIu64 ", %" PRIu64 ")\n", filedes, offset, len );
      UG_seek( fh, offset, SEEK_SET );
      rc = UG_read( ug, buf, len, fh );
      SG_debug("read(%" PRIu64 ", %" PRIu64 ", %" PRIu64 ") rc = %d\n", filedes, offset, len, rc );

      if( rc < 0 ) {
         SG_safe_free( buf );
         goto UG_repl_stmt_dispatch_out;
      }

      printf("%d\n", rc);
      printf("%s", buf);
      fflush(stdout);
      SG_safe_free( buf );
      rc = 0;
   }
   else if( strcmp(stmt->cmd, "readdir") == 0 ) {

      // dirdes num_children 
      if( stmt->argc != 2 ) {
         rc = -EINVAL;
         goto UG_repl_stmt_dispatch_out;
      }

      rc = UG_repl_stmt_parse_uint64( stmt->argv[0], &filedes );
      if( rc != 0 ) {
         goto UG_repl_stmt_dispatch_out;
      }

      rc = UG_repl_stmt_parse_uint64( stmt->argv[1], &num_children );
      if( rc != 0 ) {
         goto UG_repl_stmt_dispatch_out;
      }

      dh = UG_repl_dirdes_lookup( repl, filedes );
      if( dh == NULL ) {
         rc = -EBADF;
         goto UG_repl_stmt_dispatch_out;
      }

      SG_debug("readdir(%" PRIu64 ", %" PRIu64 ")\n", filedes, num_children);
      rc = UG_readdir( ug, &children, num_children, dh );
      SG_debug("readdir(%" PRIu64 ", %" PRIu64 ") rc = %d\n", filedes, num_children, rc);

      if( children == NULL ) {
         goto UG_repl_stmt_dispatch_out;
      }

      printf("%" PRIu64 "\n", len);

      for( uint64_t i = 0; i < len; i++ ) {

         struct md_entry* dent = children[i];
         printf("%d %" PRIX64 " '%s'\n", dent->type, dent->file_id, dent->name );
      }
        
      UG_free_dir_listing( children );
      rc = 0;
   }   
   else if( strcmp(stmt->cmd, "removexattr") == 0 ) {

      // path attrname 
      if( stmt->argc != 2 ) {
         rc = -EINVAL;
         goto UG_repl_stmt_dispatch_out;
      }

      path = stmt->argv[0];
      attrname = stmt->argv[1];

      SG_debug("removexattr('%s', '%s')\n", path, attrname );
      rc = UG_removexattr( ug, path, attrname );
      SG_debug("removexattr('%s', '%s') rc = %d\n", path, attrname, rc );

      if( rc < 0 ) {
         goto UG_repl_stmt_dispatch_out;
      }
   }
   else if( strcmp(stmt->cmd, "rename") == 0 ) {

      // path newpath 
      if( stmt->argc != 2 ) {
         rc = -EINVAL;
         goto UG_repl_stmt_dispatch_out;
      }

      path = stmt->argv[0];
      newpath = stmt->argv[1];

      SG_debug("rename('%s', '%s')\n", path, newpath );
      rc = UG_rename( ug, path, newpath );
      SG_debug("rename('%s', '%s') rc = %d\n", path, newpath, rc );

      if( rc < 0 ) {
         goto UG_repl_stmt_dispatch_out;
      }
   }
   else if( strcmp(stmt->cmd, "rmdir") == 0 ) {

      // path 
      if( stmt->argc != 0 ) {
         rc = -EINVAL;
         goto UG_repl_stmt_dispatch_out;
      }

      path = stmt->argv[0];

      SG_debug("rmdir('%s')\n", path );
      rc = UG_rmdir( ug, path );
      SG_debug("rmdir('%s') rc = %d\n", path, rc );

      if( rc < 0 ) {
         goto UG_repl_stmt_dispatch_out;
      }
   }
   else if( strcmp(stmt->cmd, "setxattr") == 0 ) {

      // path attrname flags attrvalue
      if( stmt->argc != 4 ) {
         rc = -EINVAL;
         goto UG_repl_stmt_dispatch_out;
      }

      path = stmt->argv[0];
      attrname = stmt->argv[1];
      attrvalue = stmt->argv[3];

      rc = UG_repl_stmt_parse_uint64( stmt->argv[2], &flags );
      if( rc != 0 ) {
         goto UG_repl_stmt_dispatch_out;
      }

      SG_debug("setxattr('%s', '%s', '%s', %" PRIx64 ")\n", path, attrname, attrvalue, flags );
      rc = UG_setxattr( ug, path, attrname, attrvalue, strlen(attrvalue), flags );
      SG_debug("setxattr('%s', '%s', '%s', %" PRIx64 ") rc = %d\n", path, attrname, attrvalue, flags, rc );

      if( rc < 0 ) {
         goto UG_repl_stmt_dispatch_out;
      }
   }
   else if( strcmp(stmt->cmd, "stat") == 0 ) {

      // path 
      if( stmt->argc != 1 ) {
         rc = -EINVAL;
         goto UG_repl_stmt_dispatch_out;
      }

      path = stmt->argv[1];

      SG_debug("stat('%s')\n", path );
      rc = UG_stat( ug, path, &sb );
      SG_debug("stat('%s') rc = %d\n", path, rc );

      if( rc < 0 ) {
         goto UG_repl_stmt_dispatch_out;
      }

      printf("st_dev=%" PRIu64 ", st_ino=%" PRIX64 ", st_mode=%o, st_nlink=%" PRIu64 ", st_uid=%" PRIu64 ", st_gid=%" PRIu64 ", st_rdev=%" PRIu64 ", st_size=%" PRIu64 
            ", st_blksize=%" PRIu64 ", st_blocks=%" PRIu64 ", st_atime=%" PRIu64 ".%ld, st_mtime=%" PRIu64 ".%ld, st_ctime=%" PRIu64 ".%ld\n",
             sb.st_dev, sb.st_ino, sb.st_mode, sb.st_nlink, (uint64_t)sb.st_uid, (uint64_t)sb.st_gid, sb.st_rdev, sb.st_size, sb.st_blksize, sb.st_blocks,
             sb.st_atim.tv_sec, sb.st_atim.tv_nsec, sb.st_mtim.tv_sec, sb.st_mtim.tv_nsec, sb.st_ctim.tv_sec, sb.st_ctim.tv_nsec );
             
   }   
   else if( strcmp(stmt->cmd, "sync") == 0 ) {

      // filedes 
      if( stmt->argc != 1 ) {
         rc = -EINVAL;
         goto UG_repl_stmt_dispatch_out;
      }

      rc = UG_repl_stmt_parse_uint64( stmt->argv[0], &filedes );
      if( rc != 0 ) {
         goto UG_repl_stmt_dispatch_out;
      }

      fh = UG_repl_filedes_lookup( repl, filedes );
      if( fh == NULL ) {
         rc = -EBADF;
         goto UG_repl_stmt_dispatch_out;
      }

      SG_debug("fsync(%" PRIu64 ")\n", filedes );
      rc = UG_fsync( ug, fh );
      SG_debug("fsync(%" PRIu64 ") rc = %d\n", filedes, rc );

      if( rc < 0 ) {
         goto UG_repl_stmt_dispatch_out;
      }
   }
   else if( strcmp(stmt->cmd, "trunc") == 0 ) {

      // path newsize 
      if( stmt->argc != 2 ) {
         rc = -EINVAL;
         goto UG_repl_stmt_dispatch_out;
      }

      path = stmt->argv[0];
      rc = UG_repl_stmt_parse_uint64( stmt->argv[1], &size );
      if( rc != 0 ) {
         goto UG_repl_stmt_dispatch_out;
      }

      SG_debug("trunc('%s', %" PRIu64 ")\n", path, size );
      rc = UG_truncate( ug, path, size );
      SG_debug("trunc('%s', %" PRIu64 ") rc = %d\n", path, size, rc );

      if( rc < 0 ) {
         goto UG_repl_stmt_dispatch_out;
      }
   }
   else if( strcmp(stmt->cmd, "ftrunc") == 0 ) {

      // fd newsize
      if( stmt->argc != 2 ) {
         rc = -EINVAL;
         goto UG_repl_stmt_dispatch_out;
      }

      rc = UG_repl_stmt_parse_uint64( stmt->argv[0], &filedes );
      if( rc != 0 ) {
         goto UG_repl_stmt_dispatch_out;
      }

      fh = UG_repl_filedes_lookup( repl, filedes );
      if( fh == NULL ) {
         rc = -EBADF;
         goto UG_repl_stmt_dispatch_out;
      }

      rc = UG_repl_stmt_parse_uint64( stmt->argv[1], &size );
      if( rc != 0 ) {
         goto UG_repl_stmt_dispatch_out;
      }

      SG_debug("ftrunc(%" PRIu64 ", %" PRIu64 ")\n", filedes, size);
      rc = UG_ftruncate( ug, size, fh );
      SG_debug("ftrunc(%" PRIu64 ", %" PRIu64 ") rc = %d\n", filedes, size, rc );

      if( rc < 0 ) {
         goto UG_repl_stmt_dispatch_out;
      }
   }     
   else if( strcmp(stmt->cmd, "unlink") == 0 ) {

      // path 
      if( stmt->argc != 1 ) {
         rc = -EINVAL;
         goto UG_repl_stmt_dispatch_out;
      }

      path = stmt->argv[0];
      
      SG_debug("unlink('%s')\n", path );
      rc = UG_unlink( ug, path );
      SG_debug("unlink('%s') rc = %d\n", path, rc );

      if( rc != 0 ) {
         goto UG_repl_stmt_dispatch_out;
      }
   }
   else if( strcmp(stmt->cmd, "utime") == 0 ) {

      // path atime_sec mtime_sec 
      if( stmt->argc != 3 ) {
         rc = -EINVAL;
         goto UG_repl_stmt_dispatch_out;
      }

      memset( &ubuf, 0, sizeof(struct utimbuf));

      path = stmt->argv[0];
      rc = UG_repl_stmt_parse_uint64( stmt->argv[1], &timetmp );
      if( rc != 0 ) {
         goto UG_repl_stmt_dispatch_out;
      }

      ubuf.actime = timetmp;

      rc = UG_repl_stmt_parse_uint64( stmt->argv[2], &timetmp );
      if( rc != 0 ) {
         goto UG_repl_stmt_dispatch_out;
      }

      ubuf.modtime = timetmp;

      SG_debug("utimes('%s', %ld, %ld)\n", path, ubuf.actime, ubuf.modtime );
      rc = UG_utime( ug, path, &ubuf );
      SG_debug("utimes('%s', %ld, %ld) rc = %d\n", path, ubuf.actime, ubuf.modtime, rc );

      if( rc != 0 ) {
         goto UG_repl_stmt_dispatch_out;
      }
   }
   else if( strcmp(stmt->cmd, "write") == 0 ) {

      // filedes offset length string
      if( stmt->argc != 4 ) {
         rc = -EINVAL;
         goto UG_repl_stmt_dispatch_out;
      }

      rc = UG_repl_stmt_parse_uint64( stmt->argv[0], &filedes );
      if( rc != 0 ) {
         goto UG_repl_stmt_dispatch_out;
      }

      buf = stmt->argv[3];

      rc = UG_repl_stmt_parse_uint64( stmt->argv[1], &offset );
      if( rc != 0 ) {
         goto UG_repl_stmt_dispatch_out;
      }

      rc = UG_repl_stmt_parse_uint64( stmt->argv[2], &len );
      if( rc != 0 ) {
         goto UG_repl_stmt_dispatch_out;
      }

      fh = UG_repl_filedes_lookup( repl, filedes );
      if( fh == NULL ) {
         rc = -EBADF;
         goto UG_repl_stmt_dispatch_out;
      }

      memset( debug_buf, 0, 52 );
      memcpy( debug_buf, buf, MIN(len, 45) );

      if( len >= 45 ) {
          memcpy( debug_buf + (MIN(len, 45)), "...", 4 );
      }

      SG_debug("write(%" PRIu64 ", '%s', %" PRIu64 ", %zu)\n", filedes, debug_buf, offset, len );
      UG_seek( fh, offset, SEEK_SET );
      rc = UG_write( ug, buf, len, fh );
      SG_debug("write(%" PRIu64 ", '%s', %" PRIu64 ", %zu) rc = %d\n", filedes, debug_buf, offset, len, rc );

      if( rc < 0 ) {
         goto UG_repl_stmt_dispatch_out;
      }

      printf("%d\n", rc);
   }
   else if( strcmp(stmt->cmd, "shell") == 0 ) {

      // shell out and run argv[1...n]
      size = 0;
      for( int i = 0; stmt->argv[i] != NULL; i++ ) {
         size += strlen(stmt->argv[i]) + 2;
      }

      full_cmd = SG_CALLOC( char, size );
      if( full_cmd == NULL ) {
         return -ENOMEM;
      }

      for( int i = 0; stmt->argv[i] != NULL; i++ ) {
         strcat(full_cmd, stmt->argv[i]);
         strcat(full_cmd, " ");
      }

      SG_debug("sh -c '%s'\n", full_cmd);
      rc = system(full_cmd);
      SG_debug("sh -c '%s' rc = %d\n", full_cmd, rc);

      if( rc != 0 ) {
         SG_error("system('%s') rc = %d\n", full_cmd, rc );
      }

      SG_safe_free( full_cmd );
      printf("%d\n", rc);
   }

   else {

      SG_error("Unrecognized command '%s'\n", stmt->cmd );
      rc = -EINVAL;
   } 
         
UG_repl_stmt_dispatch_out:

   return rc;
}


// main REPL loop
// reads commands from the given file until EOF,
// and dispatches them to the given repl.
// returns the status of the last line processed
int UG_repl_main( struct UG_repl* repl, FILE* f ) {

   int rc = 0;
   struct UG_repl_stmt* stmt = NULL;

   while( !feof( f ) ) {

       stmt = UG_repl_stmt_parse( f, &rc );
       if( stmt == NULL ) {
          if( rc != 0 && rc != -ENODATA ) {
              SG_error("UG_repl_stmt_parse failed, rc = %d\n", rc);
          }
          else if( rc == -ENODATA ) {
              // EOF
              rc = 0;
              break;
          }
          continue;
       }

       rc = UG_repl_stmt_dispatch( repl, stmt );
       if( rc < 0 ) {
          SG_error("UG_repl_stmt_dispatch('%s') rc = %d\n", stmt->cmd, rc );
       }
       
       UG_repl_stmt_free( stmt );
   }

   return rc;
}


// reads a string of commands from stdin and feed them to the UG
int main(int argc, char** argv) {
   
   struct UG_repl* repl = NULL;
   int rc = 0;
   struct UG_state* ug = NULL;

   struct tool_opts opts;
   
   memset( &opts, 0, sizeof(tool_opts) );
   
   argc = parse_args( argc, argv, &opts );
   if( argc < 0 ) {
      
      usage( argv[0], "file [file...]" );
      md_common_usage();
      exit(1);
   }
   
   // setup...
   ug = UG_init( argc, argv );
   if( ug == NULL ) {
      
      SG_error("%s", "UG_init failed\n" );
      exit(1);
   }

   repl = UG_repl_new( ug );
   if( repl == NULL ) {
      exit(1);
   }

   // read from stdin
   rc = UG_repl_main( repl, stdin );
   
   UG_repl_free( repl );
   UG_shutdown( ug );

   if( rc >= 0 ) {
      exit(0);
   }
   else {
      exit(1);
   }
}

