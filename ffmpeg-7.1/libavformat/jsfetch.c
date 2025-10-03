/*
 * JavaScript fetch metaprotocol for ffmpeg client
 * Copyright (c) 2023 Yahweasel and contributors
 *
 * This file is part of FFmpeg in libav.js. The following license applies only
 * to this file.
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
 * SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION
 * OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "config.h"
#include "config_components.h"

#include "libavutil/error.h"
#include "libavutil/opt.h"

#include "url.h"
#include "jsfetch.h"

#include <emscripten.h>
#include <errno.h>

typedef struct JSFetchContext {
    const AVClass *class;
    // All of the real information is stored in a JavaScript structure
    int idx;
} JSFetchContext;

static const AVOption options[] = {
    { NULL }
};
#define CONFIG_JSFETCH_PROTOCOL 1
#if CONFIG_JSFETCH_PROTOCOL
static const AVClass jsfetch_context_class = {
    .class_name = "jsfetch",
    .item_name = av_default_item_name,
    .option = options,
    .version = LIBAVUTIL_VERSION_INT
};

EM_JS(void, jsfetch_abort, (), {
    var abortController = Module.libavjsJSFetch.abortController;
    if (abortController) {
        abortController.abort();
    }
});

void jsfetch_abort_request(void) {
  jsfetch_abort();
}

/**
 * Open a fetch connection (JavaScript side).
 */
EM_JS(int, jsfetch_open_js, (const char* url, char* range_header, bool has_range, int force_idx), {
    return Asyncify.handleAsync(function() {
      if (!Module.libavjsJSFetch)
        Module.libavjsJSFetch = {ctr: 1, fetches: {}, abortController: new AbortController()};
      return Promise.all([]).then(function() {
        url = UTF8ToString(url);
        var headers = {};
        if (has_range) {
          var range = range_header ? UTF8ToString(range_header) : undefined;
          headers.Range = range;
        }
        var fetchUrl = url.startsWith("jsfetch:") ? url.slice(8) : url;
        
        // Retry function with exponential backoff
        function fetchWithRetry(retryCount) {
          return fetch(fetchUrl, { headers, signal: Module.libavjsJSFetch.abortController.signal }).then(function(response) {
            // Check for HTTP errors (4xx/5xx status codes)
            if (!response.ok) {
              var error = new Error('HTTP Error: ' + response.status + ' ' + response.statusText);
              error.status = response.status;
              error.response = response;
              throw error;
            }
            return response;
          }).catch(function(error) {
            // No retry
            if (error instanceof DOMException && error.name == 'AbortError') {
              return error;
            }

            console.warn("Caught error", error);
            var shouldRetry = error.status && (error.status >= 500 || error.status == 429 || error.status == 408);

            // Retry for all exceptions, and 5xx.
            if (shouldRetry && retryCount < 5) {
              console.warn('Fetch attempt ' + (retryCount + 1) + ' failed for ' + fetchUrl + 
                          ', retrying in ' + Math.pow(2, retryCount) * 250 + 'ms...', error);
              
              // Exponential backoff: [250ms -> 4s]
              var delay = Math.pow(2, retryCount) * 250;
              return new Promise(function(resolve) {
                let timeoutId = setTimeout(resolve, delay);
                Module.libavjsJSFetch.abortController.signal.addEventListener('abort', () => {
                  clearTimeout(timeoutId);
                  resolve();
                  error.aborted = true
                });

              }).then(function() {
                if (error.aborted) {
                  return error;
                } else {
                  return fetchWithRetry(retryCount + 1);
                }
              });
            }
            
            // For dash, ffmpeg keeps going past the last segment for some reason
            // that happens even on ffmpeg-cli. It will be a 404.
            // For other errors, lets try to return a partial download.
            return error;
          });
        }
        
        return fetchWithRetry(0);
      }).then(function(response) {
        if (response.name == 'AbortError' || response.aborted) {
          return -0x54584945; /* AVERROR_EXIT*/
        }
        if (response instanceof Error) {
          // Should return a partial file if we've downloaded anything so far.
          return -0x20464f45 /* AVERROR_EOF */;
        }
        var jsf = Module.libavjsJSFetch;

        const accept_range = (response.headers.get("accept-ranges") || "").toLowerCase();
        const support_range = accept_range && accept_range == "bytes";
        const content_length = parseInt(response.headers.get("content-length") || "0", 10);

        if (!jsf.support_range) {
          jsf.support_range = support_range;
        }

        // This could be a range request, so don't overwrite.
        if (!jsf.content_length) {
          jsf.content_length = content_length;
        }

        var idx = force_idx ? force_idx : jsf.ctr++;
        var reader = response.body.getReader();
        var jsfo = jsf.fetches[idx] = {
          url: url,
          response: response,
          reader: reader,
          next: reader.read().then(function(res) {
            jsfo.buf = res;
          }).catch(function(rej) {
            jsfo.rej = rej;
          }),
          buf: null,
          rej: null
        };
        return idx;
      }).catch(function(ex) {
        Module.fsThrownError = ex;
        console.error('Final fetch error after retries:', ex);
        return -6 /* EAGAIN */;
      });
    });
   });

