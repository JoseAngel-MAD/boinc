// This file is part of BOINC.
// http://boinc.berkeley.edu
// Copyright (C) 2008 University of California
//
// BOINC is free software; you can redistribute it and/or modify it
// under the terms of the GNU Lesser General Public License
// as published by the Free Software Foundation,
// either version 3 of the License, or (at your option) any later version.
//
// BOINC is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
// See the GNU Lesser General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public License
// along with BOINC.  If not, see <http://www.gnu.org/licenses/>.

// The BOINC file upload handler.
// See doc/upload.php for protocol spec.
//

#include "config.h"
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <sys/stat.h>
#include <sys/types.h>
#include <cerrno>
#include <unistd.h>
#include <csignal>
#include <fcntl.h>

#ifdef _USING_FCGI_
#include "boinc_fcgi.h"
#else
#include <cstdio>
#endif

#include "crypt.h"
#include "parse.h"
#include "util.h"
#include "error_numbers.h"
#include "filesys.h"

#include "sched_config.h"
#include "sched_util.h"

#include "sched_msgs.h"

#define ERR_TRANSIENT   true
#define ERR_PERMANENT   false

char this_filename[256];
double start_time();

struct FILE_INFO {
    char name[256];
    double max_nbytes;
    char* xml_signature;
    char* signed_xml;
    int parse(FILE*);
};

int FILE_INFO::parse(FILE* in) {
    char buf[256];
    int retval;

    memset(this, 0, sizeof(FILE_INFO));
    signed_xml = strdup("");
    while (fgets(buf, 256, in)) {
        //log_messages.printf(MSG_DEBUG, buf, "FILE_INFO::parse: ");
        if (match_tag(buf, "</file_info>")) return 0;
        if (match_tag(buf, "<xml_signature>")) {
            retval = dup_element_contents(in, "</xml_signature>", &xml_signature);
            if (retval) return retval;
            continue;
        }
        retval = strcatdup(signed_xml, buf);
        if (retval) return retval;
        if (parse_str(buf, "<name>", name, sizeof(name))) {
            strcpy(this_filename, name);
            continue;
        }
        if (parse_double(buf, "<max_nbytes>", max_nbytes)) continue;
        if (match_tag(buf, "<generated_locally/>")) continue;
        if (match_tag(buf, "<upload_when_present/>")) continue;
        if (match_tag(buf, "<url>")) continue;
        if (match_tag(buf, "<gzip_when_done")) continue;
        log_messages.printf(MSG_NORMAL,
            "FILE_INFO::parse: unrecognized: %s \n", buf
        );
    }
    return ERR_XML_PARSE;
}

inline static const char* get_remote_addr() {
    char* p = getenv("REMOTE_ADDR");
    if (p) return p;
    return "Unknown remote address";
}

int return_error(bool transient, const char* message, ...) {
    va_list va;
    va_start(va, message);
    char buf[10240];

    vsprintf(buf, message, va);
    va_end(va);

    fprintf(stdout,
        "Content-type: text/plain\n\n"
        "<data_server_reply>\n"
        "    <status>%d</status>\n"
        "    <message>%s</message>\n"
        "</data_server_reply>\n",
        transient?1:-1,
        buf
    );

    log_messages.printf(MSG_NORMAL,
        "Returning error to client %s: %s (%s)\n",
        get_remote_addr(), buf,
        transient?"transient":"permanent"
    );
    return 1;
}

int return_success(const char* text) {
    fprintf(stdout,
        "Content-type: text/plain\n\n"
        "<data_server_reply>\n"
        "    <status>0</status>\n"
    );
    if (text) {
        fprintf(stdout, "    %s\n", text);
    }
    fprintf(stdout, "</data_server_reply>\n");
    return 0;
}

#define BLOCK_SIZE  16382
double bytes_left=-1;

