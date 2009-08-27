/* Copyright (c) 2008 Apple Inc.
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation files
 * (the "Software"), to deal in the Software without restriction,
 * including without limitation the rights to use, copy, modify, merge,
 * publish, distribute, sublicense, and/or sell copies of the Software,
 * and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT.  IN NO EVENT SHALL THE ABOVE LISTED COPYRIGHT
 * HOLDER(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 * Except as contained in this notice, the name(s) of the above
 * copyright holders shall not be used in advertising or otherwise to
 * promote the sale, use or other dealings in this Software without
 * prior written authorization.
 */

#include <CoreServices/CoreServices.h>

#ifdef HAVE_DIX_CONFIG_H
#include <dix-config.h>
#endif

#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>

#include <sys/socket.h>
#include <sys/un.h>

static char *server_bootstrap_name = "org.x.X11";

/* The launchd startup is only designed for the primary X11.app that is
 * org.x.X11... server_bootstrap_name might be differnet if we were triggered to
 * start by another X11.app.
 */
#define kX11AppBundleId "org.x.X11"
#define kX11AppBundlePath "/Contents/MacOS/X11"

#include <mach/mach.h>
#include <mach/mach_error.h>
#include <servers/bootstrap.h>
#include "mach_startup.h"

#include <signal.h>

#include <AvailabilityMacros.h>

#include "launchd_fd.h"

#ifndef BUILD_DATE
#define BUILD_DATE "?"
#endif
#ifndef XSERVER_VERSION
#define XSERVER_VERSION "?"
#endif

#define DEBUG 1

static char x11_path[PATH_MAX + 1];

static pid_t x11app_pid = 0;

static void set_x11_path() {
#if MAC_OS_X_VERSION_MIN_REQUIRED >= 1050

    CFURLRef appURL = NULL;
    CFBundleRef bundle = NULL;
    OSStatus osstatus = LSFindApplicationForInfo(kLSUnknownCreator, CFSTR(kX11AppBundleId), nil, nil, &appURL);
    UInt32 ver;

    switch (osstatus) {
        case noErr:
            if (appURL == NULL) {
                fprintf(stderr, "Xquartz: Invalid response from LSFindApplicationForInfo(%s)\n", 
                        kX11AppBundleId);
                exit(1);
            }

            bundle = CFBundleCreate(NULL, appURL);
            if(!bundle) {
                fprintf(stderr, "Xquartz: Null value returned from CFBundleCreate().\n");
                exit(2);                
            }

            if (!CFURLGetFileSystemRepresentation(appURL, true, (unsigned char *)x11_path, sizeof(x11_path))) {
                fprintf(stderr, "Xquartz: Error resolving URL for %s\n", kX11AppBundleId);
                exit(3);
            }

            ver = CFBundleGetVersionNumber(bundle);
            if( !(ver >= 0x02308000 || (ver >= 0x02168000 && ver < 0x02208000))) {
                CFStringRef versionStr = CFBundleGetValueForInfoDictionaryKey(bundle, kCFBundleVersionKey);
                const char * versionCStr = "Unknown";

                if(versionStr) 
                    versionCStr = CFStringGetCStringPtr(versionStr, kCFStringEncodingMacRoman);

                fprintf(stderr, "Xquartz: Could not find a new enough X11.app LSFindApplicationForInfo() returned\n");
                fprintf(stderr, "         X11.app = %s\n", x11_path);
                fprintf(stderr, "         Version = %s (%x), Expected Version > 2.3.0 or 2.1.6\n", versionCStr, (unsigned)ver);
                exit(9);
            }

            strlcat(x11_path, kX11AppBundlePath, sizeof(x11_path));
#ifdef DEBUG
            fprintf(stderr, "Xquartz: X11.app = %s\n", x11_path);
#endif
            break;
        case kLSApplicationNotFoundErr:
            fprintf(stderr, "Xquartz: Unable to find application for %s\n", kX11AppBundleId);
            exit(10);
        default:
            fprintf(stderr, "Xquartz: Unable to find application for %s, error code = %d\n", 
                    kX11AppBundleId, (int)osstatus);
            exit(11);
    }
#else
    /* TODO: Make Tiger smarter... but TBH, this should never get called on Tiger... */
    strlcpy(x11_path, "/Applications/Utilities/X11.app/Contents/MacOS/X11", sizeof(x11_path));
#endif
}

