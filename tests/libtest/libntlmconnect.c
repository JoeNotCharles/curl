/***************************************************************************
 *                                  _   _ ____  _
 *  Project                     ___| | | |  _ \| |
 *                             / __| | | | |_) | |
 *                            | (__| |_| |  _ <| |___
 *                             \___|\___/|_| \_\_____|
 *
 * Copyright (C) 1998 - 2011, Daniel Stenberg, <daniel@haxx.se>, et al.
 *
 * This software is licensed as described in the file COPYING, which
 * you should have received as part of this distribution. The terms
 * are also available at http://curl.haxx.se/docs/copyright.html.
 *
 * You may opt to use, copy, modify, merge, publish, distribute and/or sell
 * copies of the Software, and permit persons to whom the Software is
 * furnished to do so, under the terms of the COPYING file.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ***************************************************************************/
#include "test.h"

#include "testutil.h"
#include "memdebug.h"

#define TEST_HANG_TIMEOUT 5 * 1000
#define MAX_EASY_HANDLES 3 

CURL *easy[MAX_EASY_HANDLES];
curl_socket_t sockets[MAX_EASY_HANDLES];
int res = 0;

static size_t callback(char* ptr, size_t size, size_t nmemb, void* data)
{
  int idx = ((CURL **) data) - easy;
  curl_socket_t sock;

  fprintf(stdout, ptr);

  res = curl_easy_getinfo(easy[idx], CURLINFO_LASTSOCKET, &sock);
  if (CURLE_OK != res) {
    fprintf(stderr, "Error reading CURLINFO_LASTSOCKET\n");
    return 0;
  }
  /* sock will only be set for NTLM requests; for others it is -1 */
  if (sock != -1 && sock != sockets[idx]) {
    fprintf(stderr, "Handle %d started on socket %d and moved to %d\n", idx, sockets[idx], sock);
    res = TEST_ERR_MAJOR_BAD;
    return 0;
  }
  return size * nmemb;
}

enum HandleState {
  ReadyForNewHandle,
  NeedSocketForNewHandle,
  NoMoreHandles
};

int test(char *url)
{
  CURLM *multi = NULL;
  int running;
  int i, j;
  int num_handles = 0;
  enum HandleState state = ReadyForNewHandle;
  char* full_url = malloc(strlen(url) + 4 + 1);

  start_test_timing();

  if (!full_url) {
    fprintf(stderr, "Not enough memory for full url\n");
    return CURLE_OUT_OF_MEMORY;
  }

  for (i = 0; i < MAX_EASY_HANDLES; ++i) {
    easy[i] = NULL;
    sockets[i] = -1;
  }

  res = 0;
  res_global_init(CURL_GLOBAL_ALL);
  if(res) {
    return res;
  }

  multi_init(multi);

  for(;;) {
    struct timeval interval;
    fd_set fdread;
    fd_set fdwrite;
    fd_set fdexcep;
    long timeout = -99;
    curl_socket_t maxfd = -99;
    bool found_new_socket = FALSE;

    /* Start a new handle if we aren't at the max */
    if (state == ReadyForNewHandle) {
      easy_init(easy[num_handles]);

      if (num_handles % 3 == 2) {
        sprintf(full_url, "%s0200", url);
        easy_setopt(easy[num_handles], CURLOPT_HTTPAUTH, CURLAUTH_NTLM);
      } else {
        sprintf(full_url, "%s0100", url);
        easy_setopt(easy[num_handles], CURLOPT_HTTPAUTH, CURLAUTH_BASIC);
      }
      easy_setopt(easy[num_handles], CURLOPT_FRESH_CONNECT, 1L);
      easy_setopt(easy[num_handles], CURLOPT_URL, full_url);
      easy_setopt(easy[num_handles], CURLOPT_VERBOSE, 1L);
      easy_setopt(easy[num_handles], CURLOPT_HTTPGET, 1L);
      easy_setopt(easy[num_handles], CURLOPT_USERPWD, "testuser:testpass");
      easy_setopt(easy[num_handles], CURLOPT_WRITEFUNCTION, callback);
      easy_setopt(easy[num_handles], CURLOPT_WRITEDATA, easy + num_handles);
      easy_setopt(easy[num_handles], CURLOPT_HEADER, 1L);

      multi_add_handle(multi, easy[num_handles]);
      num_handles += 1;
      state = NeedSocketForNewHandle;
    }

    multi_perform(multi, &running);
    if (0 != res)
      break;

    abort_on_test_timeout();

    if(!running)
      break; /* done */

    FD_ZERO(&fdread);
    FD_ZERO(&fdwrite);
    FD_ZERO(&fdexcep);

    multi_fdset(multi, &fdread, &fdwrite, &fdexcep, &maxfd);

    /* At this point, maxfd is guaranteed to be greater or equal than -1. */

    /* Any socket which is new in fdread is associated with the new handle */
    for (i = 0; i <= maxfd; ++i) {
      bool socket_exists = FALSE;
      if (!FD_ISSET(i, &fdread)) {
        continue;
      }
      for (j = 0; j < num_handles; ++j) {
        if (sockets[j] == i) {
          socket_exists = TRUE;
          break;
        }
      }
      if (socket_exists) {
        continue;
      }

      if (found_new_socket || state != NeedSocketForNewHandle) {
        fprintf(stderr, "Unexpected new socket\n");
        res = TEST_ERR_MAJOR_BAD;
        goto test_cleanup;
      }

      sockets[num_handles-1] = i;
      found_new_socket = TRUE;
      /* continue to make sure there's only one new handle */
    }

    if (state == NeedSocketForNewHandle) {
      if (!found_new_socket) {
        fprintf(stderr, "Socket did not open immediately for new handle\n");
        res = TEST_ERR_MAJOR_BAD;
        goto test_cleanup;
      }
      state = num_handles < MAX_EASY_HANDLES ? ReadyForNewHandle
                                             : NoMoreHandles;
    }

    multi_timeout(multi, &timeout);

    /* At this point, timeout is guaranteed to be greater or equal than -1. */

    if(timeout != -1L) {
      interval.tv_sec = timeout/1000;
      interval.tv_usec = (timeout%1000)*1000;
    }
    else {
      interval.tv_sec = TEST_HANG_TIMEOUT/1000+1;
      interval.tv_usec = 0;
    }

    select_test(maxfd+1, &fdread, &fdwrite, &fdexcep, &interval);

    abort_on_test_timeout();
  }

test_cleanup:

  for (i = 0; i < MAX_EASY_HANDLES; ++i) {
    if (easy[i]) {
       if (multi) {
         curl_multi_remove_handle(multi, easy[i]);
       }
       curl_easy_cleanup(easy[i]);
    }
  }

  if (multi) {
    curl_multi_cleanup(multi);
  }

  curl_global_cleanup();

  if (full_url) {
    free(full_url);
  }

  return res;
}
