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

/**
 * Initialize functions.
 * See pre.js for other initialized functions.
 */
EM_JS(void, jsfetch_init, (), {
  if (Module.initialized) {
    return;
  }
  Module.MAX_FETCH_ATTEMPTS = 5;
  Module.MAX_READ_ATTEMPTS = 5;
  Module.FETCH_TIMEOUT = 30 * 1000;
  Module.READ_TIMEOUT = 30 * 1000;

  Module.libavjsJSFetch = { ctr: 1, fetches: {}, pos: 0, read_failures: 0 };
  Module.abortController = new AbortController();
  Module.initialized = true;
});

/**
 * Get return code
 * 
 * 100-600:  Reserved for HTTP (i.e 403, 500)
 * 1001:     Network Error
 * 1002:     Read Error
 * 1003:     Fetch timeout
 */
EM_JS(int, jsfetch_get_code, (), {
  return Module.returnCode || 0;
});

int jsfetch_get_return_code() {
  return jsfetch_get_code();
}

EM_JS(void, jsfetch_abort, (), {
    var abortController = Module.abortController;
    if (abortController) {
        abortController.abort();
    } else {
      // Early-abort any fetches started after this.
      Module.abortController = new AbortController();
      Module.abortController.abort();
    }
});

void jsfetch_abort_request(void) {
  jsfetch_abort();
}

/**
 * Open a fetch connection (JavaScript side).
 * Must return an Asyncify.
 */
EM_JS(int, jsfetch_open_js, (const char* url, char* range_header, bool has_range, int force_idx, bool enable_retries), {
  return Asyncify.handleAsync(async function() {
    if (Module.abortController.signal.aborted) {
      console.warn("jsfetch_open_js aborted.");
      return -0x54584945; /* AVERROR_EXIT*/
    }    

    // Headers
    let headers = {};
    if (has_range) {
      const range = range_header ? UTF8ToString(range_header) : undefined;
      headers.Range = range;
    }

    url = UTF8ToString(url);
    const fetchUrl = url.startsWith("jsfetch:") ? url.slice(8) : url;
    const attempts = enable_retries ? Module.MAX_FETCH_ATTEMPTS : 1;
    const response = await FetchWithRetry(fetchUrl, headers, attempts, Module.FETCH_TIMEOUT, Module.abortController.signal);
    if (response.aborted) {
      return -0x54584945; /* AVERROR_EXIT*/
    } else if (response.timeout) {
      Module.returnCode = 1003; /* Fetch timeout*/
      // Should return a partial file if we've downloaded anything so far.
      return -0x20464f45 /* AVERROR_EOF */;
    } else if (response instanceof Error) {
      Module.returnCode = 1001; /* Network Error*/
      return -0x20464f45 /* AVERROR_EOF */;
    } else if (response.err_status) {
      Module.returnCode = response.err_status;
      return -0x20464f45 /* AVERROR_EOF */;
    }
    // ..Response is ok

    const accept_range = (response.headers.get("accept-ranges") || "").toLowerCase();
    const support_range = accept_range && accept_range == "bytes";
    const content_length = parseInt(response.headers.get("content-length") || "0", 10);

    if (!Module.libavjsJSFetch.support_range) {
      Module.libavjsJSFetch.support_range = support_range;
    }

    // This could be a range request, so don't overwrite.
    if (!Module.libavjsJSFetch.content_length) {
      Module.libavjsJSFetch.content_length = content_length;
    }

    const idx = force_idx ? force_idx : Module.libavjsJSFetch.ctr++;
    const reader = response.body.getReader();
    Module.abortController.signal.addEventListener('abort', () => {
      reader.cancel();
    });

    var jsfo = Module.libavjsJSFetch.fetches[idx] = {
      url,
      response,
      reader,
      first_read: true,
      enable_retries,
      buf: null,
      rej: null,
    };
    return idx;
  });
});

/**
 * Check byte range support
 */
EM_JS(int, jsfetch_support_range_js, (), {
  var ret = Module.libavjsJSFetch.support_range ? 1 : 0;
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
  Module.libavjsJSFetch.pos = pos;
});

/**
 * Open a fetch connection.
 */