// read from socket, write to file
// ALWAYS returns an HTML reply
//
int copy_socket_to_file(FILE* in, char* path, double offset, double nbytes) {
    unsigned char buf[BLOCK_SIZE];
    struct stat sbuf;
    int pid;

    // open file.  Use raw IO not buffered IO so that we can use reliable
    // posix file locking.
    // Advisory file locking is not guaranteed reliable when
    // used with stream buffered IO.
    //
    int fd = open(path,
        O_WRONLY|O_CREAT|O_APPEND,
        S_IRUSR|S_IWUSR|S_IRGRP|S_IROTH
    );
    if (fd<0) {
        return return_error(ERR_TRANSIENT,
            "can't open file %s: %s\n", path, strerror(errno)
        );
    }

    // Put an advisory lock on the file.
    // This will prevent OTHER instances of file_upload_handler
    // from being able to write to the file.
    //
    if ((pid=mylockf(fd))) {
        close(fd);
        return return_error(ERR_TRANSIENT,
            "can't lock file %s: %s locked by PID=%d\n",
            path, strerror(errno), pid
        );
    }

    // check that file length corresponds to offset
    // TODO: use a 64-bit variant
    //
    if (stat(path, &sbuf)) {
        close(fd);
        return return_error(ERR_TRANSIENT,
            "can't stat file %s: %s\n", path, strerror(errno)
        );
    }
    if (sbuf.st_size < offset) {
        close(fd);
        return return_error(ERR_TRANSIENT,
            "length of file %s %d bytes < offset %.0f bytes",
            path, (int)sbuf.st_size, offset
        );
    }
    if (sbuf.st_size > offset) {
        log_messages.printf(MSG_CRITICAL,
            "file %s length on disk %d bytes; host upload starting at %.0f bytes.\n",
             this_filename, (int)sbuf.st_size, offset
        );
    }

    // caller guarantees that nbytes > offset
    //
    bytes_left = nbytes - offset;

    while (bytes_left > 0) {

        int n, m, to_write;

        m = bytes_left<(double)BLOCK_SIZE ? (int)bytes_left : BLOCK_SIZE;

        // try to get m bytes from socket (n>=0 is number actually returned)
        //
        n = fread(buf, 1, m, in);

        // try to write n bytes to file
        //
        to_write=n;
        while (to_write > 0) {
            ssize_t ret = write(fd, buf+n-to_write, to_write);
            if (ret < 0) {
                close(fd);
                const char* errmsg;
                if (errno == ENOSPC) {
                    errmsg = "No space left on server";
                } else {
                    errmsg = strerror(errno);
                }
                return return_error(ERR_TRANSIENT,
                    "can't write file %s: %s\n", path, errmsg
                );
            }
            to_write -= ret;
        }

        // check that we got all bytes from socket that were requested
        // Note: fread() reads less than requested only if there's
        // an error or EOF (see the man page)
        //
        if (n != m) {
            close(fd);
            if (feof(in)) {
                return return_error(ERR_TRANSIENT,
                    "EOF on socket read : asked for %d, got %d\n",
                    m, n
                );
            } else if (ferror(in)) {
                return return_error(ERR_TRANSIENT,
                    "error %d (%s) on socket read: asked for %d, got %d\n",
                    ferror(in), strerror(ferror(in)), m, n
                );
            } else {
                return return_error(ERR_TRANSIENT,
                    "incomplete socket read: asked for %d, got %d\n",
                    m, n
                );
            }
        }

        bytes_left -= n;
    }
    close(fd);
    return return_success(0);
}

// read from socket, discard data
//
void copy_socket_to_null(FILE* in) {
    unsigned char buf[BLOCK_SIZE];

    while (1) {
        int n = fread(buf, 1, BLOCK_SIZE, in);
        if (n <= 0) return;
    }
}

