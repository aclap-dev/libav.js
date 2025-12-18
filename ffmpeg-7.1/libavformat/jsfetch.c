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
static volatile int jsfetch_aborted = 0;

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
  Module.MAX_FETCH_ATTEMPTS = 6;
  Module.MAX_READ_ATTEMPTS = 6;
  
  // Maybe set below during testing.
  Module.FETCH_TIMEOUT = Module.FETCH_TIMEOUT || 30 * 1000;
  Module.READ_TIMEOUT = Module.READ_TIMEOUT || 30 * 1000;
  Module.INITIAL_RETRY_DELAY = Module.INITIAL_RETRY_DELAY || 250;

  Module.libavjsJSFetch = { ctr: 1, fetches: {}, pos: 0, read_failures: 0 };
  Module.abortController = new AbortController();
  Module.readFailureMap = new Map();

  Module.initialized = true;
});

/**
 * Set timeouts - for testing only.
 */
EM_JS(void, jsfetch_set_fetch_timeout_js, (int ms), {
  Module.FETCH_TIMEOUT = ms;
});

void jsfetch_set_fetch_timeout(int ms) {
  return jsfetch_set_fetch_timeout_js(ms);
}

EM_JS(void, jsfetch_set_read_timeout_js, (int ms), {
  Module.READ_TIMEOUT = ms;
});

void jsfetch_set_read_timeout(int ms) {
  return jsfetch_set_read_timeout_js(ms);
}

EM_JS(void, jsfetch_set_initial_retry_delay_js, (int ms), {
  Module.INITIAL_RETRY_DELAY = ms;
});

void jsfetch_set_initial_retry_delay(int ms) {
  return jsfetch_set_initial_retry_delay_js(ms);
}

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
        abortController.abort(`Download aborted by user`);
    } else {
      // Early-abort any fetches started after this.
      Module.abortController = new AbortController();
      Module.abortController.abort(`Download aborted by user`);
    }
});

void jsfetch_abort_request(void) {
  jsfetch_abort();
  jsfetch_aborted = 1;
}

int jsfetch_already_aborted(void) {
  return jsfetch_aborted;
}

/**
 * Open a fetch connection (JavaScript side).
 * Must return an Asyncify.
 */
EM_JS(int, jsfetch_open_js, (const char* url, char* range_header, bool has_range, int force_idx, bool probe_range_support), {
  return Asyncify.handleAsync(async function() {
    if (Module.abortController.signal.aborted) {
      console.warn("jsfetch_open_js aborted.");
      return -0x54584945; /* AVERROR_EXIT*/
    }

    // Headers
    let headers = {};
    let range;
    if (has_range) {
      range = range_header ? UTF8ToString(range_header) : undefined;
      headers.Range = range;
    } else if (probe_range_support) {
      headers.Range = 'bytes=0-';
    }
    
    url = UTF8ToString(url);
    const fetchUrl = url.startsWith("jsfetch:") ? url.slice(8) : url;

    // First, probe for range support. 
    // No retries, we could get 416 (Range Not Satisfiable).
    // The server could also reject it with another code or close it if they don't support ranges.
    let response;
    if (probe_range_support) {
      response = await Module.FetchWithRetry(fetchUrl, headers, 1, Module.FETCH_TIMEOUT, Module.INITIAL_RETRY_DELAY, Module.abortController.signal);
    }

    // Not probing for range support, or the probe failed.
    if (!probe_range_support || !response.ok) {
      response = await Module.FetchWithRetry(fetchUrl, headers, Module.MAX_FETCH_ATTEMPTS, Module.FETCH_TIMEOUT, Module.INITIAL_RETRY_DELAY, Module.abortController.signal);
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
    }
    // ..Response is ok

    const accept_range = (response.headers.get("accept-ranges") || "").toLowerCase();
    const support_range = (accept_range && accept_range == "bytes") || (response.status == 206 && response.headers.has("content-range"));

    // This could be a range request, so don't overwrite.
    let content_length = 0;
    if (force_idx && Module.libavjsJSFetch.fetches[force_idx]) {
      content_length = Module.libavjsJSFetch.fetches[force_idx].content_length || 0;
    } else {
      content_length = parseInt(response.headers.get("content-length") || "0", 10);
    }

    const idx = force_idx ? force_idx : Module.libavjsJSFetch.ctr++;
    const reader = response.body.getReader();

    Module.abortController.signal.addEventListener('abort', async () => {
      try { await reader.cancel(); } catch (e) { /* */};
    });

    // The \\d here is intentional, because this goes through C first. \\d -> \d
    const pos = range ? Number(range.match(/bytes=(\\d+)/)?.[1] ?? 0) : 0;

    var jsfo = Module.libavjsJSFetch.fetches[idx] = {
      url,
      response,
      reader,
      support_range,
      content_length,
      pos,
      first_read: true,
      buf: null,
      rej: null,
    };

    return idx;
  });
});