/**
 * Check byte range support
 */
EM_JS(int, jsfetch_support_range_js, (), {
  var jsf = Module.libavjsJSFetch;
  var ret = jsf.support_range ? 1 : 0;
  return ret;
});

/**
 * Check size
 */
EM_JS(double, jsfetch_get_size_js, (), {
  var jsf = Module.libavjsJSFetch;
  var size = jsf.content_length ? jsf.content_length : 0;

  return size;
});

/**
 * Get curr position
 */
EM_JS(double, jsfetch_get_pos_js, (), {
  var jsf = Module.libavjsJSFetch;
  var pos = jsf.pos ? jsf.pos : 0;
  return pos;
});

/**
 * Set curr position
 */
EM_JS(void, jsfetch_set_pos_js, (double pos), {
  var jsf = Module.libavjsJSFetch;
  jsf.pos = pos;
});

/**
 * Open a fetch connection.
 */
static int jsfetch_open(URLContext *h, const char *url, int flags, AVDictionary **options)
{
    JSFetchContext *ctx = h->priv_data;

    AVDictionaryEntry *entry = av_dict_get(*options, "range_header", NULL, 0);
    const char *range_ptr = entry ? entry->value : NULL;
    bool has_range = range_ptr != NULL;
    ctx->idx = jsfetch_open_js(url, range_ptr, has_range, 0);

    // Check for our special header to indicate this is M3U8/DASH and should not seek.
    // Those formats can seek within the playlist, but not within an individual segment.
    entry = av_dict_get(*options, "skip_seek", NULL, 0);

   if (entry || has_range) {
       // Don't seek.
       h->is_streamed = 1;
   } else {
       // Seek if range header is supported.
       int support = jsfetch_support_range_js();
       h->is_streamed = (bool) !support;
   }

    return (ctx->idx > 0) ? 0 : ctx->idx;
}

/**
 * Read from a fetch connection (JavaScript side).
 */
EM_JS(int, jsfetch_read_js, (int idx, unsigned char *toBuf, int size), {
    var jsfo = Module.libavjsJSFetch.fetches[idx];
    if (!jsfo) {
      console.warn("Null jsfo. Probably aborted.");
      return -0x54584945; /* AVERROR_EXIT*/
    }
    return Asyncify.handleAsync(function() { return Promise.all([]).then(function() {
        if (jsfo.buf || jsfo.rej) {
            // Already have data
            var fromBuf = jsfo.buf;
            var rej = jsfo.rej;

            if (fromBuf) {
                if (fromBuf.done) {
                    // EOF
                    return -0x20464f45 /* AVERROR_EOF */;
                }
                if (fromBuf.value.length > size) {
                    // Return some of the buffer
                    Module.HEAPU8.set(fromBuf.value.subarray(0, size), toBuf);
                    fromBuf.value = fromBuf.value.subarray(size);
                    return size;
                }

                /* Otherwise, return the remainder of the buffer and start
                 * the next read */
                var ret = fromBuf.value.length;
                Module.HEAPU8.set(fromBuf.value, toBuf);
                jsfo.buf = jsfo.rej = null;
                jsfo.next = jsfo.reader.read().then(function(res) {
                    jsfo.buf = res;
                }).catch(function(rej) {
                    jsfo.rej = rej;
                });
                return ret;
            }

            if (rej.name == 'AbortError') {
              return -0x54584945; /* AVERROR_EXIT*/
            }

            // Otherwise, there was an error
            Module.fsThrownError = rej;
            console.error(rej);
            return -11 /* ECANCELED */;
        }

        // The next data isn't available yet. Force them to wait.
        return Promise.race([
            jsfo.next,
            new Promise(function(res) { setTimeout(res, 100); })
        ]).then(function() { return -6 /* EAGAIN */; });
    }); });
});