// ALWAYS generates an HTML reply
//
int handle_file_upload(FILE* in, R_RSA_PUBLIC_KEY& key) {
    char buf[256], path[512], temp[256];
    FILE_INFO file_info;
    int retval;
    double nbytes=-1, offset=0;
    bool is_valid;

    while (fgets(buf, 256, in)) {
#if 0
        log_messages.printf(MSG_NORMAL, "got:%s\n", buf);
#endif
        if (match_tag(buf, "<file_info>")) {
            retval = file_info.parse(in);
            if (retval) {
                return return_error(ERR_PERMANENT, "FILE_INFO::parse");
            }
#if 0
            log_messages.printf(MSG_NORMAL,
                "file info:\n%s\n", file_info.signed_xml
            );
#endif
            if (!config.ignore_upload_certificates) {
                if (!file_info.signed_xml || !file_info.xml_signature) {
                    log_messages.printf(MSG_CRITICAL,
                        "file info is missing signature\n"
                    );
                    return return_error(ERR_PERMANENT, "invalid signature");
                } else {
                    retval = verify_string(
                        file_info.signed_xml, file_info.xml_signature, key, is_valid
                    );
                    if (retval || !is_valid) {
                        log_messages.printf(MSG_CRITICAL,
                            "verify_string() [%s] [%s] retval %d, is_valid = %d\n",
                            file_info.signed_xml, file_info.xml_signature,
                            retval, is_valid
                        );
                        log_messages.printf(MSG_NORMAL,
                            "signed xml: %s", file_info.signed_xml
                        );
                        log_messages.printf(MSG_NORMAL,
                            "signature: %s", file_info.xml_signature
                        );
                        return return_error(ERR_PERMANENT, "invalid signature");
                    }
                }
            }
            continue;
        }
        if (parse_double(buf, "<offset>", offset)) continue;
        if (parse_double(buf, "<nbytes>", nbytes)) continue;
        if (parse_str(buf, "<md5_cksum>", temp, sizeof(temp))) continue;
        if (match_tag(buf, "<data>")) {
            if (nbytes <= 0) {
                return return_error(ERR_PERMANENT, "nbytes missing or negative");
            }

            // enforce limits in signed XML
            //
            if (!config.ignore_upload_certificates) {
                if (nbytes > file_info.max_nbytes) {
                    sprintf(buf,
                        "file size (%d KB) exceeds limit (%d KB)",
                        (int)(nbytes/1024), (int)(file_info.max_nbytes/1024)
                    );
                    copy_socket_to_null(in);
                    return return_error(ERR_PERMANENT, buf);
                }
            }

            // make sure filename is legit
            //
            if (strstr(file_info.name, "..")) {
                return return_error(ERR_PERMANENT,
                    "file_upload_handler: .. found in filename: %s",
                    file_info.name
                );
            }

            if (strlen(file_info.name) == 0) {
                return return_error(ERR_PERMANENT,
                    "file_upload_handler: no filename; nbytes %f", nbytes
                );
            }

            retval = dir_hier_path(
                file_info.name, config.upload_dir, config.uldl_dir_fanout,
                path, true
            );
            if (retval) {
                log_messages.printf(MSG_CRITICAL,
                    "Failed to find/create directory for file '%s' in '%s'\n",
                    file_info.name, config.upload_dir
                );
                return return_error(ERR_TRANSIENT, "can't open file");
            }
            log_messages.printf(MSG_NORMAL,
                "Starting upload of %s from %s [offset=%.0f, nbytes=%.0f]\n",
                file_info.name,
                get_remote_addr(),
                offset, nbytes
            );
#ifndef _USING_FCGI_
            fflush(stderr);
#endif
            if (offset >= nbytes) {
                log_messages.printf(MSG_CRITICAL,
                    "ERROR: offset >= nbytes!!\n"
                );
                return return_success(0);
            }
            retval = copy_socket_to_file(in, path, offset, nbytes);
            log_messages.printf(MSG_NORMAL,
                "Ended upload of %s from %s; retval %d\n",
                file_info.name,
                get_remote_addr(),
                retval
            );
#ifndef _USING_FCGI_
            fflush(stderr);
#endif
            return retval;
        }
        log_messages.printf(MSG_CRITICAL,
            "unrecognized: %s", buf
        );
    }
    return return_error(ERR_PERMANENT, "Missing <data> tag");
}

bool volume_full(char* path) {
    double total, avail;
    int retval = get_filesystem_info(total, avail, path);
    if (retval) return false;
    if (avail<1e6) {
        return true;
    }
    return false;
}