static int jsfetch_open(URLContext *h, const char *url, int flags, AVDictionary **options)
{
    jsfetch_init();

    JSFetchContext *ctx = h->priv_data;

    AVDictionaryEntry *entry = av_dict_get(*options, "range_header", NULL, 0);
    const char *range_ptr = entry ? entry->value : NULL;
    bool has_range = range_ptr != NULL;

    // Custom values used by us in jsfetch to prevent unnecessary seeking & retries.
    entry = av_dict_get(*options, "jsfetch_skip_seek", NULL, 0);
    bool skip_seek = entry != NULL;
    entry = av_dict_get(*options, "jsfetch_skip_retry", NULL, 0);
    bool skip_retries = entry != NULL;

    ctx->idx = jsfetch_open_js(url, range_ptr, has_range, 0, !skip_retries);

   if (skip_seek || has_range) {
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
    return Asyncify.handleAsync(async function() {
      var jsfo = Module.libavjsJSFetch.fetches[idx];
      if (Module.abortController.signal.aborted || !jsfo) {
        console.warn("jsfetch_read_js aborted.");
        return -0x54584945; /* AVERROR_EXIT*/
      }
      try {
        // Check for remainder
        if (jsfo.buf && jsfo.buf.value && jsfo.buf.value.length > 0) {
          const chunk = jsfo.buf.value;
          const len = Math.min(size, chunk.length);

          Module.HEAPU8.set(chunk.subarray(0, len), toBuf);
          jsfo.buf.value = chunk.subarray(len);
          if (jsfo.buf.value.length === 0) {
            jsfo.buf = null;
          }
          Module.libavjsJSFetch.pos += len;
          return len;
        }
        
        let timeout_p = DoAbortableSleep(Module.READ_TIMEOUT, Module.abortController.signal);
        let read_p = jsfo.reader.read();
        const race_res = await Promise.race([timeout_p, read_p]);
        
        // Aborted / Timed out
        if (race_res == "aborted") {
          return -0x54584945; /* AVERROR_EXIT*/
        } else if (race_res == "timed_out") {
          jsfo.reader.cancel();
          throw `Timed out after ${Module.READ_TIMEOUT} seconds.`;
        }

        if (race_res.done) {
          return -0x20464f45;
        }

        // Reset read failures
        Module.libavjsJSFetch.read_failures = 0;

        let chunk = race_res.value;
        // Check for PNG in the first read only.
        // Have to assume/hope the PNG is not split across reads.
        // Otherwise this won't work.
        if (jsfo.first_read) {
          let png_index = Module.FindPngSliceIndex(chunk);
          if (png_index >= 0) {
            chunk = chunk.subarray(png_index, chunk.length);
          }
          jsfo.first_read = false;
        }
        const len = Math.min(size, chunk.length);
        Module.HEAPU8.set(chunk.subarray(0, len), toBuf);

        // Save the remainder
        if (chunk.length > len) {
          jsfo.buf = {
            value: chunk.subarray(len)
          };
        } else {
          jsfo.buf = null;
        }
        Module.libavjsJSFetch.pos += len;

        return len;
      } catch (e) {
        // Cannot retry
        console.error("jsfetch_read_js error", e);
        if (!Module.libavjsJSFetch.support_range || !jsfo.enable_retries || Module.libavjsJSFetch.read_failures >= Module.MAX_READ_ATTEMPTS) {
          Module.returnCode = 1002; /* Read Error*/
          Module.fsThrownError = e;
          return -11;  /* ECANCELED */;
        }

        // Signal to retry
        const delay = Math.pow(2, Module.libavjsJSFetch.read_failures) * 250;
        console.warn(`Retrying read in ${delay} ms`);
        const abort_or_timeout = await Module.DoAbortableSleep(delay, Module.abortController.signal);
        if (abort_or_timeout == "aborted") {
          return -0x54584945; /* AVERROR_EXIT*/
        }
        Module.libavjsJSFetch.read_failures++;
        return -1000;
      }
  });
});

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
    ret = jsfetch_open_js(h->filename, range_header, has_range, ctx->idx, true);
    if (ret < 0) {
      return ret;
    }

    return pos;
}


/**
 * Read from a fetch connection.
 */
static int jsfetch_read(URLContext *h, unsigned char *buf, int size)
{
    JSFetchContext *ctx = h->priv_data;
    int ret = jsfetch_read_js(ctx->idx, buf, size);
    // If retries are not possible / needed.
    if (ret != -1000) {
     return ret;
    }

    /* Retry via seek */
    // Get pos
    double curr_pos = jsfetch_get_pos_js();
    int64_t pos = (int64_t) curr_pos;

    // Seek
    ret = jsfetch_seek(h, pos, 0);
    if (ret < 0) {
     return ret;
    }

    return jsfetch_read(h, buf, size);
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