static int connect_to_socket(const char *filename) {
    struct sockaddr_un servaddr_un;
    struct sockaddr *servaddr;
    socklen_t servaddr_len;
    int ret_fd;

    /* Setup servaddr_un */
    memset (&servaddr_un, 0, sizeof (struct sockaddr_un));
    servaddr_un.sun_family = AF_UNIX;
    strlcpy(servaddr_un.sun_path, filename, sizeof(servaddr_un.sun_path));
    
    servaddr = (struct sockaddr *) &servaddr_un;
    servaddr_len = sizeof(struct sockaddr_un) - sizeof(servaddr_un.sun_path) + strlen(filename);
    
    ret_fd = socket(PF_UNIX, SOCK_STREAM, 0);
    if(ret_fd == -1) {
        fprintf(stderr, "Xquartz: Failed to create socket: %s - %s\n", filename, strerror(errno));
        return -1;
    }

    if(connect(ret_fd, servaddr, servaddr_len) < 0) {
        fprintf(stderr, "Xquartz: Failed to connect to socket: %s - %d - %s\n", filename, errno, strerror(errno));
        close(ret_fd);
        return -1;
    }
    
    return ret_fd;
}

static void send_fd_handoff(int connected_fd, int launchd_fd) {
    char databuf[] = "display";
    struct iovec iov[1];
    
    iov[0].iov_base = databuf;
    iov[0].iov_len  = sizeof(databuf);

    union {
        struct cmsghdr hdr;
        char bytes[CMSG_SPACE(sizeof(int))];
    } buf;
    
    struct msghdr msg;
    msg.msg_iov = iov;
    msg.msg_iovlen = 1;
    msg.msg_control = buf.bytes;
    msg.msg_controllen = sizeof(buf);
    msg.msg_name = 0;
    msg.msg_namelen = 0;
    msg.msg_flags = 0;

    struct cmsghdr *cmsg = CMSG_FIRSTHDR (&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));

    msg.msg_controllen = cmsg->cmsg_len;
    
    *((int*)CMSG_DATA(cmsg)) = launchd_fd;
    
    if(sendmsg(connected_fd, &msg, 0) < 0) {
        fprintf(stderr, "Xquartz: Error sending $DISPLAY file descriptor over fd %d: %d -- %s\n", connected_fd, errno, strerror(errno));
        return;
    }

#ifdef DEBUG
    fprintf(stderr, "Xquartz: Message sent.  Closing handoff fd.\n");
#endif

    close(connected_fd);
}

static void signal_handler(int sig) {
    if(x11app_pid)
        kill(x11app_pid, sig);
    _exit(0);
}