// always returns HTML reply
//
int handle_get_file_size(char* file_name) {
    struct stat sbuf;
    char path[512], buf[256];
    int retval, pid, fd;

    // TODO: check to ensure path doesn't point somewhere bad
    // Use 64-bit variant
    //
    retval = dir_hier_path(
        file_name, config.upload_dir, config.uldl_dir_fanout, path
    );
    if (retval) {
        log_messages.printf(MSG_CRITICAL,
            "Failed to find/create directory for file '%s' in '%s'.\n",
            file_name, config.upload_dir
        );
        return return_error(ERR_TRANSIENT, "can't open file");
    }

    // if the volume is full, report a transient error
    // to prevent the client from starting a transfer
    //
    if (volume_full(config.upload_dir)) {
        return return_error(ERR_TRANSIENT, "Server is out of disk space");
    }

    fd = open(path, O_WRONLY|O_APPEND);

    if (fd<0 && ENOENT==errno) {
        // file does not exist: return zero length
        //
        log_messages.printf(MSG_NORMAL,
            "handle_get_file_size(): [%s] returning zero\n", file_name
        );
        return return_success("<file_size>0</file_size>");
    }

    if (fd<0) {
        // can't get file descriptor: try again later
        //
        log_messages.printf(MSG_CRITICAL,
            "handle_get_file_size(): cannot open [%s] %s\n",
            file_name, strerror(errno)
        );
        return return_error(ERR_TRANSIENT, "can't open file");
    }

    if ((pid=mylockf(fd))) {
        // file locked by another file_upload_handler: try again later
        //
        close(fd);
        log_messages.printf(MSG_CRITICAL,
            "handle_get_file_size(): [%s] returning error\n", file_name
        );
        return return_error(ERR_TRANSIENT,
            "[%s] locked by file_upload_handler PID=%d", file_name, pid
        );
    }
    // file exists, writable, not locked by anyone else, so return length.
    //
    retval = stat(path, &sbuf);
    close(fd);
    if (retval) {
        // file DOES perhaps exist, but can't stat it: try again later
        //
        log_messages.printf(MSG_CRITICAL,
            "handle_get_file_size(): [%s] returning error %s\n",
            file_name, strerror(errno)
        );
        return return_error(ERR_TRANSIENT, "cannot stat file" );
    }

    log_messages.printf(MSG_NORMAL,
        "handle_get_file_size(): [%s] returning %d\n",
        file_name, (int)sbuf.st_size
    );
    sprintf(buf, "<file_size>%d</file_size>", (int)sbuf.st_size);
    return return_success(buf);
}

// always generates an HTML reply
//
int handle_request(FILE* in, R_RSA_PUBLIC_KEY& key) {
    char buf[256];
    char file_name[256];
    int major, minor, release, retval=0;
    bool got_version = true;
    bool did_something = false;
    double start_time = dtime();

#ifdef _USING_FCGI_
    log_messages.set_indent_level(1);
#endif
    while (fgets(buf, 256, in)) {
        log_messages.printf(MSG_DEBUG, "handle_request: %s", buf);
        if (parse_int(buf, "<core_client_major_version>", major)) {
            continue;
        } else if (parse_int(buf, "<core_client_minor_version>", minor)) {
            continue;
        } else if (parse_int(buf, "<core_client_release>", release)) {
            continue;
        } else if (match_tag(buf, "<file_upload>")) {

            if (!got_version) {
                retval = return_error(ERR_PERMANENT, "Missing version");
            } else {
                retval = handle_file_upload(in, key);
            }
            did_something = true;
            break;
        } else if (parse_str(buf, "<get_file_size>", file_name, sizeof(file_name))) {
            if (strstr(file_name, "..")) {
                return return_error(ERR_PERMANENT, "Bad filename");
            }
            if (!got_version) {
                retval = return_error(ERR_PERMANENT, "Missing version");
            } else {
                retval = handle_get_file_size(file_name);
            }
            did_something = true;
            break;
        } else if (match_tag(buf, "<data_server_request>")) {
            // DO NOTHING
        } else {
            log_messages.printf(MSG_DEBUG, "handle_request: unrecognized %s\n", buf);
        }
    }
    if (!did_something) {
        log_messages.printf(MSG_CRITICAL, "handle_request: no command\n");
        return return_error(ERR_TRANSIENT, "no command");
    }

    log_messages.printf(MSG_DEBUG, "elapsed time %f seconds\n", dtime()-start_time);

    return retval;
}