/**
 * Read from a fetch connection.
 */
static int jsfetch_read(URLContext *h, unsigned char *buf, int size)
{
    JSFetchContext *ctx = h->priv_data;
    return jsfetch_read_js(ctx->idx, buf, size);
}

/**
 * Close a fetch connection (JavaScript side).
 */
EM_JS(void, jsfetch_close_js, (int idx), {
    var jsfo = Module.libavjsJSFetch.fetches[idx];
    if (jsfo) {
        try { jsfo.reader.cancel(); } catch (ex) {}
        delete Module.libavjsJSFetch.fetches[idx];
    }
});

/**
 * Close a fetch connection.
 */
static int jsfetch_close(URLContext *h)
{
    JSFetchContext *ctx = h->priv_data;
    jsfetch_close_js(ctx->idx);
    return 0;
}

// Note: Use double for size/pos. Otherwise we can overflow.
// Works up to 2^53 bytes ~ 9PB, so should be fine.
// EM_JS can't handle int64_t without the -sWASM_BIGINT build flag. 
// Building with it might mess up ffmpeg function bindings that do int64_t => {lo, hi} conversion.
// https://emscripten.org/docs/getting_started/FAQ.html#how-do-i-pass-int64-t-and-uint64-t-values-from-js-into-wasm-functions
static int64_t jsfetch_seek(URLContext *h, int64_t pos, int whence)
{
    av_log(h, AV_LOG_DEBUG, "[jsfetch] Seeking to pos=%" PRId64 ", whence=%d, url=%s\n",
           pos, whence, h->filename);

    JSFetchContext *ctx = h->priv_data;

    if (h->is_streamed) {
         return AVERROR(ENOSYS); 
    }
     // AVSEEK_SIZE - should return the size
    int64_t size = (double) jsfetch_get_size_js();
    if (whence == 0x10000) {
      return size ? size : -38;// AVERROR(ENOSYS)
    }

    // SEEK_CUR: Seek to curr + pos. Very unlikely.
    if (whence == 1) {
      double curr_pos = jsfetch_get_pos_js();
      pos += (int64_t) curr_pos;
    }

    // SEEK_END: Seek to end of stream + pos.
    // Usually pos is -1.
    if (whence == 2) {
      double size = jsfetch_get_size_js();
      pos += (int64_t) size;
    }

    // In case SEEK_CUR is called later.
    jsfetch_set_pos_js((double) pos);

    int ret = jsfetch_close(h);
    if (ret < 0) {
        av_log(h, AV_LOG_ERROR, "[jsfetch] jsfetch_close failed with error %d\n", ret);
        return ret;
    }
    int has_range = (pos >= 0);
    char range_header_buf[128];
    char *range_header = NULL;
    if (has_range) {
        // "bytes=<start>-"
        snprintf(range_header_buf, sizeof(range_header_buf), "bytes=%" PRId64 "-", pos);
        range_header = range_header_buf;
    }

    // Force the same idx
    ret = jsfetch_open_js(h->filename, range_header, has_range, ctx->idx);
    if (ret < 0) {
      return ret;
    }

    return pos;
}

const URLProtocol ff_jsfetch_protocol = {
    .name               = "jsfetch",
    .url_open2          = jsfetch_open,
    .url_read           = jsfetch_read,
    .url_close          = jsfetch_close,
    .url_seek           = jsfetch_seek,
    .priv_data_size     = sizeof(JSFetchContext),
    .priv_data_class    = &jsfetch_context_class,
    .flags              = URL_PROTOCOL_FLAG_NETWORK,
    .default_whitelist  = "jsfetch,http,https,crypto"
};
#endif