/**
 * Check byte range support
 */
EM_JS(int, jsfetch_support_range_js, (int idx), {
  const jsfo = Module.libavjsJSFetch.fetches[idx];
  return jsfo && jsfo.support_range ? 1 : 0;
});

/**
 * Check size
 */
EM_JS(double, jsfetch_get_size_js, (int idx), {
  const jsfo = Module.libavjsJSFetch.fetches[idx];
  return jsfo ? jsfo.content_length : 0;
});

/**
 * Get curr position
 */
EM_JS(double, jsfetch_get_pos_js, (int idx), {
  const jsfo = Module.libavjsJSFetch.fetches[idx];
  return jsfo ? jsfo.pos : 0;
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

    ctx->idx = jsfetch_open_js(url, range_ptr, has_range, 0, !has_range);

    // Seek if range header is supported.
    int support = jsfetch_support_range_js(ctx->idx);
    h->is_streamed = (bool) !support;

    return (ctx->idx > 0) ? 0 : ctx->idx;
}

/**
 * Read from a fetch connection (JavaScript side).
 */
EM_JS(int, jsfetch_read_js, (int idx, unsigned char *toBuf, int size), {
    return Asyncify.handleAsync(async function() {
      const self = async function() {
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
            jsfo.pos += len;
            return len;
          }
          
          // We may abort, timeout, or read successfully.
          const timeout_p = Module.DoAbortableSleep(Module.READ_TIMEOUT, Module.abortController.signal);
          const read_p = jsfo.reader.read();
          const race_res = await Promise.race([timeout_p, read_p]);
          
          // Aborted / Timed out
          if (race_res == "aborted") {
            return -0x54584945; /* AVERROR_EXIT*/
          } else if (race_res == "timed_out") {
            await jsfo.reader.cancel();
            throw `Timed out after ${Module.READ_TIMEOUT} ms.`;
          }

          if (race_res.done) {
            return -0x20464f45 /* AVERROR_EOF */;
          }

          // Reset read failures
          Module.libavjsJSFetch.read_failures = 0;

          let chunk = race_res.value;

          // Skip
          const skip_bytes = Module.readFailureMap.get(idx);
          if (skip_bytes > jsfo.pos) {
            const bytes_to_skip = skip_bytes - jsfo.pos;
            if (bytes_to_skip >= chunk.length) {
              // Entire chunk is before our skip point - discard and fetch next
              jsfo.pos += chunk.length;
              return self();
            } else {
              // Partial skip
              chunk = chunk.subarray(bytes_to_skip);
              jsfo.pos += bytes_to_skip;
              Module.readFailureMap.delete(idx);
            }
          }
        
          // Check for PNG in the first read only.
          // Have to assume/hope the PNG is not split across reads.
          // Otherwise this won't work.
          if (jsfo.first_read) {
            jsfo.first_read = false;
            const png_index = Module.FindPngSliceIndex(chunk);
            if (png_index >= 0) {
              chunk = chunk.subarray(png_index);
              jsfo.pos += png_index;
            }
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
          jsfo.pos += len;

          return len;
        } catch (e) {
          if (Module.abortController.signal.aborted) {
            console.warn("jsfetch_read_js aborted.");
            return -0x54584945; /* AVERROR_EXIT*/
          }

          console.warn("jsfetch_read_js error", e);
          Module.libavjsJSFetch.read_failures++;

          // Cannot retry
          const too_big_to_retry_without_seek = jsfo.content_length > 41943040; // 40 MB
          if (Module.libavjsJSFetch.read_failures >= Module.MAX_READ_ATTEMPTS || (!jsfo.support_range && too_big_to_retry_without_seek)) {
            console.warn(`Cant retry. read_failures: ${Module.libavjsJSFetch.read_failures}, too_big: ${too_big_to_retry_without_seek}`);
            Module.returnCode = 1002; /* Read Error*/
            
            return -0x20464f45 /* AVERROR_EOF */;
          }

          const delay = Math.pow(2, Module.libavjsJSFetch.read_failures) * Module.INITIAL_RETRY_DELAY;
          console.warn(`Retrying read in ${delay} ms`);
          const abort_or_timeout = await Module.DoAbortableSleep(delay, Module.abortController.signal);
          if (abort_or_timeout == "aborted") {
            return -0x54584945; /* AVERROR_EXIT*/
          }

          // Retry without range support
          if (!jsfo.support_range) {
            console.warn(`Retrying without range support, will skip ${jsfo.pos} bytes`);
            const existing = Module.readFailureMap.get(idx) || 0;
            const skip_bytes = Math.max(existing, jsfo.pos);
            Module.readFailureMap.set(idx, skip_bytes);
            return -1001; // Signal retry without seek in jsfetch_read()
          }

          // Retry with range support
          console.warn(`Retrying with range support from ${jsfo.pos}`);
          return -1000; // Signals retry in jsfetch_read()
        }
      };
    return self();
  });
});