int get_key(R_RSA_PUBLIC_KEY& key) {
    int retval;
    char buf[256];
    sprintf(buf, "%s/upload_public", config.key_dir);
#ifndef _USING_FCGI_
    FILE *f = fopen(buf, "r");
#else
    FCGI_FILE *f = FCGI::fopen(buf, "r");
#endif
    if (!f) return -1;
    retval = scan_key_hex(f, (KEY*)&key, sizeof(key));
    fclose(f);
    if (retval) return retval;
    return 0;
}

void boinc_catch_signal(int signal_num) {
    char buffer[512]="";
    if (this_filename[0]) {
        sprintf(buffer, "FILE=%s (%.0f bytes left) ", this_filename, bytes_left);
    }
    log_messages.printf(MSG_CRITICAL,
        "%sIP=%s caught signal %d [%s]\n",
        buffer, get_remote_addr(),
        signal_num, strsignal(signal_num)
    );

    // there is no point in trying to return an error.
    // At this point Apache has broken the connection
    // so a write to stdout will just generate a SIGPIPE
    //
    // return_error(ERR_TRANSIENT, "while downloading %s server caught signal %d", this_filename, signal_num);
    exit(1);
}

void installer() {
    signal(SIGHUP, boinc_catch_signal);  // terminal line hangup
    signal(SIGINT, boinc_catch_signal);  // interrupt program
    signal(SIGQUIT, boinc_catch_signal); // quit program
    signal(SIGILL, boinc_catch_signal);  // illegal instruction
    signal(SIGTRAP, boinc_catch_signal); // illegal instruction
    signal(SIGABRT, boinc_catch_signal); // abort(2) call
    signal(SIGFPE, boinc_catch_signal);  // bus error
    signal(SIGKILL, boinc_catch_signal); // bus error
    signal(SIGBUS, boinc_catch_signal);  // bus error
    signal(SIGSEGV, boinc_catch_signal); // segmentation violation
    signal(SIGSYS, boinc_catch_signal);  // system call given invalid argument
    signal(SIGPIPE, boinc_catch_signal); // write on a pipe with no reader
    signal(SIGTERM, boinc_catch_signal); // terminate process
}

int main() {
    int retval;
    R_RSA_PUBLIC_KEY key;
    char log_path[256];
#ifdef _USING_FCGI_
    unsigned int counter=0;
#endif

    installer();

    get_log_path(log_path, "file_upload_handler.log");
#ifndef _USING_FCGI_
    if (!freopen(log_path, "a", stderr)) {
        fprintf(stderr, "Can't open log file '%s' (errno: %d)\n",
            log_path, errno
        );
        return_error(ERR_TRANSIENT, "can't open log file '%s' (errno: %d)",
            log_path, errno
        );
        exit(1);
    }
#else
    FCGI_FILE *f = FCGI::fopen(log_path, "a");
    if (f) {
       log_messages.redirect(f);
    } else {
        fprintf(stderr, "Can't redirect FCGI log messages\n");
        return_error(ERR_TRANSIENT, "can't open log file (FCGI)");
        exit(1);
    }
#endif

    retval = config.parse_file("..");
    if (retval) {
        fprintf(stderr, "Can't parse config file\n");
        return_error(ERR_TRANSIENT,
            "can't parse config file", log_path, errno
        );
        exit(1);
    }

    log_messages.pid = getpid();
    log_messages.set_debug_level(config.fuh_debug_level);

    if (boinc_file_exists("../stop_upload")) {
        return_error(ERR_TRANSIENT, "Maintenance underway: file uploads are temporarily disabled.");
        exit(1);
    }

    if (!config.ignore_upload_certificates) {
        retval = get_key(key);
        if (retval) {
            return_error(ERR_TRANSIENT, "can't read key file");
            exit(1);
        }
    }

#ifdef _USING_FCGI_
  while(FCGI_Accept() >= 0) {
    counter++;
    //fprintf(stderr, "file_upload_handler (FCGI): counter: %d\n", counter);
    log_messages.set_indent_level(0);
#endif
    handle_request(stdin, key);
#ifdef _USING_FCGI_
    // flush log for FCGI, otherwise it just buffers a lot
    log_messages.flush();
  }
  // when exiting, write headers back to apache so it won't complain
  // about "incomplete headers"
  fprintf(stdout,"Content-type: text/plain\n\n");
#endif
    return 0;
}

const char *BOINC_RCSID_470a0d4d11 = "$Id$";
