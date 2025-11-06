/*
 * Copyright (C) 2019-2024 Yahweasel and contributors
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

// Import LibAV.base if applicable
let _scriptName;
if (typeof _scriptName === "undefined") {
    if (typeof LibAV === "object" && LibAV && LibAV.base)
        _scriptName = LibAV.base + "/libav-@VER-@VARIANT.@DBG@TARGET.@JS";
    else if (typeof self === "object" && self && self.location)
        _scriptName = self.location.href;
}

Module.printErr = console.log.bind(console);

Module.locateFile = function(path, prefix) {
    // if it's the wasm file
    if (path.lastIndexOf(".wasm") === path.length - 5 &&
        path.indexOf("libav-") !== -1) {
        // Look for overrides
        if (Module.wasmurl)
            return Module.wasmurl;
        if (Module.variant)
            return prefix + "libav-@VER-" + Module.variant + ".@DBG@TARGET.wasm";
    }

    // Otherwise, use the default
    return prefix + path;
}

/* Appended PreJS: */
async function DoAbortableSleep(sleep_ms,signal){return await new Promise(resolve=>{if(signal.aborted){resolve();return}let abortHandler=()=>{clearTimeout(timeout_id),signal.removeEventListener("abort",abortHandler),resolve()};signal.addEventListener("abort",abortHandler);let timeout_id=setTimeout(()=>{signal.removeEventListener("abort",abortHandler),resolve()},sleep_ms)}),signal.aborted?"aborted":"timed_out"}async function FetchWithRetry(url,headers,max_attempts,timeout_ms,signal){let last_error;for(let i=0;i<max_attempts;++i){let timeout_controller=new AbortController,abort_or_timeout_signal=AbortSignal.any([timeout_controller.signal,signal]),timeout_id=setTimeout(()=>timeout_controller.abort(),timeout_ms);try{let response=await fetch(url,{headers,signal:abort_or_timeout_signal});if(response.ok)return response;if(console.warn(`HTTP Error: ${response.status}`),response.status==404)return{err_status:404};last_error={err_status:response.status}}catch(error){if(console.warn("Caught error",error),error instanceof DOMException&&error.name=="AbortError")return{aborted:!0};error instanceof DOMException&&error.name=="TimeoutError"?last_error={timeout:!0}:last_error=error}finally{clearTimeout(timeout_id)}let delay=Math.pow(2,i)*250;if(console.warn(`Retrying in ${delay} ms`),await Module.DoAbortableSleep(delay,signal)=="aborted")return{aborted:!0}}return console.warn(`Failed fetching after ${max_attempts}. Error:`,last_error),last_error}function FindPngSliceIndex(data){let png_header=[137,80,78,71,13,10,26,10],iend_chunk=[73,69,78,68,174,66,96,130];for(let i=0;i<png_header.length;i++)if(data[i]!==png_header[i])return-1;let seq_len=iend_chunk.length,data_len=data.length;if(seq_len===0||seq_len>data_len)return-1;let first_byte=iend_chunk[0];for(let i=png_header.length;i<=data_len-seq_len;i++){if(data[i]!==first_byte)continue;let match=!0;for(let j=1;j<seq_len;j++)if(data[i+j]!==iend_chunk[j]){match=!1;break}if(match)return i+iend_chunk.length}return-1}Module.DoAbortableSleep=DoAbortableSleep;Module.FindPngSliceIndex=FindPngSliceIndex;Module.FetchWithRetry=FetchWithRetry;