/**
 * Close a fetch connection (JavaScript side).
 */
EM_JS(void, jsfetch_close_js, (int idx), {
  var jsfo = Module.libavjsJSFetch.fetches[idx];
  if (jsfo) {
    jsfo.reader.cancel().catch((e) => { /* */});

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
      av_log(h, AV_LOG_ERROR, "[jsfetch] Seek was called but not supported.\n");
      return AVERROR(ENOSYS); 
    }

    // AVSEEK_SIZE - should return the size
    int64_t size = (double) jsfetch_get_size_js(ctx->idx);
    if (whence == 0x10000) {
      return size ? size : -38;// AVERROR(ENOSYS)
    }

    // SEEK_CUR: Seek to curr + pos. Very unlikely.
    if (whence == 1) {
      double curr_pos = jsfetch_get_pos_js(ctx->idx);
      pos += (int64_t) curr_pos;
    }

    // SEEK_END: Seek to end of stream + pos.
    // Usually pos is -1.
    if (whence == 2) {
      double size = jsfetch_get_size_js(ctx->idx);
      pos += (int64_t) size;
    }

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
    // NOTE: pos is set in jsfetch_open_js. 
    // If SEEK_CUR is then called, jsfetch_get_pos_js works.
    ret = jsfetch_open_js(h->filename, range_header, has_range, ctx->idx, false);
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
    if (ret != -1000 && ret != -1001) {
     return ret;
    }
    av_log(h, AV_LOG_ERROR, "[jsfetch] jsfetch_read retrying mode: %d\n", ret);

    /* Retry with close/reopen */
    if (ret == -1001) {
      ret = jsfetch_close(h);
       if (ret < 0) {
        av_log(h, AV_LOG_ERROR, "[jsfetch] jsfetch_close failed with error %d\n", ret);
        return ret;
      }
      // Force the same idx
      ret = jsfetch_open_js(h->filename, NULL, false, ctx->idx, false);
      if (ret < 0) {
        return ret;
      }
      return jsfetch_read(h, buf, size);
    }

    /* Retry via seek */
    // Get pos
    int64_t pos = (int64_t) jsfetch_get_pos_js(ctx->idx);

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