int main(int argc, char **argv, char **envp) {
    int envpc;
    kern_return_t kr;
    mach_port_t mp;
    string_array_t newenvp;
    string_array_t newargv;
    size_t i;
    int launchd_fd;
    string_t handoff_socket_filename;
    sig_t handler;

    if(argc == 2 && !strcmp(argv[1], "-version")) {
        fprintf(stderr, "X.org Release 7.3\n");
        fprintf(stderr, "X.Org X Server %s\n", XSERVER_VERSION);
        fprintf(stderr, "Build Date: %s\n", BUILD_DATE);
        return EXIT_SUCCESS;
    }

    if(getenv("X11_PREFS_DOMAIN"))
        server_bootstrap_name = getenv("X11_PREFS_DOMAIN");
    
    /* We don't have a mechanism in place to handle this interrupt driven
     * server-start notification, so just send the signal now, so xinit doesn't
     * time out waiting for it and will just poll for the server.
     */
    handler = signal(SIGUSR1, SIG_IGN);
    if(handler == SIG_IGN)
        kill(getppid(), SIGUSR1);
    signal(SIGUSR1, handler);

    /* Pass on SIGs to X11.app */
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    /* Get the $DISPLAY FD */
    launchd_fd = launchd_display_fd();

    kr = bootstrap_look_up(bootstrap_port, server_bootstrap_name, &mp);
    if(kr != KERN_SUCCESS) {
        set_x11_path();

        /* This forking is ugly and will be cleaned up later */
        pid_t child = fork();
        if(child == -1) {
            fprintf(stderr, "Xquartz: Could not fork: %s\n", strerror(errno));
            return EXIT_FAILURE;
        }

        if(child == 0) {
            char *_argv[3];
            _argv[0] = x11_path;
            _argv[1] = "--listenonly";
            _argv[2] = NULL;
            fprintf(stderr, "Xquartz: Starting X server: %s --listenonly\n", x11_path);
            return execvp(x11_path, _argv);
        }

        /* Try connecting for 10 seconds */
        for(i=0; i < 80; i++) {
            usleep(250000);
            kr = bootstrap_look_up(bootstrap_port, server_bootstrap_name, &mp);
            if(kr == KERN_SUCCESS)
                break;
        }

        if(kr != KERN_SUCCESS) {
#if MAC_OS_X_VERSION_MIN_REQUIRED >= 1050
            fprintf(stderr, "Xquartz: bootstrap_look_up(): %s\n", bootstrap_strerror(kr));
#else
            fprintf(stderr, "Xquartz: bootstrap_look_up(): %ul\n", (unsigned long)kr);
#endif
            return EXIT_FAILURE;
        }
    }
    
    /* Get X11.app's pid */
    request_pid(mp, &x11app_pid);

    /* Handoff the $DISPLAY FD */
    if(launchd_fd != -1) {
        size_t try, try_max;
        int handoff_fd = -1;

        for(try=0, try_max=5; try < try_max; try++) {
            if(request_fd_handoff_socket(mp, handoff_socket_filename) != KERN_SUCCESS) {
                fprintf(stderr, "Xquartz: Failed to request a socket from the server to send the $DISPLAY fd over (try %d of %d)\n", (int)try+1, (int)try_max);
                continue;
            }

            handoff_fd = connect_to_socket(handoff_socket_filename);
            if(handoff_fd == -1) {
                fprintf(stderr, "Xquartz: Failed to connect to socket (try %d of %d)\n", (int)try+1, (int)try_max);
                continue;
            }

#ifdef DEBUG
            fprintf(stderr, "Xquartz: Handoff connection established (try %d of %d) on fd %d, \"%s\".  Sending message.\n", (int)try+1, (int)try_max, handoff_fd, handoff_socket_filename);
#endif

            send_fd_handoff(handoff_fd, launchd_fd);            
            close(handoff_fd);
            break;
        }
    }

    /* Count envp */
    for(envpc=0; envp[envpc]; envpc++);
    
    /* We have fixed-size string lengths due to limitations in IPC,
     * so we need to copy our argv and envp.
     */
    newargv = (string_array_t)alloca(argc * sizeof(string_t));
    newenvp = (string_array_t)alloca(envpc * sizeof(string_t));
    
    if(!newargv || !newenvp) {
        fprintf(stderr, "Xquartz: Memory allocation failure\n");
        exit(EXIT_FAILURE);
    }
    
    for(i=0; i < argc; i++) {
        strlcpy(newargv[i], argv[i], STRING_T_SIZE);
    }
    for(i=0; i < envpc; i++) {
        strlcpy(newenvp[i], envp[i], STRING_T_SIZE);
    }

    kr = start_x11_server(mp, newargv, argc, newenvp, envpc);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "Xquartz: start_x11_server: %s\n", mach_error_string(kr));
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}